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

static const char *statement_type_name(StatementType type) {
    if (type == STATEMENT_INSERT) {
        return "insert";
    }

    return "select";
}

static const char *statement_table_name(const Statement *statement) {
    if (statement->type == STATEMENT_INSERT) {
        return statement->as.insert_statement.table_name;
    }

    return statement->as.select_statement.table_name;
}

static void set_error(SqlEngineAdapterResult *result, SqlEngineErrorCode code, const char *message) {
    result->ok = 0;
    result->error_code = code;
    snprintf(result->error_message, sizeof(result->error_message), "%s", message);
}

static int is_blank_sql(const char *sql) {
    while (*sql != '\0') {
        if (*sql != ' ' && *sql != '\t' && *sql != '\r' && *sql != '\n') {
            return 0;
        }
        sql++;
    }

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

static double monotonic_time_ms(void) {
#ifdef _WIN32
    return (double)GetTickCount64();
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
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

static SqlEngineErrorCode classify_schema_load_error(const char *message) {
    if (strstr(message, "failed to open table data file") != NULL ||
        strstr(message, "table data file is empty") != NULL ||
        strstr(message, "failed to open schema meta file") != NULL) {
        return SQL_ENGINE_ERROR_STORAGE_IO_ERROR;
    }

    return SQL_ENGINE_ERROR_SCHEMA_LOAD_ERROR;
}

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

    if (sql == NULL || is_blank_sql(sql)) {
        set_error(result, SQL_ENGINE_ERROR_INVALID_SQL_ARGUMENT, "SQL statement must not be blank");
        return 1;
    }

    if (config->sql_length_limit > 0 && strlen(sql) > config->sql_length_limit) {
        set_error(result, SQL_ENGINE_ERROR_PAYLOAD_TOO_LARGE, "SQL statement exceeds configured length limit");
        return 1;
    }

    started_cpu = clock();
    started_wall_ms = monotonic_time_ms();

    if (!lex_sql(sql, &tokens, result->error_message, sizeof(result->error_message))) {
        set_error(result, SQL_ENGINE_ERROR_SQL_LEX_ERROR, result->error_message);
        return 1;
    }

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

    schema_result = load_schema(config->schema_dir,
                                config->data_dir,
                                statement_table_name(&parse_result.statement));
    if (!schema_result.ok) {
        set_error(result, classify_schema_load_error(schema_result.message), schema_result.message);
        free_statement(&parse_result.statement);
        free_tokens(&tokens);
        return 1;
    }

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

    output_stream = tmpfile();
    if (output_stream == NULL) {
        set_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, "failed to create SQL output buffer");
        goto cleanup;
    }

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

    result->ok = 1;
    snprintf(result->statement_type,
             sizeof(result->statement_type),
             "%s",
             statement_type_name(parse_result.statement.type));
    result->affected_rows = exec_result.affected_rows;
    snprintf(result->summary, sizeof(result->summary), "%s", exec_result.message);

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

    result->elapsed_ms = ((double)(clock() - started_cpu) / (double)CLOCKS_PER_SEC) * 1000.0;
    result->wall_elapsed_ms = monotonic_time_ms() - started_wall_ms;

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
