#include "sqlparser/service/db_service.h"

int db_service_execute_sql(DbService *service, const char *sql, SqlEngineAdapterResult *result) {
    return sql_engine_adapter_execute(&service->adapter_config, sql, result);
}
