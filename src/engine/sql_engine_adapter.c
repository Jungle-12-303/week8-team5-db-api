/*
 * engine/sql_engine_adapter.c
 *
 * 이 파일은 8주차 API 서버와 기존 SQL 엔진 사이를 잇는 연결부다.
 *
 * 핵심 역할:
 * - API에서 받은 SQL 문자열을 lexer/parser/executor로 연결한다.
 * - 실행 전 schema를 읽어 canonical table key를 확정한다.
 * - 같은 물리 테이블 요청을 table-level lock으로 직렬화한다.
 * - executor의 FILE* 출력 결과를 문자열로 캡처한다.
 * - 내부 엔진 오류를 API용 오류 코드로 분류한다.
 *
 * 초심자 관점에서는 "HTTP 세계의 문자열 요청을 기존 DB 엔진 실행으로 바꾸는 어댑터"라고 보면 된다.
 */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#endif

#include "sqlparser/engine/sql_engine_adapter.h"

#include "sqlparser/common/util.h"
#include "sqlparser/execution/executor.h"
#include "sqlparser/sql/ast.h"
#include "sqlparser/sql/lexer.h"
#include "sqlparser/sql/parser.h"
#include "sqlparser/storage/schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static pthread_mutex_t test_hook_mutex = PTHREAD_MUTEX_INITIALIZER;
static SqlEngineErrorCode forced_next_error = SQL_ENGINE_ERROR_NONE;
static int delay_after_lock_ms = 0;

/* API 응답의 statement_type 문자열을 AST의 문장 종류 enum에서 변환한다. */
static const char *statement_type_name(StatementType type) {
    if (type == STATEMENT_INSERT) {
        return "insert";
    }

    return "select";
}

/* 현재 AST가 가리키는 대상 테이블 이름을 문장 종류에 맞게 꺼낸다. */
static const char *statement_table_name(const Statement *statement) {
    if (statement->type == STATEMENT_INSERT) {
        return statement->as.insert_statement.table_name;
    }

    return statement->as.select_statement.table_name;
}

/* 공통 오류 결과 구조를 채우는 작은 헬퍼 함수다. */
static void set_error(SqlEngineAdapterResult *result, SqlEngineErrorCode code, const char *message) {
    result->ok = 0;
    result->error_code = code;
    snprintf(result->error_message, sizeof(result->error_message), "%s", message);
}

/* 공백만 들어온 SQL은 실행할 가치가 없으므로 미리 거른다. */
static int is_blank_sql(const char *sql) {
    while (*sql != '\0') {
        if (*sql != ' ' && *sql != '\t' && *sql != '\r' && *sql != '\n') {
            return 0;
        }
        sql++;
    }

    return 1;
}

/* 테스트에서 락 경쟁이나 장시간 실행을 재현하기 위한 인위적 지연 훅이다. */
static void sleep_for_test_delay(int delay_ms) {
    if (delay_ms <= 0) {
        return;
    }

#ifdef _WIN32
    Sleep((DWORD)delay_ms);
#else
    usleep((useconds_t)delay_ms * 1000U);
#endif
}

