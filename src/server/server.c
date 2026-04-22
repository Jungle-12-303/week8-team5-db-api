#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif

#include "sqlparser/server/server.h"

#include "sqlparser/api/api_context.h"
#include "sqlparser/common/platform.h"
#include "sqlparser/common/util.h"
#include "sqlparser/engine/engine_lock_manager.h"
#include "sqlparser/http/http_request.h"
#include "sqlparser/http/http_response.h"
#include "sqlparser/http/router.h"
#include "sqlparser/index/table_index.h"
#include "sqlparser/server/task_queue.h"
#include "sqlparser/server/worker_pool.h"
#include "sqlparser/service/db_service.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>
#endif

struct SqlApiServer {
    char *host;
    int port;
    int worker_count;
    int queue_capacity;
    char *schema_dir;
    char *data_dir;
    size_t request_body_limit;
    size_t sql_length_limit;
    size_t header_limit;
    sql_socket_t listen_socket;
    pthread_t accept_thread;
    pthread_mutex_t state_mutex;
    int shutting_down;
    int started;
    ServerTaskQueue task_queue;
    ServerWorkerPool worker_pool;
    EngineLockManager lock_manager;
    DbService db_service;
};

static int directory_exists(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

void sqlapi_server_config_set_defaults(SqlApiServerConfig *config) {
    config->host = "127.0.0.1";
    config->port = 8080;
    config->worker_count = 4;
    config->queue_capacity = 64;
    config->schema_dir = "schema";
    config->data_dir = "data";
    config->request_body_limit = 16 * 1024;
    config->sql_length_limit = 8 * 1024;
    config->header_limit = 8 * 1024;
}

int sqlapi_server_validate_config(const SqlApiServerConfig *config, char *error, size_t error_size) {
    if (config->port < 1 || config->port > 65535) {
        snprintf(error, error_size, "--port must be between 1 and 65535");
        return 0;
    }
    if (config->worker_count < 1) {
        snprintf(error, error_size, "--worker-count must be at least 1");
        return 0;
    }
    if (config->queue_capacity < 1) {
        snprintf(error, error_size, "--queue-capacity must be at least 1");
        return 0;
    }
    if (!directory_exists(config->schema_dir)) {
        snprintf(error, error_size, "--schema-dir must point to an existing directory");
        return 0;
    }
    if (!directory_exists(config->data_dir)) {
        snprintf(error, error_size, "--data-dir must point to an existing directory");
        return 0;
    }
    return 1;
}

static int send_simple_error(sql_socket_t socket_fd, SqlEngineErrorCode code, const char *message) {
    HttpResponse response;
    int ok;
    http_response_init(&response);
    if (!http_response_set_error(&response, code, message)) {
        return 0;
    }
    ok = http_response_send(socket_fd, &response);
    http_response_free(&response);
    return ok;
}

static void close_client_socket(sql_socket_t socket_fd) {
    if (socket_fd != SQL_INVALID_SOCKET) {
        sql_platform_close_socket(socket_fd);
    }
}

static void handle_client_request(SqlApiServer *server, sql_socket_t client_socket) {
    HttpRequest request;
    HttpRequestReadResult read_result;
    HttpResponse response;
    ApiContext context;

    http_request_init(&request);
    http_response_init(&response);

    if (!http_read_request(client_socket,
                           server->header_limit,
                           server->request_body_limit,
                           &request,
                           &read_result)) {
        send_simple_error(client_socket, read_result.error_code, read_result.message);
        http_request_free(&request);
        http_response_free(&response);
        close_client_socket(client_socket);
        return;
    }

    context.db_service = &server->db_service;
    context.worker_count = server->worker_count;
    context.queue_depth = sqlapi_server_queue_depth(server);

    if (!http_route_request(&request, &context, &response)) {
        send_simple_error(client_socket,
                          SQL_ENGINE_ERROR_INTERNAL_ERROR,
                          "failed to construct HTTP response");
    } else {
        http_response_send(client_socket, &response);
    }

    http_request_free(&request);
    http_response_free(&response);
    close_client_socket(client_socket);
}

static void *server_worker_main(void *context) {
    SqlApiServer *server = (SqlApiServer *)context;
    ServerTask task;

    while (server_task_queue_pop(&server->task_queue, &task)) {
        handle_client_request(server, task.client_socket);
    }

    return NULL;
}

static int server_is_shutting_down(SqlApiServer *server) {
    int shutting_down;

    pthread_mutex_lock(&server->state_mutex);
    shutting_down = server->shutting_down;
    pthread_mutex_unlock(&server->state_mutex);
    return shutting_down;
}

static void *server_accept_main(void *context) {
    SqlApiServer *server = (SqlApiServer *)context;

    while (!server_is_shutting_down(server)) {
        sql_socket_t client_socket = accept(server->listen_socket, NULL, NULL);
        if (client_socket == SQL_INVALID_SOCKET) {
            if (server_is_shutting_down(server)) {
                break;
            }
            continue;
        }

        if (!server_task_queue_try_push(&server->task_queue, &(ServerTask){client_socket})) {
            send_simple_error(client_socket,
                              SQL_ENGINE_ERROR_QUEUE_FULL,
                              "server is busy");
            close_client_socket(client_socket);
        }
    }

    return NULL;
}

static int open_listen_socket(SqlApiServer *server, char *error, size_t error_size) {
    char port_text[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *current;
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    snprintf(port_text, sizeof(port_text), "%d", server->port);

    status = getaddrinfo(server->host, port_text, &hints, &result);
    if (status != 0) {
        snprintf(error, error_size, "failed to resolve listen address");
        return 0;
    }

    for (current = result; current != NULL; current = current->ai_next) {
        int yes = 1;
        server->listen_socket = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (server->listen_socket == SQL_INVALID_SOCKET) {
            continue;
        }

        setsockopt(server->listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
        if (bind(server->listen_socket, current->ai_addr, (int)current->ai_addrlen) == 0 &&
            listen(server->listen_socket, server->queue_capacity) == 0) {
            freeaddrinfo(result);
            return 1;
        }

        sql_platform_close_socket(server->listen_socket);
        server->listen_socket = SQL_INVALID_SOCKET;
    }

    freeaddrinfo(result);
    sql_platform_format_socket_error(error, error_size, "failed to bind or listen");
    return 0;
}

int sqlapi_server_create(SqlApiServer **out_server,
                         const SqlApiServerConfig *config,
                         char *error,
                         size_t error_size) {
    SqlApiServer *server;

    if (!sqlapi_server_validate_config(config, error, error_size)) {
        return 0;
    }

    server = (SqlApiServer *)calloc(1, sizeof(*server));
    if (server == NULL) {
        snprintf(error, error_size, "out of memory while creating server");
        return 0;
    }

    server->host = copy_string(config->host);
    server->schema_dir = copy_string(config->schema_dir);
    server->data_dir = copy_string(config->data_dir);
    if (server->host == NULL || server->schema_dir == NULL || server->data_dir == NULL) {
        free(server->host);
        free(server->schema_dir);
        free(server->data_dir);
        free(server);
        snprintf(error, error_size, "out of memory while copying server configuration");
        return 0;
    }

    server->port = config->port;
    server->worker_count = config->worker_count;
    server->queue_capacity = config->queue_capacity;
    server->request_body_limit = config->request_body_limit;
    server->sql_length_limit = config->sql_length_limit;
    server->header_limit = config->header_limit;
    server->listen_socket = SQL_INVALID_SOCKET;

    if (pthread_mutex_init(&server->state_mutex, NULL) != 0) {
        free(server->host);
        free(server->schema_dir);
        free(server->data_dir);
        free(server);
        snprintf(error, error_size, "failed to initialize server state mutex");
        return 0;
    }

    if (!server_task_queue_init(&server->task_queue, server->queue_capacity, error, error_size)) {
        pthread_mutex_destroy(&server->state_mutex);
        free(server->host);
        free(server->schema_dir);
        free(server->data_dir);
        free(server);
        return 0;
    }

    if (!table_index_registry_init(error, error_size)) {
        server_task_queue_destroy(&server->task_queue);
        pthread_mutex_destroy(&server->state_mutex);
        free(server->host);
        free(server->schema_dir);
        free(server->data_dir);
        free(server);
        return 0;
    }

    if (!engine_lock_manager_init(&server->lock_manager, error, error_size)) {
        server_task_queue_destroy(&server->task_queue);
        pthread_mutex_destroy(&server->state_mutex);
        free(server->host);
        free(server->schema_dir);
        free(server->data_dir);
        free(server);
        return 0;
    }

    server->db_service.adapter_config.schema_dir = server->schema_dir;
    server->db_service.adapter_config.data_dir = server->data_dir;
    server->db_service.adapter_config.sql_length_limit = server->sql_length_limit;
    server->db_service.adapter_config.lock_manager = &server->lock_manager;

    *out_server = server;
    return 1;
}

int sqlapi_server_start(SqlApiServer *server, char *error, size_t error_size) {
    if (!sql_platform_network_init(error, error_size)) {
        return 0;
    }

    if (!open_listen_socket(server, error, error_size)) {
        return 0;
    }

    if (!server_worker_pool_start(&server->worker_pool,
                                  server->worker_count,
                                  server_worker_main,
                                  server,
                                  error,
                                  error_size)) {
        sql_platform_close_socket(server->listen_socket);
        server->listen_socket = SQL_INVALID_SOCKET;
        return 0;
    }

    if (pthread_create(&server->accept_thread, NULL, server_accept_main, server) != 0) {
        snprintf(error, error_size, "failed to create accept thread");
        server_task_queue_close(&server->task_queue);
        server_worker_pool_join(&server->worker_pool);
        server_worker_pool_destroy(&server->worker_pool);
        sql_platform_close_socket(server->listen_socket);
        server->listen_socket = SQL_INVALID_SOCKET;
        return 0;
    }

    server->started = 1;
    return 1;
}

void sqlapi_server_request_shutdown(SqlApiServer *server) {
    pthread_mutex_lock(&server->state_mutex);
    if (!server->shutting_down) {
        server->shutting_down = 1;
        if (server->listen_socket != SQL_INVALID_SOCKET) {
            sql_platform_shutdown_socket(server->listen_socket);
            sql_platform_close_socket(server->listen_socket);
            server->listen_socket = SQL_INVALID_SOCKET;
        }
        server_task_queue_close(&server->task_queue);
    }
    pthread_mutex_unlock(&server->state_mutex);
}

void sqlapi_server_wait(SqlApiServer *server) {
    if (!server->started) {
        return;
    }

    pthread_join(server->accept_thread, NULL);
    server_worker_pool_join(&server->worker_pool);
}

void sqlapi_server_destroy(SqlApiServer *server) {
    if (server == NULL) {
        return;
    }

    sqlapi_server_request_shutdown(server);
    if (server->started) {
        server_worker_pool_destroy(&server->worker_pool);
    }
    table_index_registry_reset();
    table_index_registry_shutdown();
    engine_lock_manager_destroy(&server->lock_manager);
    server_task_queue_destroy(&server->task_queue);
    pthread_mutex_destroy(&server->state_mutex);
    free(server->host);
    free(server->schema_dir);
    free(server->data_dir);
    sql_platform_network_cleanup();
    free(server);
}

int sqlapi_server_queue_depth(SqlApiServer *server) {
    return server_task_queue_depth(&server->task_queue);
}

int sqlapi_server_worker_count(SqlApiServer *server) {
    return server->worker_count;
}
