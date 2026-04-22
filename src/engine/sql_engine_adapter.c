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

//sql_engine_adapter.c는 “API 요청용 SQL 문자열”을 “기존 DB 엔진 실행 결과”로 바꿔주는 연결 파일입니다.

// SQL 문자열 받음
// -> lexer/parser로 해석
// -> schema 확인
// -> 락 잡음
// -> execute_statement() 실행
// -> 결과를 JSON 응답에 넣기 좋은 형태로 바꿔서 돌려줌




// parser가 만든 statementType값 받아서 insert 또는 select 문자열로 바꾸는 함수
static const char *statement_type_name(StatementType type) {
    if (type == STATEMENT_INSERT) {
        return "insert";
    }

    return "select";
}

// 테이블 이름만 꺼내는 함수
static const char *statement_table_name(const Statement *statement) {
    if (statement->type == STATEMENT_INSERT) {
        return statement->as.insert_statement.table_name;
    }

    return statement->as.select_statement.table_name;
}

// 에러 결과를 result 안에 정리해서 넣는 함수 ex)  SQL 비어있음, SQL 길이 너무김, lexer 실패 등
static void set_error(SqlEngineAdapterResult *result, SqlEngineErrorCode code, const char *message) {
    result->ok = 0;
    result->error_code = code;
    snprintf(result->error_message, sizeof(result->error_message), "%s", message);
}

// sql이 비어있거나 공백만 있는지 확인하는 함수

static int is_blank_sql(const char *sql) {

    //sql이 비어있지 않으면
    while (*sql != '\0') {
        if (*sql != ' ' && *sql != '\t' && *sql != '\r' && *sql != '\n') {
            return 0; // 0반환
        }
        sql++;
    }

    //sql이 비어있으면
    return 1; 
}

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

static SqlEngineErrorCode consume_forced_next_error(void) {
    SqlEngineErrorCode code;

    pthread_mutex_lock(&test_hook_mutex);
    code = forced_next_error;
    forced_next_error = SQL_ENGINE_ERROR_NONE;
    pthread_mutex_unlock(&test_hook_mutex);
    return code;
}

static int current_delay_after_lock_ms(void) {
    int delay_ms;

    pthread_mutex_lock(&test_hook_mutex);
    delay_ms = delay_after_lock_ms;
    pthread_mutex_unlock(&test_hook_mutex);
    return delay_ms;
}

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
// 파일 스트림에 들어있는 내용을 전부 읽어서 하나의 문자열로 만드는 함수

// 예를 들어 SELECT * FROM student; 실행 결과가 임시 스트림에 이렇게 들어있다고 해보면:

// id,name
// 1,Alice
// 2,Bob

// 반환값 : "id,name\n1,Alice\n2,Bob\n"
// DB 실행 결과를 클라이언트에게 보내기 쉬운 문자열 형태로 바꾸려고 하기 위해
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