/* 실제 경과 시간을 재기 위한 monotonic clock 읽기 헬퍼다. */
static double monotonic_time_ms(void) {
#ifdef _WIN32
    return (double)GetTickCount64();
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

/* 다음 한 번의 실행에만 적용할 강제 오류 훅을 읽고 즉시 소진한다. */
static SqlEngineErrorCode consume_forced_next_error(void) {
    SqlEngineErrorCode code;

    pthread_mutex_lock(&test_hook_mutex);
    code = forced_next_error;
    forced_next_error = SQL_ENGINE_ERROR_NONE;
    pthread_mutex_unlock(&test_hook_mutex);
    return code;
}

/* 현재 설정된 테스트용 after-lock 지연 값을 읽어 온다. */
static int current_delay_after_lock_ms(void) {
    int delay_ms;

    pthread_mutex_lock(&test_hook_mutex);
    delay_ms = delay_after_lock_ms;
    pthread_mutex_unlock(&test_hook_mutex);
    return delay_ms;
}

/* 테스트 훅이 주입한 강제 오류 코드를 사람이 읽을 수 있는 메시지로 바꾼다. */
static const char *forced_error_message(SqlEngineErrorCode code) {
    switch (code) {
        case SQL_ENGINE_ERROR_ENGINE_EXECUTION_ERROR:
            return "forced engine execution failure";
        case SQL_ENGINE_ERROR_INTERNAL_ERROR:
            return "forced internal adapter failure";
        case SQL_ENGINE_ERROR_INDEX_REBUILD_ERROR:
            return "forced index rebuild failure";
        case SQL_ENGINE_ERROR_SCHEMA_LOAD_ERROR:
            return "forced schema load failure";
        default:
            return "forced adapter failure";
    }
}

/*
 * executor가 FILE*에 써 둔 결과를 다시 문자열 하나로 모은다.
 *
 * SELECT 결과는 기존 엔진이 표 형식 문자열로 출력하므로,
 * API 서버는 이 문자열을 읽어 JSON output 필드에 그대로 실어 보낸다.
 */
static char *read_stream_to_string(FILE *stream, char *error, size_t error_size) {
    char chunk[1024];
    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;

    if (fseek(stream, 0, SEEK_SET) != 0) {
        snprintf(error, error_size, "failed to rewind SQL output buffer");
        return NULL;
    }

    while (!feof(stream)) {
        size_t read_size = fread(chunk, 1, sizeof(chunk), stream);
        if (read_size > 0) {
            size_t required = length + read_size + 1;
            size_t new_capacity = capacity == 0 ? 1024 : capacity;
            char *new_buffer;

            while (new_capacity < required) {
                new_capacity *= 2;
            }

            new_buffer = (char *)realloc(buffer, new_capacity);
            if (new_buffer == NULL) {
                free(buffer);
                snprintf(error, error_size, "out of memory while reading SQL output");
                return NULL;
            }

            buffer = new_buffer;
            capacity = new_capacity;
            memcpy(buffer + length, chunk, read_size);
            length += read_size;
            buffer[length] = '\0';
        }

        if (ferror(stream)) {
            free(buffer);
            snprintf(error, error_size, "failed to read SQL output buffer");
            return NULL;
        }
    }

    if (buffer == NULL) {
        buffer = copy_string("");
        if (buffer == NULL) {
            snprintf(error, error_size, "out of memory while reading SQL output");
            return NULL;
        }
    }

    return buffer;
}

/*
 * executor가 돌려준 실패 메시지를 API 오류 코드로 분류한다.
 *
 * 같은 "실행 실패"여도
 * - 사용자 인자 오류인지
 * - 저장소 I/O 문제인지
 * - 인덱스 rebuild 문제인지
 * 를 나누어야 API가 더 정확한 오류를 반환할 수 있다.
 */
static SqlEngineErrorCode classify_execution_error(const Statement *statement, const char *message) {
    if (strstr(message, "WHERE id value must be an integer") != NULL ||
        strstr(message, "explicit id column is not allowed") != NULL ||
        strstr(message, "unknown column in INSERT") != NULL ||
        strstr(message, "unknown column in SELECT") != NULL ||
        strstr(message, "unknown column in WHERE") != NULL ||
        strstr(message, "column count and value count do not match") != NULL ||
        strstr(message, "duplicate column in INSERT") != NULL ||
        strstr(message, "newline in value is not supported") != NULL) {
        return SQL_ENGINE_ERROR_INVALID_SQL_ARGUMENT;
    }

    if (strstr(message, "failed to open table data file") != NULL ||
        strstr(message, "table data file is empty") != NULL ||
        strstr(message, "failed to open table file") != NULL) {
        return SQL_ENGINE_ERROR_STORAGE_IO_ERROR;
    }

    if ((statement->type == STATEMENT_SELECT &&
         statement->as.select_statement.has_where &&
         strcmp(statement->as.select_statement.where_column, "id") == 0) ||
        strstr(message, "forced index registration failure") != NULL) {
        return SQL_ENGINE_ERROR_INDEX_REBUILD_ERROR;
    }

    return SQL_ENGINE_ERROR_ENGINE_EXECUTION_ERROR;
}

/* schema 로딩 실패 중에서도 저장소 I/O와 스키마 형식 오류를 분리한다. */
static SqlEngineErrorCode classify_schema_load_error(const char *message) {
    if (strstr(message, "failed to open table data file") != NULL ||
        strstr(message, "table data file is empty") != NULL ||
        strstr(message, "failed to open schema meta file") != NULL) {
        return SQL_ENGINE_ERROR_STORAGE_IO_ERROR;
    }

    return SQL_ENGINE_ERROR_SCHEMA_LOAD_ERROR;
}

/*
 * SQL 문자열 하나를 기존 엔진으로 실행하는 어댑터의 핵심 함수다.
 *
 * 큰 흐름:
 * 1. 입력 검증
 * 2. lexer / parser 호출
 * 3. schema 로딩
 * 4. table lock 획득
 * 5. executor 호출
 * 6. 결과 캡처
 * 7. 시간 측정값 채우기
 * 8. 공통 cleanup
 */
int sql_engine_adapter_execute(const SqlEngineAdapterConfig *config,
                               const char *sql,
                               SqlEngineAdapterResult *result) {
    TokenArray tokens = {0};
    ParseResult parse_result;
    SchemaResult schema_result;
    ExecResult exec_result;
    EngineTableLockHandle lock_handle = {0};
    FILE *output_stream = NULL;
    clock_t started_cpu;
    double started_wall_ms;
    int lock_acquired = 0;

    memset(result, 0, sizeof(*result));
    memset(&schema_result, 0, sizeof(schema_result));

    /* NULL 또는 공백만 있는 SQL은 실행 전에 사용자 입력 오류로 처리한다. */
    if (sql == NULL || is_blank_sql(sql)) {
        set_error(result, SQL_ENGINE_ERROR_INVALID_SQL_ARGUMENT, "SQL statement must not be blank");
        return 1;
    }

    /* API에서 약속한 SQL 길이 제한을 초과하면 엔진 진입 전에 즉시 실패시킨다. */
    if (config->sql_length_limit > 0 && strlen(sql) > config->sql_length_limit) {
        set_error(result, SQL_ENGINE_ERROR_PAYLOAD_TOO_LARGE, "SQL statement exceeds configured length limit");
        return 1;
    }

    /*
     * CPU 시간과 실제 경과 시간을 각각 따로 잰다.
     * - elapsed_ms: clock() 기반 CPU 시간
     * - wall_elapsed_ms: monotonic clock 기반 실제 경과 시간
     */
    started_cpu = clock();
    started_wall_ms = monotonic_time_ms();

    /* 1단계: SQL 문자열을 토큰 배열로 분해한다. */
    if (!lex_sql(sql, &tokens, result->error_message, sizeof(result->error_message))) {
        set_error(result, SQL_ENGINE_ERROR_SQL_LEX_ERROR, result->error_message);
        return 1;
    }

    /* 2단계: 토큰 배열을 AST 한 문장으로 해석한다. */
    parse_result = parse_statement(&tokens);
    if (!parse_result.ok) {
        SqlEngineErrorCode code = SQL_ENGINE_ERROR_SQL_PARSE_ERROR;

        /*
         * parser 실패 중에서도 현재 과제 범위 밖인 기능은
         * 단순 parse error가 아니라 unsupported SQL로 분류한다.
         */
        if (strstr(parse_result.message, "only INSERT and SELECT are supported") != NULL ||
            strstr(parse_result.message, "AND/OR conditions are not supported") != NULL) {
            code = SQL_ENGINE_ERROR_UNSUPPORTED_SQL;
        }

        set_error(result, code, parse_result.message);
        free_tokens(&tokens);
        return 1;
    }

    /*
     * 3단계: schema를 로드해
     * - 요청 테이블이 실제로 존재하는지 확인하고
     * - canonical storage_name을 확정한다.
     *
     * 이후 락 키는 raw SQL 이름이 아니라 storage_name을 기준으로 삼는다.
     */
    schema_result = load_schema(config->schema_dir,
                                config->data_dir,
                                statement_table_name(&parse_result.statement));
    if (!schema_result.ok) {
        set_error(result, classify_schema_load_error(schema_result.message), schema_result.message);
        free_statement(&parse_result.statement);
        free_tokens(&tokens);
        return 1;
    }

    /*
     * 4단계: 같은 물리 테이블 요청을 직렬화하기 위해 table-level lock을 잡는다.
     * 단일 요청만 처리 중이라면 바로 통과하고,
     * 같은 테이블의 다른 요청이 먼저 실행 중이면 여기서 기다린다.
     */
    if (!engine_lock_manager_acquire(config->lock_manager,
                                     schema_result.schema.storage_name,
                                     &lock_handle,
                                     result->error_message,
                                     sizeof(result->error_message))) {
        set_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, result->error_message);
        goto cleanup;
    }
    lock_acquired = 1;

    /* 테스트에서는 락을 잡은 직후 일부러 지연을 넣어 대기 상황을 재현할 수 있다. */
    sleep_for_test_delay(current_delay_after_lock_ms());

    {
        /* 테스트 훅이 강제 오류를 설정해 두었으면 실제 executor 호출 전에 바로 실패시킨다. */
        SqlEngineErrorCode forced_error = consume_forced_next_error();
        if (forced_error != SQL_ENGINE_ERROR_NONE) {
            set_error(result, forced_error, forced_error_message(forced_error));
            goto cleanup;
        }
    }

    /* 5단계: 기존 executor 출력용 임시 FILE* 버퍼를 준비한다. */
    output_stream = tmpfile();
    if (output_stream == NULL) {
        set_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, "failed to create SQL output buffer");
        goto cleanup;
    }

    /*
     * 6단계: 실제 SQL 실행을 기존 executor에 위임한다.
     * 이 함수가 INSERT / SELECT / WHERE id 인덱스 경로 / 일반 CSV 스캔 경로를 모두 처리한다.
     */
    exec_result = execute_statement(&parse_result.statement,
                                    config->schema_dir,
                                    config->data_dir,
                                    output_stream);
    if (!exec_result.ok) {
        set_error(result,
                  classify_execution_error(&parse_result.statement, exec_result.message),
                  exec_result.message);
        goto cleanup;
    }

    /* 실행이 성공했으면 API 응답에 필요한 공통 필드를 채운다. */
    result->ok = 1;
    snprintf(result->statement_type,
             sizeof(result->statement_type),
             "%s",
             statement_type_name(parse_result.statement.type));
    result->affected_rows = exec_result.affected_rows;
    snprintf(result->summary, sizeof(result->summary), "%s", exec_result.message);

    /*
     * INSERT는 기존 엔진이 표 출력을 만들지 않으므로 빈 문자열을 output으로 사용한다.
     * SELECT는 executor가 만든 표 문자열을 다시 읽어 와 JSON 응답에 담는다.
     */
    if (parse_result.statement.type == STATEMENT_INSERT) {
        result->output = copy_string("");
    } else {
        result->output = read_stream_to_string(output_stream,
                                              result->error_message,
                                              sizeof(result->error_message));
        if (result->output == NULL) {
            set_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, result->error_message);
            goto cleanup;
        }
    }

    /* 마지막으로 두 종류의 시간 측정값을 모두 응답 구조에 기록한다. */
    result->elapsed_ms = ((double)(clock() - started_cpu) / (double)CLOCKS_PER_SEC) * 1000.0;
    result->wall_elapsed_ms = monotonic_time_ms() - started_wall_ms;

