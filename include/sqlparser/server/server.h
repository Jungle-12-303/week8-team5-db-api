#ifndef SQLPARSER_SERVER_SERVER_H
#define SQLPARSER_SERVER_SERVER_H

#include "sqlparser/common/platform.h"

#include <stddef.h>

typedef struct {
    const char *host;
    int port;
    int worker_count;
    int queue_capacity;
    const char *schema_dir;
    const char *data_dir;
    size_t request_body_limit;
    size_t sql_length_limit;
    size_t header_limit;
} SqlApiServerConfig;

typedef struct SqlApiServer SqlApiServer;

void sqlapi_server_config_set_defaults(SqlApiServerConfig *config);
int sqlapi_server_validate_config(const SqlApiServerConfig *config, char *error, size_t error_size);
int sqlapi_server_create(SqlApiServer **out_server,
                         const SqlApiServerConfig *config,
                         char *error,
                         size_t error_size);
int sqlapi_server_start(SqlApiServer *server, char *error, size_t error_size);
void sqlapi_server_request_shutdown(SqlApiServer *server);
void sqlapi_server_wait(SqlApiServer *server);
void sqlapi_server_destroy(SqlApiServer *server);
int sqlapi_server_queue_depth(SqlApiServer *server);
int sqlapi_server_worker_count(SqlApiServer *server);

#endif