// 실행 중 난 에러메시지를 보고 에러 종류를 분류하는 함수
// message 보고
static SqlEngineErrorCode classify_execution_error(const Statement *statement, const char *message) {
    // message안에 저 문자열이 있으면 에러 종류 반환
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

// schema 읽는 중에 난 에러를 분류하는 함수
static SqlEngineErrorCode classify_schema_load_error(const char *message) {
    // message안에 저 문자열이 있으면 에러 종류 반환
    if (strstr(message, "failed to open table data file") != NULL ||
        strstr(message, "table data file is empty") != NULL ||
        strstr(message, "failed to open schema meta file") != NULL) {
        return SQL_ENGINE_ERROR_STORAGE_IO_ERROR;
    }

    return SQL_ENGINE_ERROR_SCHEMA_LOAD_ERROR;
}

// sql 1개를 실제로 받아 실행하고 그 결과를 result에 채워주는 핵심 함수
//result에는 SQL 실행 결과 요약 정보가 들어 있다

//result.output = "id,name\n1,Alice\n2,Bob\n"
int sql_engine_adapter_execute(const SqlEngineAdapterConfig *config,
                               const char *sql,
                               SqlEngineAdapterResult *result) {
    TokenArray tokens = {0}; // 토큰 저장할 변수
    ParseResult parse_result; // 파싱 결과 저장
    SchemaResult schema_result; // 스키마 읽은 결과 저장
    ExecResult exec_result; // 실제 실행 결과 저장
    EngineTableLockHandle lock_handle = {0}; // 테이블 락 핸들
    FILE *output_stream = NULL; // SELECT 결과 담을 임시 파일
    clock_t started; // 시작시간 저장
    int lock_acquired = 0; // 락 잡았는지 표시

    memset(result, 0, sizeof(*result)); // result 초기화
    memset(&schema_result, 0, sizeof(schema_result)); // schema_result 초기화
    
    //sql이 비었는지 검사
    if (sql == NULL || is_blank_sql(sql)) {
        set_error(result, SQL_ENGINE_ERROR_INVALID_SQL_ARGUMENT, "SQL statement must not be blank");
        return 1;
    }

    //sql 길이 제한 초과 검사
    if (config->sql_length_limit > 0 && strlen(sql) > config->sql_length_limit) {
        set_error(result, SQL_ENGINE_ERROR_PAYLOAD_TOO_LARGE, "SQL statement exceeds configured length limit");
        return 1;
    }

    // 실행 시작 시간 기록
    started = clock();

    //lexer로 sql 토큰으로 분해
    if (!lex_sql(sql, &tokens, result->error_message, sizeof(result->error_message))) {
        set_error(result, SQL_ENGINE_ERROR_SQL_LEX_ERROR, result->error_message);
        return 1;
    }

    //토큰을 sql 문장 구조로 해석
    parse_result = parse_statement(&tokens);
    if (!parse_result.ok) {
        SqlEngineErrorCode code = SQL_ENGINE_ERROR_SQL_PARSE_ERROR;
        if (strstr(parse_result.message, "only INSERT and SELECT are supported") != NULL ||
            strstr(parse_result.message, "AND/OR conditions are not supported") != NULL) {
            code = SQL_ENGINE_ERROR_UNSUPPORTED_SQL;
        }

        set_error(result, code, parse_result.message);
        free_tokens(&tokens);
        return 1;
    }

    //schema 읽기
    schema_result = load_schema(config->schema_dir,
                                config->data_dir,
                                statement_table_name(&parse_result.statement));
    if (!schema_result.ok) {
        set_error(result, classify_schema_load_error(schema_result.message), schema_result.message);
        free_statement(&parse_result.statement);
        free_tokens(&tokens);
        return 1;
    }

    // 테이블 락 잡기
    // 같은 테이블에 다른 worker thread가 이미 들어가 있으면 여기서 기다린다

    // worker1 : studnet 테이블 INSERT
    // worker2 : student 테이블 SELECT
    if (!engine_lock_manager_acquire(config->lock_manager,
                                     schema_result.schema.storage_name,
                                     &lock_handle,
                                     result->error_message,
                                     sizeof(result->error_message))) {
        set_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, result->error_message);
        goto cleanup;
    }
    lock_acquired = 1;

    sleep_for_test_delay(current_delay_after_lock_ms());

    {
        SqlEngineErrorCode forced_error = consume_forced_next_error();
        if (forced_error != SQL_ENGINE_ERROR_NONE) {
            set_error(result, forced_error, forced_error_message(forced_error));
            goto cleanup;
        }
    }
    //SELECT 결과 담을 임시파일 만들기
    output_stream = tmpfile();
    if (output_stream == NULL) {
        set_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, "failed to create SQL output buffer");
        goto cleanup;
    }


    //실제 SQL 실행

    //INSERT면 CSV에 저장
    //SELECT면 조회해서 결과를 output_stream에 씀
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

    //성공 결과 채우기
    //구조체 채운다
    result->ok = 1;
    snprintf(result->statement_type,
             sizeof(result->statement_type),
             "%s",
             statement_type_name(parse_result.statement.type));
    result->affected_rows = exec_result.affected_rows;
    snprintf(result->summary, sizeof(result->summary), "%s", exec_result.message);


    //INSERT는 보여줄 결과 없으니까 output =""
    if (parse_result.statement.type == STATEMENT_INSERT) {
        result->output = copy_string("");
    }
    //SELECT는 result->output에 output_stream저장 
    else {
        result->output = read_stream_to_string(output_stream,
                                              result->error_message,
                                              sizeof(result->error_message));
        if (result->output == NULL) {
            set_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, result->error_message);
            goto cleanup;
        }
    }

    result->elapsed_ms = ((double)(clock() - started) / (double)CLOCKS_PER_SEC) * 1000.0;

cleanup:
    if (output_stream != NULL) {
        fclose(output_stream);
    }
    if (lock_acquired) {
        engine_lock_manager_release(&lock_handle);
    }
    free_schema(&schema_result.schema);
    free_statement(&parse_result.statement);
    free_tokens(&tokens);
    return 1;
}

//메모리 해제
void sql_engine_adapter_result_free(SqlEngineAdapterResult *result) {
    free(result->output);
    result->output = NULL;
}

void sql_engine_adapter_test_force_next_error(SqlEngineErrorCode code) {
    pthread_mutex_lock(&test_hook_mutex);
    forced_next_error = code;
    pthread_mutex_unlock(&test_hook_mutex);
}

void sql_engine_adapter_test_set_delay_after_lock_ms(int delay_ms) {
    pthread_mutex_lock(&test_hook_mutex);
    delay_after_lock_ms = delay_ms < 0 ? 0 : delay_ms;
    pthread_mutex_unlock(&test_hook_mutex);
}

void sql_engine_adapter_test_clear_hooks(void) {
    pthread_mutex_lock(&test_hook_mutex);
    forced_next_error = SQL_ENGINE_ERROR_NONE;
    delay_after_lock_ms = 0;
    pthread_mutex_unlock(&test_hook_mutex);
}

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
