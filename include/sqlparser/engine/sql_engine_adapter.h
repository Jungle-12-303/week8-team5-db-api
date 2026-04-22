#ifndef SQLPARSER_ENGINE_SQL_ENGINE_ADAPTER_H
#define SQLPARSER_ENGINE_SQL_ENGINE_ADAPTER_H

#include "sqlparser/engine/engine_lock_manager.h"

#include <stddef.h>

typedef enum {
    SQL_ENGINE_ERROR_NONE,
    SQL_ENGINE_ERROR_INVALID_JSON,
    SQL_ENGINE_ERROR_MISSING_SQL_FIELD,
    SQL_ENGINE_ERROR_INVALID_CONTENT_TYPE,
    SQL_ENGINE_ERROR_CONTENT_LENGTH_REQUIRED,
    SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH,
    SQL_ENGINE_ERROR_PAYLOAD_TOO_LARGE,
    SQL_ENGINE_ERROR_HEADER_TOO_LARGE,
    SQL_ENGINE_ERROR_CHUNKED_NOT_SUPPORTED,
    SQL_ENGINE_ERROR_METHOD_NOT_ALLOWED,
    SQL_ENGINE_ERROR_NOT_FOUND,
    SQL_ENGINE_ERROR_SQL_LEX_ERROR,
    SQL_ENGINE_ERROR_SQL_PARSE_ERROR,
    SQL_ENGINE_ERROR_UNSUPPORTED_SQL,
    SQL_ENGINE_ERROR_INVALID_SQL_ARGUMENT,
    SQL_ENGINE_ERROR_ENGINE_EXECUTION_ERROR,
    SQL_ENGINE_ERROR_SCHEMA_LOAD_ERROR,
    SQL_ENGINE_ERROR_STORAGE_IO_ERROR,
    SQL_ENGINE_ERROR_INDEX_REBUILD_ERROR,
    SQL_ENGINE_ERROR_QUEUE_FULL,
    SQL_ENGINE_ERROR_INTERNAL_ERROR
} SqlEngineErrorCode;

typedef struct {
    const char *schema_dir;
    const char *data_dir;
    size_t sql_length_limit;
    EngineLockManager *lock_manager;
} SqlEngineAdapterConfig;

typedef struct {
    int ok;
    char statement_type[16];
    int affected_rows;
    char summary[256];
    char *output;
    double elapsed_ms;
    double wall_elapsed_ms;
    SqlEngineErrorCode error_code;
    char error_message[256];
} SqlEngineAdapterResult;

int sql_engine_adapter_execute(const SqlEngineAdapterConfig *config,
                               const char *sql,
                               SqlEngineAdapterResult *result);
void sql_engine_adapter_result_free(SqlEngineAdapterResult *result);
const char *sql_engine_error_code_name(SqlEngineErrorCode code);

/* 테스트용: 다음 실행 한 번에 강제로 특정 오류를 반환한다. */
void sql_engine_adapter_test_force_next_error(SqlEngineErrorCode code);

/* 테스트용: 테이블 락을 잡은 직후 인위적으로 지연을 삽입한다. */
void sql_engine_adapter_test_set_delay_after_lock_ms(int delay_ms);

/* 테스트용 hook 상태를 초기화한다. */
void sql_engine_adapter_test_clear_hooks(void);

#endif