cleanup:
    /*
     * cleanup은 성공/실패 공통 종료 경로다.
     * 중간 단계 어디서 실패하더라도 여기로 와서 자원을 빠짐없이 정리한다.
     */
    if (output_stream != NULL) {
        fclose(output_stream);
    }
    if (lock_acquired) {
        /* lock을 실제로 잡은 경우에만 release 한다. */
        engine_lock_manager_release(&lock_handle);
    }
    /* schema / AST / token 메모리는 실행 성공 여부와 관계없이 정리한다. */
    free_schema(&schema_result.schema);
    free_statement(&parse_result.statement);
    free_tokens(&tokens);
    return 1;
}

/* result가 소유한 동적 output 문자열만 별도로 해제하는 함수다. */
void sql_engine_adapter_result_free(SqlEngineAdapterResult *result) {
    free(result->output);
    result->output = NULL;
}

/* 테스트에서 다음 실행 한 번에만 특정 오류를 강제로 발생시키도록 설정한다. */
void sql_engine_adapter_test_force_next_error(SqlEngineErrorCode code) {
    pthread_mutex_lock(&test_hook_mutex);
    forced_next_error = code;
    pthread_mutex_unlock(&test_hook_mutex);
}

/* 테스트에서 락 획득 직후 인위적 지연을 넣도록 설정한다. */
void sql_engine_adapter_test_set_delay_after_lock_ms(int delay_ms) {
    pthread_mutex_lock(&test_hook_mutex);
    delay_after_lock_ms = delay_ms < 0 ? 0 : delay_ms;
    pthread_mutex_unlock(&test_hook_mutex);
}

