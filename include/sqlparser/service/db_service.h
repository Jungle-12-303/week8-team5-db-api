#ifndef SQLPARSER_SERVICE_DB_SERVICE_H
#define SQLPARSER_SERVICE_DB_SERVICE_H

#include "sqlparser/engine/sql_engine_adapter.h"

typedef struct {
    SqlEngineAdapterConfig adapter_config;
} DbService;

int db_service_execute_sql(DbService *service, const char *sql, SqlEngineAdapterResult *result);

#endif