/* 테스트 훅 상태를 모두 초기화해 다음 테스트에 영향이 남지 않게 한다. */
void sql_engine_adapter_test_clear_hooks(void) {
    pthread_mutex_lock(&test_hook_mutex);
    forced_next_error = SQL_ENGINE_ERROR_NONE;
    delay_after_lock_ms = 0;
    pthread_mutex_unlock(&test_hook_mutex);
}

/* 내부 enum 오류 코드를 HTTP/JSON 응답에 노출할 문자열 코드로 변환한다. */
const char *sql_engine_error_code_name(SqlEngineErrorCode code) {
    switch (code) {
        case SQL_ENGINE_ERROR_NONE:
            return "NONE";
        case SQL_ENGINE_ERROR_INVALID_JSON:
            return "INVALID_JSON";
        case SQL_ENGINE_ERROR_MISSING_SQL_FIELD:
            return "MISSING_SQL_FIELD";
        case SQL_ENGINE_ERROR_INVALID_CONTENT_TYPE:
            return "INVALID_CONTENT_TYPE";
        case SQL_ENGINE_ERROR_CONTENT_LENGTH_REQUIRED:
            return "CONTENT_LENGTH_REQUIRED";
        case SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH:
            return "INVALID_CONTENT_LENGTH";
        case SQL_ENGINE_ERROR_PAYLOAD_TOO_LARGE:
            return "PAYLOAD_TOO_LARGE";
        case SQL_ENGINE_ERROR_HEADER_TOO_LARGE:
            return "HEADER_TOO_LARGE";
        case SQL_ENGINE_ERROR_CHUNKED_NOT_SUPPORTED:
            return "CHUNKED_NOT_SUPPORTED";
        case SQL_ENGINE_ERROR_METHOD_NOT_ALLOWED:
            return "METHOD_NOT_ALLOWED";
        case SQL_ENGINE_ERROR_NOT_FOUND:
            return "NOT_FOUND";
        case SQL_ENGINE_ERROR_SQL_LEX_ERROR:
            return "SQL_LEX_ERROR";
        case SQL_ENGINE_ERROR_SQL_PARSE_ERROR:
            return "SQL_PARSE_ERROR";
        case SQL_ENGINE_ERROR_UNSUPPORTED_SQL:
            return "UNSUPPORTED_SQL";
        case SQL_ENGINE_ERROR_INVALID_SQL_ARGUMENT:
            return "INVALID_SQL_ARGUMENT";
        case SQL_ENGINE_ERROR_ENGINE_EXECUTION_ERROR:
            return "ENGINE_EXECUTION_ERROR";
        case SQL_ENGINE_ERROR_SCHEMA_LOAD_ERROR:
            return "SCHEMA_LOAD_ERROR";
        case SQL_ENGINE_ERROR_STORAGE_IO_ERROR:
            return "STORAGE_IO_ERROR";
        case SQL_ENGINE_ERROR_INDEX_REBUILD_ERROR:
            return "INDEX_REBUILD_ERROR";
        case SQL_ENGINE_ERROR_QUEUE_FULL:
            return "QUEUE_FULL";
        case SQL_ENGINE_ERROR_INTERNAL_ERROR:
            return "INTERNAL_ERROR";
    }

    return "INTERNAL_ERROR";
}
