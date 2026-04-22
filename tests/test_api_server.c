#ifndef _WIN32
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#endif

#include "sqlparser/common/platform.h"
#include "sqlparser/common/util.h"
#include "sqlparser/engine/sql_engine_adapter.h"
#include "sqlparser/index/table_index.h"
#include "sqlparser/server/server.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MAKE_DIR(path) _mkdir(path)
#define CLOSE_WRITE(socket_fd) shutdown((socket_fd), SD_SEND)
#else
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#define MAKE_DIR(path) mkdir(path, 0755)
#define CLOSE_WRITE(socket_fd) shutdown((socket_fd), SHUT_WR)
#endif

static int api_temp_counter = 0;

static int allocate_test_port(void) {
    sql_socket_t socket_fd;
    struct sockaddr_in address;
    socklen_t address_length = (socklen_t)sizeof(address);
    int port = 0;

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == SQL_INVALID_SOCKET) {
        return 0;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);

    if (bind(socket_fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        sql_platform_close_socket(socket_fd);
        return 0;
    }

    if (getsockname(socket_fd, (struct sockaddr *)&address, &address_length) != 0) {
        sql_platform_close_socket(socket_fd);
        return 0;
    }

    port = (int)ntohs(address.sin_port);
    sql_platform_close_socket(socket_fd);
    return port;
}

static void build_child_path(char *buffer, size_t size, const char *root, const char *child) {
    snprintf(buffer, size, "%s/%s", root, child);
}

static int write_text_file(const char *path, const char *content) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    fputs(content, file);
    fclose(file);
    return 1;
}

static int create_test_dirs(char *root, size_t root_size, char *schema_dir, size_t schema_size, char *data_dir, size_t data_size) {
    long suffix = (long)time(NULL);
    api_temp_counter++;

    snprintf(root, root_size, "build/tests/api_tmp_%ld_%d", suffix, api_temp_counter);
    build_child_path(schema_dir, schema_size, root, "schema");
    build_child_path(data_dir, data_size, root, "data");

    MAKE_DIR("build");
    MAKE_DIR("build/tests");
    if (MAKE_DIR(root) != 0) {
        return 0;
    }
    if (MAKE_DIR(schema_dir) != 0) {
        return 0;
    }
    if (MAKE_DIR(data_dir) != 0) {
        return 0;
    }

    return 1;
}

static void sleep_briefly(void) {
#ifdef _WIN32
    Sleep(100);
#else
    usleep(100000);
#endif
}

static void sleep_millis(int milliseconds) {
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    usleep((useconds_t)milliseconds * 1000U);
#endif
}

static double now_ms(void) {
#ifdef _WIN32
    return (double)GetTickCount64();
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

static int connect_to_server(const char *host, int port, sql_socket_t *socket_out) {
    char port_text[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *current;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_text, sizeof(port_text), "%d", port);

    if (getaddrinfo(host, port_text, &hints, &result) != 0) {
        return 0;
    }

    for (current = result; current != NULL; current = current->ai_next) {
        sql_socket_t socket_fd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socket_fd == SQL_INVALID_SOCKET) {
            continue;
        }

        if (connect(socket_fd, current->ai_addr, (int)current->ai_addrlen) == 0) {
            freeaddrinfo(result);
            *socket_out = socket_fd;
            return 1;
        }

        sql_platform_close_socket(socket_fd);
    }

    freeaddrinfo(result);
    return 0;
}

static int send_all(sql_socket_t socket_fd, const char *buffer, size_t length) {
    size_t offset = 0;

    while (offset < length) {
        int sent = send(socket_fd, buffer + offset, (int)(length - offset), 0);
        if (sent <= 0) {
            return 0;
        }
        offset += (size_t)sent;
    }

    return 1;
}

static int send_http_request(const char *host, int port, const char *request, char *response, size_t response_size) {
    sql_socket_t socket_fd;
    size_t offset = 0;

    if (!connect_to_server(host, port, &socket_fd)) {
        return 0;
    }

    if (!send_all(socket_fd, request, strlen(request))) {
        sql_platform_close_socket(socket_fd);
        return 0;
    }

    CLOSE_WRITE(socket_fd);
    while (offset + 1 < response_size) {
        int received = recv(socket_fd, response + offset, (int)(response_size - offset - 1), 0);
        if (received <= 0) {
            break;
        }
        offset += (size_t)received;
    }

    response[offset] = '\0';
    sql_platform_close_socket(socket_fd);
    return 1;
}

static int expect_contains(const char *response, const char *needle, const char *name) {
    if (strstr(response, needle) == NULL) {
        fprintf(stderr, "[FAIL] %s\n", name);
        fprintf(stderr, "response was:\n%s\n", response);
        return 0;
    }

    printf("[PASS] %s\n", name);
    return 1;
}

static int expect_error_response(const char *response,
                                 const char *status_line,
                                 const char *error_code,
                                 const char *name_prefix) {
    char status_name[128];
    char code_name[128];

    snprintf(status_name, sizeof(status_name), "%s status", name_prefix);
    snprintf(code_name, sizeof(code_name), "%s error code", name_prefix);

    return expect_contains(response, status_line, status_name) &&
           expect_contains(response, error_code, code_name);
}

static int expect_true(int condition, const char *name) {
    if (!condition) {
        fprintf(stderr, "[FAIL] %s\n", name);
        return 0;
    }

    printf("[PASS] %s\n", name);
    return 1;
}

static int expect_not_contains(const char *response, const char *needle, const char *name) {
    if (strstr(response, needle) != NULL) {
        fprintf(stderr, "[FAIL] %s\n", name);
        fprintf(stderr, "response was:\n%s\n", response);
        return 0;
    }

    printf("[PASS] %s\n", name);
    return 1;
}

static int send_large_http_request(const char *host,
                                   int port,
                                   const char *request_prefix,
                                   size_t repeated_length,
                                   char repeated_char,
                                   const char *request_suffix,
                                   char *response,
                                   size_t response_size) {
    size_t prefix_length = strlen(request_prefix);
    size_t suffix_length = strlen(request_suffix);
    size_t total_length = prefix_length + repeated_length + suffix_length;
    char *request = (char *)malloc(total_length + 1);
    int ok;

    if (request == NULL) {
        return 0;
    }

    memcpy(request, request_prefix, prefix_length);
    memset(request + prefix_length, repeated_char, repeated_length);
    memcpy(request + prefix_length + repeated_length, request_suffix, suffix_length);
    request[total_length] = '\0';

    ok = send_http_request(host, port, request, response, response_size);
    free(request);
    return ok;
}

static char *build_post_request(const char *path, const char *content_type, const char *body) {
    int required = snprintf(NULL,
                            0,
                            "POST %s HTTP/1.1\r\n"
                            "Host: 127.0.0.1\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "\r\n"
                            "%s",
                            path,
                            content_type,
                            strlen(body),
                            body);
    char *request;

    if (required < 0) {
        return NULL;
    }

    request = (char *)malloc((size_t)required + 1);
    if (request == NULL) {
        return NULL;
    }

    snprintf(request,
             (size_t)required + 1,
             "POST %s HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             path,
             content_type,
             strlen(body),
             body);
    return request;
}

static char *build_query_json_with_padding(const char *sql, size_t target_length) {
    const char *prefix = "{\"sql\":\"";
    const char *middle = "\",\"pad\":\"";
    const char *suffix = "\"}";
    size_t base_length = strlen(prefix) + strlen(sql) + strlen(middle) + strlen(suffix);
    size_t pad_length;
    char *body;

    if (target_length < base_length) {
        return NULL;
    }

    pad_length = target_length - base_length;
    body = (char *)malloc(target_length + 1);
    if (body == NULL) {
        return NULL;
    }

    memcpy(body, prefix, strlen(prefix));
    memcpy(body + strlen(prefix), sql, strlen(sql));
    memcpy(body + strlen(prefix) + strlen(sql), middle, strlen(middle));
    memset(body + strlen(prefix) + strlen(sql) + strlen(middle), 'a', pad_length);
    memcpy(body + strlen(prefix) + strlen(sql) + strlen(middle) + pad_length, suffix, strlen(suffix));
    body[target_length] = '\0';
    return body;
}

static char *build_insert_sql_with_length(size_t target_length) {
    const char *prefix = "INSERT INTO users (name) VALUES ('";
    const char *suffix = "');";
    size_t prefix_length = strlen(prefix);
    size_t suffix_length = strlen(suffix);
    size_t fill_length;
    char *sql;

    if (target_length < prefix_length + suffix_length) {
        return NULL;
    }

    fill_length = target_length - prefix_length - suffix_length;
    sql = (char *)malloc(target_length + 1);
    if (sql == NULL) {
        return NULL;
    }

    memcpy(sql, prefix, prefix_length);
    memset(sql + prefix_length, 'a', fill_length);
    memcpy(sql + prefix_length + fill_length, suffix, suffix_length);
    sql[target_length] = '\0';
    return sql;
}

static char *build_exact_header_request(size_t total_header_length) {
    const char *prefix = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Fill: ";
    const char *suffix = "\r\n\r\n";
    size_t prefix_length = strlen(prefix);
    size_t suffix_length = strlen(suffix);
    size_t fill_length;
    char *request;

    if (total_header_length < prefix_length + suffix_length) {
        return NULL;
    }

    fill_length = total_header_length - prefix_length - suffix_length;
    request = (char *)malloc(total_header_length + 1);
    if (request == NULL) {
        return NULL;
    }

    memcpy(request, prefix, prefix_length);
    memset(request + prefix_length, 'h', fill_length);
    memcpy(request + prefix_length + fill_length, suffix, suffix_length);
    request[total_header_length] = '\0';
    return request;
}

typedef struct {
    const char *host;
    int port;
    const char *request;
    char response[131072];
    int ok;
    double elapsed_ms;
} AsyncHttpRequest;

static void *run_async_http_request(void *context) {
    AsyncHttpRequest *request = (AsyncHttpRequest *)context;
    double started = now_ms();

    request->ok = send_http_request(request->host,
                                    request->port,
                                    request->request,
                                    request->response,
                                    sizeof(request->response));
    request->elapsed_ms = now_ms() - started;
    return NULL;
}

static int wait_for_queue_depth(SqlApiServer *server, int expected_depth, int timeout_ms) {
    double deadline = now_ms() + (double)timeout_ms;

    while (now_ms() < deadline) {
        if (sqlapi_server_queue_depth(server) == expected_depth) {
            return 1;
        }
        sleep_millis(10);
    }

    return sqlapi_server_queue_depth(server) == expected_depth;
}

static SqlApiServer *start_test_server(int port,
                                       int worker_count,
                                       int queue_capacity,
                                       const char *schema_dir,
                                       const char *data_dir,
                                       size_t request_body_limit,
                                       size_t sql_length_limit,
                                       char *error,
                                       size_t error_size) {
    SqlApiServerConfig config;
    SqlApiServer *server = NULL;

    sqlapi_server_config_set_defaults(&config);
    config.port = port;
    config.worker_count = worker_count;
    config.queue_capacity = queue_capacity;
    config.schema_dir = schema_dir;
    config.data_dir = data_dir;
    config.request_body_limit = request_body_limit;
    config.sql_length_limit = sql_length_limit;

    if (!sqlapi_server_create(&server, &config, error, error_size)) {
        return NULL;
    }

    if (!sqlapi_server_start(server, error, error_size)) {
        sqlapi_server_destroy(server);
        return NULL;
    }

    sleep_briefly();
    return server;
}

static void stop_test_server(SqlApiServer *server) {
    if (server == NULL) {
        return;
    }

    sqlapi_server_request_shutdown(server);
    sqlapi_server_wait(server);
    sqlapi_server_destroy(server);
}

static int open_blocking_request_connection(const char *host, int port, sql_socket_t *socket_out) {
    const char *request =
        "POST /query HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 64\r\n"
        "\r\n"
        "{";

    if (!connect_to_server(host, port, socket_out)) {
        return 0;
    }

    if (!send_all(*socket_out, request, strlen(request))) {
        sql_platform_close_socket(*socket_out);
        *socket_out = SQL_INVALID_SOCKET;
        return 0;
    }

    return 1;
}

static void drain_and_close_socket(sql_socket_t socket_fd) {
    char buffer[256];

    if (socket_fd == SQL_INVALID_SOCKET) {
        return;
    }

    CLOSE_WRITE(socket_fd);
    while (recv(socket_fd, buffer, (int)sizeof(buffer), 0) > 0) {
    }
    sql_platform_close_socket(socket_fd);
}

static int run_api_server_config_validation_tests(const char *schema_dir, const char *data_dir) {
    SqlApiServerConfig config;
    SqlApiServer *server = NULL;
    char error[256];
    char missing_path[256];
    int failures = 0;

    sqlapi_server_config_set_defaults(&config);
    config.schema_dir = schema_dir;
    config.data_dir = data_dir;

    config.port = 0;
    failures += !expect_true(!sqlapi_server_create(&server, &config, error, sizeof(error)),
                             "API server rejects port 0");
    failures += !expect_contains(error, "--port must be between 1 and 65535",
                                 "API server reports invalid port range");

    sqlapi_server_config_set_defaults(&config);
    config.schema_dir = schema_dir;
    config.data_dir = data_dir;
    config.worker_count = 0;
    failures += !expect_true(!sqlapi_server_create(&server, &config, error, sizeof(error)),
                             "API server rejects worker-count below 1");
    failures += !expect_contains(error, "--worker-count must be at least 1",
                                 "API server reports invalid worker-count");

    sqlapi_server_config_set_defaults(&config);
    config.schema_dir = schema_dir;
    config.data_dir = data_dir;
    config.queue_capacity = 0;
    failures += !expect_true(!sqlapi_server_create(&server, &config, error, sizeof(error)),
                             "API server rejects queue-capacity below 1");
    failures += !expect_contains(error, "--queue-capacity must be at least 1",
                                 "API server reports invalid queue-capacity");

    build_child_path(missing_path, sizeof(missing_path), schema_dir, "missing_schema_dir");
    sqlapi_server_config_set_defaults(&config);
    config.schema_dir = missing_path;
    config.data_dir = data_dir;
    failures += !expect_true(!sqlapi_server_create(&server, &config, error, sizeof(error)),
                             "API server rejects missing schema directory");
    failures += !expect_contains(error, "--schema-dir must point to an existing directory",
                                 "API server reports missing schema directory");

    build_child_path(missing_path, sizeof(missing_path), data_dir, "missing_data_dir");
    sqlapi_server_config_set_defaults(&config);
    config.schema_dir = schema_dir;
    config.data_dir = missing_path;
    failures += !expect_true(!sqlapi_server_create(&server, &config, error, sizeof(error)),
                             "API server rejects missing data directory");
    failures += !expect_contains(error, "--data-dir must point to an existing directory",
                                 "API server reports missing data directory");

    return failures;
}

static int run_api_server_queue_full_test(void) {
    char root[160];
    char schema_dir[192];
    char data_dir[192];
    char schema_path[224];
    char data_path[224];
    char response[131072];
    char error[256];
    SqlApiServerConfig config;
    SqlApiServer *server = NULL;
    sql_socket_t blocker_one = SQL_INVALID_SOCKET;
    sql_socket_t blocker_two = SQL_INVALID_SOCKET;
    int port = allocate_test_port();
    int failures = 0;

    if (port == 0) {
        fprintf(stderr, "[FAIL] allocate queue full test port\n");
        return 1;
    }

    if (!create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir))) {
        fprintf(stderr, "[FAIL] create queue full test directories\n");
        return 1;
    }

    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    if (!write_text_file(schema_path, "table=users\ncolumns=name,age\n") ||
        !write_text_file(data_path, "name,age\nAlice,20\nBob,21\n")) {
        fprintf(stderr, "[FAIL] write queue full test dataset\n");
        return 1;
    }

    sqlapi_server_config_set_defaults(&config);
    config.port = port;
    config.worker_count = 1;
    config.queue_capacity = 1;
    config.schema_dir = schema_dir;
    config.data_dir = data_dir;

    if (!sqlapi_server_create(&server, &config, error, sizeof(error))) {
        fprintf(stderr, "[FAIL] create queue full API server: %s\n", error);
        return 1;
    }

    if (!sqlapi_server_start(server, error, sizeof(error))) {
        fprintf(stderr, "[FAIL] start queue full API server: %s\n", error);
        sqlapi_server_destroy(server);
        return 1;
    }

    sleep_briefly();

    failures += !expect_true(open_blocking_request_connection("127.0.0.1", port, &blocker_one),
                             "queue full fixture opens first blocking connection");
    sleep_briefly();
    failures += !expect_true(open_blocking_request_connection("127.0.0.1", port, &blocker_two),
                             "queue full fixture opens second blocking connection");
    sleep_briefly();

    failures += !expect_true(sqlapi_server_queue_depth(server) == 1,
                             "queue full fixture fills one queued task");

    if (!send_http_request("127.0.0.1",
                           port,
                           "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send queue full probe request\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 503 Service Unavailable",
                                           "\"code\":\"QUEUE_FULL\"",
                                           "API server queue full");
    }

    drain_and_close_socket(blocker_one);
    drain_and_close_socket(blocker_two);

    sleep_briefly();
    sqlapi_server_request_shutdown(server);
    sqlapi_server_wait(server);
    sqlapi_server_destroy(server);
    return failures == 0 ? 0 : 1;
}

static int run_api_server_error_mapping_tests(void) {
    char root[160];
    char schema_dir[192];
    char data_dir[192];
    char path[224];
    char response[131072];
    char error[256];
    SqlApiServer *server;
    char *request;
    int port = allocate_test_port();
    int failures = 0;

    if (port == 0) {
        fprintf(stderr, "[FAIL] allocate API server error mapping test port\n");
        return 1;
    }

    if (!create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir))) {
        fprintf(stderr, "[FAIL] create API server error mapping test directories\n");
        return 1;
    }

    build_child_path(path, sizeof(path), schema_dir, "users.meta");
    if (!write_text_file(path, "table=users\ncolumns=name,age\n")) {
        fprintf(stderr, "[FAIL] write users schema for error mapping tests\n");
        return 1;
    }
    build_child_path(path, sizeof(path), data_dir, "users.csv");
    if (!write_text_file(path, "name,age\nAlice,20\n")) {
        fprintf(stderr, "[FAIL] write users data for error mapping tests\n");
        return 1;
    }

    build_child_path(path, sizeof(path), schema_dir, "badschema.meta");
    if (!write_text_file(path, "table=badschema\n")) {
        fprintf(stderr, "[FAIL] write badschema meta\n");
        return 1;
    }
    build_child_path(path, sizeof(path), data_dir, "badschema.csv");
    if (!write_text_file(path, "name,age\nAlice,20\n")) {
        fprintf(stderr, "[FAIL] write badschema CSV\n");
        return 1;
    }

    build_child_path(path, sizeof(path), schema_dir, "brokenexec.meta");
    if (!write_text_file(path, "table=brokenexec\ncolumns=name,age\n")) {
        fprintf(stderr, "[FAIL] write brokenexec meta\n");
        return 1;
    }
    build_child_path(path, sizeof(path), data_dir, "brokenexec.csv");
    if (!write_text_file(path, "name,age\nAlice,20,extra\n")) {
        fprintf(stderr, "[FAIL] write brokenexec CSV\n");
        return 1;
    }

    build_child_path(path, sizeof(path), schema_dir, "rebuildbad.meta");
    if (!write_text_file(path, "table=rebuildbad\ncolumns=name,age\n")) {
        fprintf(stderr, "[FAIL] write rebuildbad meta\n");
        return 1;
    }
    build_child_path(path, sizeof(path), data_dir, "rebuildbad.csv");
    if (!write_text_file(path, "name,age\nAlice,20,extra\n")) {
        fprintf(stderr, "[FAIL] write rebuildbad CSV\n");
        return 1;
    }

    server = start_test_server(port, 2, 8, schema_dir, data_dir, 16 * 1024, 8 * 1024, error, sizeof(error));
    if (server == NULL) {
        fprintf(stderr, "[FAIL] start API server error mapping fixture: %s\n", error);
        return 1;
    }

    request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT * FROM badschema;\"}");
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send SCHEMA_LOAD_ERROR request\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 500 Internal Server Error",
                                           "\"code\":\"SCHEMA_LOAD_ERROR\"",
                                           "POST /query with malformed schema meta");
    }
    free(request);

    request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT * FROM brokenexec;\"}");
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send ENGINE_EXECUTION_ERROR request\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 500 Internal Server Error",
                                           "\"code\":\"ENGINE_EXECUTION_ERROR\"",
                                           "POST /query with malformed execution row");
    }
    free(request);

    request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM rebuildbad WHERE id = 1;\"}");
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send INDEX_REBUILD_ERROR request\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 500 Internal Server Error",
                                           "\"code\":\"INDEX_REBUILD_ERROR\"",
                                           "POST /query with malformed index rebuild source");
    }
    free(request);

    sql_engine_adapter_test_force_next_error(SQL_ENGINE_ERROR_INTERNAL_ERROR);
    request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT * FROM users;\"}");
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send INTERNAL_ERROR request\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 500 Internal Server Error",
                                           "\"code\":\"INTERNAL_ERROR\"",
                                           "POST /query with forced internal error");
    }
    free(request);

    sql_engine_adapter_test_clear_hooks();
    stop_test_server(server);
    return failures == 0 ? 0 : 1;
}

static int run_api_server_boundary_tests(void) {
    char root[160];
    char schema_dir[192];
    char data_dir[192];
    char path[224];
    char response[131072];
    char error[256];
    SqlApiServer *server;
    char *request = NULL;
    char *body = NULL;
    char *sql = NULL;
    int port = allocate_test_port();
    int failures = 0;

    if (port == 0) {
        fprintf(stderr, "[FAIL] allocate API server boundary test port\n");
        return 1;
    }

    if (!create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir))) {
        fprintf(stderr, "[FAIL] create API server boundary test directories\n");
        return 1;
    }

    build_child_path(path, sizeof(path), schema_dir, "users.meta");
    if (!write_text_file(path, "table=users\ncolumns=name,age\n")) {
        fprintf(stderr, "[FAIL] write users schema for boundary tests\n");
        return 1;
    }
    build_child_path(path, sizeof(path), data_dir, "users.csv");
    if (!write_text_file(path, "name,age\nAlice,20\nBob,21\n")) {
        fprintf(stderr, "[FAIL] write users data for boundary tests\n");
        return 1;
    }

    server = start_test_server(port, 2, 8, schema_dir, data_dir, 16 * 1024, 8 * 1024, error, sizeof(error));
    if (server == NULL) {
        fprintf(stderr, "[FAIL] start API server boundary fixture: %s\n", error);
        return 1;
    }

    request = build_exact_header_request(8192);
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send exact header limit request\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 200 OK", "GET /health with 8192-byte header returns 200");
    }
    free(request);

    request = build_exact_header_request(8193);
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send oversized exact header request\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 431 Request Header Fields Too Large",
                                           "\"code\":\"HEADER_TOO_LARGE\"",
                                           "GET /health with 8193-byte header");
    }
    free(request);

    body = build_query_json_with_padding("SELECT name FROM users WHERE age = 20;", 16 * 1024);
    request = body == NULL ? NULL : build_post_request("/query", "application/json", body);
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send exact body limit request\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 200 OK", "POST /query with 16384-byte body returns 200");
        failures += !expect_contains(response, "Alice", "POST /query with 16384-byte body still executes SQL");
    }
    free(body);
    free(request);

    body = build_query_json_with_padding("SELECT name FROM users WHERE age = 20;", 16 * 1024 + 1);
    request = body == NULL ? NULL : build_post_request("/query", "application/json", body);
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send oversized body limit request\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 413 Payload Too Large",
                                           "\"code\":\"PAYLOAD_TOO_LARGE\"",
                                           "POST /query with 16385-byte body");
    }
    free(body);
    free(request);

    sql = build_insert_sql_with_length(8 * 1024);
    body = sql == NULL ? NULL : build_query_json_with_padding(sql, strlen(sql) + strlen("{\"sql\":\"\",\"pad\":\"\"}"));
    request = body == NULL ? NULL : build_post_request("/query", "application/json", body);
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send exact SQL limit request\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 200 OK", "POST /query with 8192-byte SQL returns 200");
        failures += !expect_contains(response, "\"statement_type\":\"insert\"", "POST /query with 8192-byte SQL executes insert");
    }
    free(sql);
    free(body);
    free(request);

    sql = build_insert_sql_with_length(8 * 1024 + 1);
    body = sql == NULL ? NULL : build_query_json_with_padding(sql, strlen(sql) + strlen("{\"sql\":\"\",\"pad\":\"\"}"));
    request = body == NULL ? NULL : build_post_request("/query", "application/json", body);
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send oversized SQL limit request\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 413 Payload Too Large",
                                           "\"code\":\"PAYLOAD_TOO_LARGE\"",
                                           "POST /query with 8193-byte SQL");
    }
    free(sql);
    free(body);
    free(request);

    if (!send_http_request("127.0.0.1",
                           port,
                           "GET /health HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "X-Test: first\r\n"
                           " second-line\r\n"
                           "\r\n",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send folded header request\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"INVALID_CONTENT_LENGTH\"",
                                           "GET /health with folded header");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "POST /health HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /health\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 405 Method Not Allowed",
                                           "\"code\":\"METHOD_NOT_ALLOWED\"",
                                           "POST /health");
    }

    request = build_post_request("/query", "application/json", "{\"sql\":\"   \\r\\n\\t \"}");
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send blank SQL request\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"INVALID_SQL_ARGUMENT\"",
                                           "POST /query with blank SQL");
    }
    free(request);

    stop_test_server(server);
    return failures == 0 ? 0 : 1;
}

static int run_api_server_concurrency_tests(void) {
    char root[160];
    char schema_dir[192];
    char data_dir[192];
    char path[224];
    char error[256];
    SqlApiServer *server;
    AsyncHttpRequest request_one = {0};
    AsyncHttpRequest request_two = {0};
    pthread_t thread_one;
    pthread_t thread_two;
    double elapsed_ms;
    int port = allocate_test_port();
    int failures = 0;

    if (port == 0) {
        fprintf(stderr, "[FAIL] allocate API server concurrency test port\n");
        return 1;
    }

    if (!create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir))) {
        fprintf(stderr, "[FAIL] create API server concurrency test directories\n");
        return 1;
    }

    build_child_path(path, sizeof(path), schema_dir, "users.meta");
    if (!write_text_file(path, "table=users\ncolumns=name,age\n")) {
        fprintf(stderr, "[FAIL] write users schema for concurrency tests\n");
        return 1;
    }
    build_child_path(path, sizeof(path), data_dir, "users.csv");
    if (!write_text_file(path, "name,age\nAlice,20\nBob,21\n")) {
        fprintf(stderr, "[FAIL] write users data for concurrency tests\n");
        return 1;
    }

    build_child_path(path, sizeof(path), schema_dir, "products.meta");
    if (!write_text_file(path, "table=products\ncolumns=name,price\n")) {
        fprintf(stderr, "[FAIL] write products schema for concurrency tests\n");
        return 1;
    }
    build_child_path(path, sizeof(path), data_dir, "products.csv");
    if (!write_text_file(path, "name,price\nKeyboard,100\nMouse,50\n")) {
        fprintf(stderr, "[FAIL] write products data for concurrency tests\n");
        return 1;
    }

    server = start_test_server(port, 2, 8, schema_dir, data_dir, 16 * 1024, 8 * 1024, error, sizeof(error));
    if (server == NULL) {
        fprintf(stderr, "[FAIL] start API server concurrency fixture: %s\n", error);
        return 1;
    }

    sql_engine_adapter_test_set_delay_after_lock_ms(250);
    request_one.host = "127.0.0.1";
    request_one.port = port;
    request_one.request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM users WHERE age = 20;\"}");
    request_two.host = "127.0.0.1";
    request_two.port = port;
    request_two.request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM users WHERE age = 21;\"}");
    elapsed_ms = now_ms();
    pthread_create(&thread_one, NULL, run_async_http_request, &request_one);
    pthread_create(&thread_two, NULL, run_async_http_request, &request_two);
    pthread_join(thread_one, NULL);
    pthread_join(thread_two, NULL);
    elapsed_ms = now_ms() - elapsed_ms;
    failures += !expect_true(request_one.ok && request_two.ok, "same-table concurrent requests complete");
    failures += !expect_contains(request_one.response, "HTTP/1.1 200 OK", "same-table first request returns 200");
    failures += !expect_contains(request_two.response, "HTTP/1.1 200 OK", "same-table second request returns 200");
    failures += !expect_true(elapsed_ms >= 430.0, "same-table concurrent requests are serialized");
    free((char *)request_one.request);
    free((char *)request_two.request);

    request_one = (AsyncHttpRequest){0};
    request_two = (AsyncHttpRequest){0};
    request_one.host = "127.0.0.1";
    request_one.port = port;
    request_one.request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM users WHERE age = 20;\"}");
    request_two.host = "127.0.0.1";
    request_two.port = port;
    request_two.request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM products WHERE price = 50;\"}");
    elapsed_ms = now_ms();
    pthread_create(&thread_one, NULL, run_async_http_request, &request_one);
    pthread_create(&thread_two, NULL, run_async_http_request, &request_two);
    pthread_join(thread_one, NULL);
    pthread_join(thread_two, NULL);
    elapsed_ms = now_ms() - elapsed_ms;
    failures += !expect_true(request_one.ok && request_two.ok, "different-table concurrent requests complete");
    failures += !expect_contains(request_one.response, "HTTP/1.1 200 OK", "different-table users request returns 200");
    failures += !expect_contains(request_two.response, "HTTP/1.1 200 OK", "different-table products request returns 200");
    failures += !expect_true(elapsed_ms < 430.0, "different-table requests run in parallel");
    free((char *)request_one.request);
    free((char *)request_two.request);

    request_one = (AsyncHttpRequest){0};
    request_two = (AsyncHttpRequest){0};
    sql_engine_adapter_test_force_next_error(SQL_ENGINE_ERROR_INTERNAL_ERROR);
    request_one.host = "127.0.0.1";
    request_one.port = port;
    request_one.request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT * FROM users;\"}");
    request_two.host = "127.0.0.1";
    request_two.port = port;
    request_two.request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM users WHERE age = 21;\"}");
    pthread_create(&thread_one, NULL, run_async_http_request, &request_one);
    sleep_millis(20);
    pthread_create(&thread_two, NULL, run_async_http_request, &request_two);
    pthread_join(thread_one, NULL);
    pthread_join(thread_two, NULL);
    failures += !expect_true(request_one.ok && request_two.ok, "failure path requests both return responses");
    failures += !expect_error_response(request_one.response,
                                       "HTTP/1.1 500 Internal Server Error",
                                       "\"code\":\"INTERNAL_ERROR\"",
                                       "same-table forced failure releases lock first request");
    failures += !expect_contains(request_two.response, "HTTP/1.1 200 OK", "same-table forced failure still releases lock for next request");
    sql_engine_adapter_test_clear_hooks();
    free((char *)request_one.request);
    free((char *)request_two.request);

    stop_test_server(server);
    return failures == 0 ? 0 : 1;
}

static int run_api_server_alias_lock_test(void) {
    char root[160];
    char schema_dir[192];
    char data_dir[192];
    char path[224];
    char error[256];
    SqlApiServer *server;
    AsyncHttpRequest request_one = {0};
    AsyncHttpRequest request_two = {0};
    pthread_t thread_one;
    pthread_t thread_two;
    double elapsed_ms;
    int port = allocate_test_port();
    int failures = 0;

    if (port == 0) {
        fprintf(stderr, "[FAIL] allocate API server alias lock test port\n");
        return 1;
    }

    if (!create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir))) {
        fprintf(stderr, "[FAIL] create API server alias lock test directories\n");
        return 1;
    }

    build_child_path(path, sizeof(path), schema_dir, "users_alias.meta");
    if (!write_text_file(path, "table=users\ncolumns=name,age\n")) {
        fprintf(stderr, "[FAIL] write alias schema meta\n");
        return 1;
    }
    build_child_path(path, sizeof(path), data_dir, "users_alias.csv");
    if (!write_text_file(path, "name,age\nAlice,20\nBob,21\n")) {
        fprintf(stderr, "[FAIL] write alias schema CSV\n");
        return 1;
    }

    server = start_test_server(port, 2, 8, schema_dir, data_dir, 16 * 1024, 8 * 1024, error, sizeof(error));
    if (server == NULL) {
        fprintf(stderr, "[FAIL] start API server alias lock fixture: %s\n", error);
        return 1;
    }

    sql_engine_adapter_test_set_delay_after_lock_ms(250);
    request_one.host = "127.0.0.1";
    request_one.port = port;
    request_one.request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM users WHERE age = 20;\"}");
    request_two.host = "127.0.0.1";
    request_two.port = port;
    request_two.request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM users_alias WHERE age = 21;\"}");
    elapsed_ms = now_ms();
    pthread_create(&thread_one, NULL, run_async_http_request, &request_one);
    pthread_create(&thread_two, NULL, run_async_http_request, &request_two);
    pthread_join(thread_one, NULL);
    pthread_join(thread_two, NULL);
    elapsed_ms = now_ms() - elapsed_ms;
    failures += !expect_true(request_one.ok && request_two.ok, "alias/storage concurrent requests complete");
    failures += !expect_contains(request_one.response, "HTTP/1.1 200 OK", "alias lock first request returns 200");
    failures += !expect_contains(request_two.response, "HTTP/1.1 200 OK", "alias lock second request returns 200");
    failures += !expect_true(elapsed_ms >= 430.0, "alias name and storage name share one lock");

    sql_engine_adapter_test_clear_hooks();
    free((char *)request_one.request);
    free((char *)request_two.request);
    stop_test_server(server);
    return failures == 0 ? 0 : 1;
}

static int run_api_server_shutdown_test(void) {
    char root[160];
    char schema_dir[192];
    char data_dir[192];
    char path[224];
    char error[256];
    SqlApiServer *server;
    AsyncHttpRequest request_one = {0};
    AsyncHttpRequest request_two = {0};
    pthread_t thread_one;
    pthread_t thread_two;
    sql_socket_t probe_socket = SQL_INVALID_SOCKET;
    int port = allocate_test_port();
    int failures = 0;

    if (port == 0) {
        fprintf(stderr, "[FAIL] allocate API server shutdown test port\n");
        return 1;
    }

    if (!create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir))) {
        fprintf(stderr, "[FAIL] create API server shutdown test directories\n");
        return 1;
    }

    build_child_path(path, sizeof(path), schema_dir, "users.meta");
    if (!write_text_file(path, "table=users\ncolumns=name,age\n")) {
        fprintf(stderr, "[FAIL] write users schema for shutdown tests\n");
        return 1;
    }
    build_child_path(path, sizeof(path), data_dir, "users.csv");
    if (!write_text_file(path, "name,age\nAlice,20\nBob,21\n")) {
        fprintf(stderr, "[FAIL] write users data for shutdown tests\n");
        return 1;
    }

    build_child_path(path, sizeof(path), schema_dir, "products.meta");
    if (!write_text_file(path, "table=products\ncolumns=name,price\n")) {
        fprintf(stderr, "[FAIL] write products schema for shutdown tests\n");
        return 1;
    }
    build_child_path(path, sizeof(path), data_dir, "products.csv");
    if (!write_text_file(path, "name,price\nKeyboard,100\nMouse,50\n")) {
        fprintf(stderr, "[FAIL] write products data for shutdown tests\n");
        return 1;
    }

    server = start_test_server(port, 1, 2, schema_dir, data_dir, 16 * 1024, 8 * 1024, error, sizeof(error));
    if (server == NULL) {
        fprintf(stderr, "[FAIL] start API server shutdown fixture: %s\n", error);
        return 1;
    }

    sql_engine_adapter_test_set_delay_after_lock_ms(250);
    request_one.host = "127.0.0.1";
    request_one.port = port;
    request_one.request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM users WHERE age = 20;\"}");
    request_two.host = "127.0.0.1";
    request_two.port = port;
    request_two.request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM products WHERE price = 50;\"}");

    pthread_create(&thread_one, NULL, run_async_http_request, &request_one);
    sleep_millis(20);
    pthread_create(&thread_two, NULL, run_async_http_request, &request_two);

    failures += !expect_true(wait_for_queue_depth(server, 1, 1000), "shutdown fixture queues one pending request");
    sqlapi_server_request_shutdown(server);

    pthread_join(thread_one, NULL);
    pthread_join(thread_two, NULL);
    failures += !expect_true(request_one.ok && request_two.ok, "graceful shutdown drains in-flight and queued requests");
    failures += !expect_contains(request_one.response, "HTTP/1.1 200 OK", "graceful shutdown first request returns 200");
    failures += !expect_contains(request_two.response, "HTTP/1.1 200 OK", "graceful shutdown queued request returns 200");

    sqlapi_server_wait(server);
    failures += !expect_true(!connect_to_server("127.0.0.1", port, &probe_socket),
                             "shutdown closes listen socket to new connections");
    if (probe_socket != SQL_INVALID_SOCKET) {
        sql_platform_close_socket(probe_socket);
    }

    sql_engine_adapter_test_clear_hooks();
    free((char *)request_one.request);
    free((char *)request_two.request);
    sqlapi_server_destroy(server);
    return failures == 0 ? 0 : 1;
}

static int run_api_server_restart_recovery_test(void) {
    char root[160];
    char schema_dir[192];
    char data_dir[192];
    char path[224];
    char response[131072];
    char error[256];
    SqlApiServer *server = NULL;
    char *request = NULL;
    int port = allocate_test_port();
    int restart_port = allocate_test_port();
    int failures = 0;

    if (port == 0 || restart_port == 0) {
        fprintf(stderr, "[FAIL] allocate API server restart test ports\n");
        return 1;
    }

    if (!create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir))) {
        fprintf(stderr, "[FAIL] create API server restart test directories\n");
        return 1;
    }

    build_child_path(path, sizeof(path), schema_dir, "users.meta");
    if (!write_text_file(path, "table=users\ncolumns=name,age\n")) {
        fprintf(stderr, "[FAIL] write users schema for restart tests\n");
        return 1;
    }
    build_child_path(path, sizeof(path), data_dir, "users.csv");
    if (!write_text_file(path, "name,age\nAlice,20\nBob,21\n")) {
        fprintf(stderr, "[FAIL] write users data for restart tests\n");
        return 1;
    }

    server = start_test_server(port, 2, 8, schema_dir, data_dir, 16 * 1024, 8 * 1024, error, sizeof(error));
    if (server == NULL) {
        fprintf(stderr, "[FAIL] start API server restart fixture: %s\n", error);
        return 1;
    }

    request = build_post_request("/query", "application/json", "{\"sql\":\"INSERT INTO users (name, age) VALUES ('Carol', 25);\"}");
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send restart INSERT request\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 200 OK", "restart fixture INSERT returns 200");
    }
    free(request);

    request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM users WHERE id = 3;\"}");
    if (request == NULL || !send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send restart pre-shutdown SELECT request\n");
        failures++;
    } else {
        failures += !expect_contains(response, "Carol", "restart fixture SELECT finds inserted row before shutdown");
    }
    free(request);

    stop_test_server(server);

    server = start_test_server(restart_port, 2, 8, schema_dir, data_dir, 16 * 1024, 8 * 1024, error, sizeof(error));
    if (server == NULL) {
        fprintf(stderr, "[FAIL] restart API server fixture: %s\n", error);
        return 1;
    }

    request = build_post_request("/query", "application/json", "{\"sql\":\"SELECT name FROM users WHERE id = 3;\"}");
    if (request == NULL || !send_http_request("127.0.0.1", restart_port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send restart post-start SELECT request\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 200 OK", "restart fixture SELECT after restart returns 200");
        failures += !expect_contains(response, "Carol", "restart fixture SELECT after restart rebuilds and finds persisted row");
    }
    free(request);

    stop_test_server(server);
    return failures == 0 ? 0 : 1;
}

int run_api_server_tests(void) {
    char root[160];
    char schema_dir[192];
    char data_dir[192];
    char schema_path[224];
    char data_path[224];
    char response[131072];
    char request[2048];
    SqlApiServerConfig config;
    SqlApiServer *server = NULL;
    char error[256];
    int port = allocate_test_port();
    int failures = 0;
    const char *json_body = "{\"sql\":\"SELECT name FROM users WHERE age = 20;\"}";
    const char *missing_sql_body = "{\"foo\":\"bar\"}";
    const char *parse_error_body = "{\"sql\":\"SELECT FROM users;\"}";
    const char *lex_error_body = "{\"sql\":\"SELECT name FROM users WHERE age = @;\"}";
    const char *multi_statement_body = "{\"sql\":\"SELECT name FROM users; SELECT age FROM users;\"}";
    const char *unsupported_sql_body = "{\"sql\":\"UPDATE users SET age = 30;\"}";
    const char *invalid_sql_argument_body = "{\"sql\":\"SELECT name FROM users WHERE id = abc;\"}";

    if (port == 0) {
        fprintf(stderr, "[FAIL] allocate API server test port\n");
        return 1;
    }

    if (!create_test_dirs(root, sizeof(root), schema_dir, sizeof(schema_dir), data_dir, sizeof(data_dir))) {
        fprintf(stderr, "[FAIL] create API server test directories\n");
        return 1;
    }

    failures += run_api_server_config_validation_tests(schema_dir, data_dir);

    build_child_path(schema_path, sizeof(schema_path), schema_dir, "users.meta");
    build_child_path(data_path, sizeof(data_path), data_dir, "users.csv");
    if (!write_text_file(schema_path, "table=users\ncolumns=name,age\n") ||
        !write_text_file(data_path, "name,age\nAlice,20\nBob,21\nCarol,25\nDave,25\n")) {
        fprintf(stderr, "[FAIL] write API server test dataset\n");
        return 1;
    }

    sqlapi_server_config_set_defaults(&config);
    config.port = port;
    config.worker_count = 2;
    config.queue_capacity = 8;
    config.schema_dir = schema_dir;
    config.data_dir = data_dir;

    if (!sqlapi_server_create(&server, &config, error, sizeof(error))) {
        fprintf(stderr, "[FAIL] create API server: %s\n", error);
        return 1;
    }

    if (!sqlapi_server_start(server, error, sizeof(error))) {
        fprintf(stderr, "[FAIL] start API server: %s\n", error);
        sqlapi_server_destroy(server);
        return 1;
    }

    sleep_briefly();

    if (!send_http_request("127.0.0.1",
                           port,
                           "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send GET /\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 200 OK", "GET / returns 200");
        failures += !expect_contains(response, "Content-Type: text/html; charset=utf-8", "GET / returns HTML content type");
        failures += !expect_contains(response, "<title>SQL API Console</title>", "GET / returns root page title");
        failures += !expect_contains(response, "POST /query", "GET / documents query endpoint");
        failures += !expect_contains(response, "textarea id=\"sql-input\"", "GET / returns SQL input console");
        failures += !expect_contains(response, "help, .help, --help", "GET / documents browser help commands");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "GET / HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Content-Length: 2\r\n"
                           "\r\n"
                           "{}",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send GET / with body\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"INVALID_JSON\"",
                                           "GET / with body");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send GET /health\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 200 OK", "GET /health returns 200");
        failures += !expect_contains(response, "Content-Type: application/json; charset=utf-8", "GET /health returns JSON content type");
        failures += !expect_contains(response, "\"ok\":true", "GET /health returns ok flag");
        failures += !expect_contains(response, "\"status\":\"ok\"", "GET /health returns ok status");
        failures += !expect_contains(response, "\"worker_count\":2", "GET /health reports worker count");
        failures += !expect_contains(response, "\"queue_depth\":0", "GET /health reports queue depth");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 405 Method Not Allowed",
                                           "\"code\":\"METHOD_NOT_ALLOWED\"",
                                           "POST /");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send GET /health with Content-Length 0\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 200 OK", "GET /health with Content-Length 0 returns 200");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "GET /health HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Content-Length: 2\r\n"
                           "\r\n"
                           "{}",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send GET /health with body\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"INVALID_JSON\"",
                                           "GET /health with body");
    }

    snprintf(request,
             sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json; charset=utf-8\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(json_body),
             json_body);
    if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with charset=utf-8\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 200 OK", "POST /query with charset=utf-8 returns 200");
        failures += !expect_contains(response, "Content-Type: application/json; charset=utf-8", "POST /query with charset=utf-8 returns JSON content type");
    }

    snprintf(request,
             sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(json_body),
             json_body);
    if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 200 OK", "POST /query returns 200");
        failures += !expect_contains(response, "\"statement_type\":\"select\"", "POST /query reports select type");
        failures += !expect_contains(response, "Alice", "POST /query returns SELECT output");
        failures += !expect_contains(response, "\"output\":\"+", "POST /query returns table output body");
        failures += !expect_contains(response, "\\n|", "POST /query serializes row breaks with LF escapes");
        failures += !expect_not_contains(response, "\\r\\n| id | name |", "POST /query output does not serialize CRLF escapes");
        failures += !expect_not_contains(response, "\\r\\n| 1  | Alice |", "POST /query row output does not serialize CRLF escapes");
        failures += !expect_not_contains(response, "\r\n| id | name |", "POST /query JSON body does not contain raw CRLF row separators");
        failures += !expect_not_contains(response, "\r\n| 1  | Alice |", "POST /query JSON body rows are not split by raw CRLF");
    }

    snprintf(request,
             sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen("{\"sql\":\"SELECT * FROM users WHERE age = 25;\"}"),
             "{\"sql\":\"SELECT * FROM users WHERE age = 25;\"}");
    if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with multi-row WHERE age = 25\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 200 OK", "POST /query with multi-row WHERE returns 200");
        failures += !expect_contains(response, "\"statement_type\":\"select\"", "POST /query with multi-row WHERE reports select type");
        failures += !expect_contains(response, "\"affected_rows\":", "POST /query with multi-row WHERE includes affected_rows field");
        failures += !expect_contains(response, "Carol", "POST /query with multi-row WHERE includes first matching row");
        failures += !expect_contains(response, "Dave", "POST /query with multi-row WHERE includes second matching row");
        failures += !expect_contains(response, "| age |", "POST /query with multi-row WHERE returns age column");
    }

    snprintf(request,
             sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(json_body),
             json_body);
    if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with invalid Content-Type\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"INVALID_CONTENT_TYPE\"",
                                           "POST /query with invalid Content-Type");
    }

    snprintf(request,
             sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json; charset=euc-kr\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(json_body),
             json_body);
    if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with invalid charset\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"INVALID_CONTENT_TYPE\"",
                                           "POST /query with invalid charset");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "POST /query HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: 10\r\n"
                           "\r\n"
                           "{\"sql\":123",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with invalid JSON\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"INVALID_JSON\"",
                                           "POST /query with invalid JSON");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "POST /query HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: abc\r\n"
                           "\r\n",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with non-numeric Content-Length\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"INVALID_CONTENT_LENGTH\"",
                                           "POST /query with non-numeric Content-Length");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "POST /query HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: 100\r\n"
                           "\r\n"
                           "{\"sql\":\"SELECT 1;\"}",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with truncated body\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"INVALID_CONTENT_LENGTH\"",
                                           "POST /query with truncated body");
    }

    snprintf(request,
             sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(missing_sql_body),
             missing_sql_body);
    if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query without sql field\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"MISSING_SQL_FIELD\"",
                                           "POST /query without sql field");
    }

    snprintf(request,
             sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(parse_error_body),
             parse_error_body);
    if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with parse error SQL\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"SQL_PARSE_ERROR\"",
                                           "POST /query with parse error SQL");
    }

    snprintf(request,
             sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(lex_error_body),
             lex_error_body);
    if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with lexer error SQL\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"SQL_LEX_ERROR\"",
                                           "POST /query with lexer error SQL");
    }

    snprintf(request,
             sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(multi_statement_body),
             multi_statement_body);
    if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with multi-statement SQL\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"SQL_PARSE_ERROR\"",
                                           "POST /query with multi-statement SQL");
    }

    snprintf(request,
             sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(unsupported_sql_body),
             unsupported_sql_body);
    if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with unsupported SQL\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"UNSUPPORTED_SQL\"",
                                           "POST /query with unsupported SQL");
    }

    snprintf(request,
             sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(invalid_sql_argument_body),
             invalid_sql_argument_body);
    if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with invalid SQL argument\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 400 Bad Request",
                                           "\"code\":\"INVALID_SQL_ARGUMENT\"",
                                           "POST /query with invalid SQL argument");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "POST /query HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Content-Type: application/json\r\n"
                           "\r\n"
                           "{\"sql\":\"SELECT * FROM users;\"}",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query without Content-Length\n");
        failures++;
    } else {
        failures += !expect_contains(response, "HTTP/1.1 411 Length Required", "POST /query without Content-Length returns 411");
        failures += !expect_contains(response, "\"code\":\"CONTENT_LENGTH_REQUIRED\"", "POST /query without Content-Length returns correct error code");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "GET /query HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send GET /query\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 405 Method Not Allowed",
                                           "\"code\":\"METHOD_NOT_ALLOWED\"",
                                           "GET /query");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "GET /missing HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send GET /missing\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 404 Not Found",
                                           "\"code\":\"NOT_FOUND\"",
                                           "GET /missing");
    }

    if (!send_http_request("127.0.0.1",
                           port,
                           "POST /query HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Transfer-Encoding: chunked\r\n"
                           "Content-Type: application/json\r\n"
                           "\r\n"
                           "4\r\n"
                           "test\r\n"
                           "0\r\n\r\n",
                           response,
                           sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with chunked encoding\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 501 Not Implemented",
                                           "\"code\":\"CHUNKED_NOT_SUPPORTED\"",
                                           "POST /query with chunked encoding");
    }

    if (!send_large_http_request("127.0.0.1",
                                 port,
                                 "GET /health HTTP/1.1\r\nX-Fill: ",
                                 8200,
                                 'a',
                                 "\r\n\r\n",
                                 response,
                                 sizeof(response))) {
        fprintf(stderr, "[FAIL] send GET /health with oversized header\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 431 Request Header Fields Too Large",
                                           "\"code\":\"HEADER_TOO_LARGE\"",
                                           "GET /health with oversized header");
    }

    if (!send_large_http_request("127.0.0.1",
                                 port,
                                 "POST /query HTTP/1.1\r\n"
                                 "Host: 127.0.0.1\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: 16385\r\n"
                                 "\r\n"
                                 "{\"sql\":\"",
                                 16376,
                                 'a',
                                 "\"}",
                                 response,
                                 sizeof(response))) {
        fprintf(stderr, "[FAIL] send POST /query with oversized body\n");
        failures++;
    } else {
        failures += !expect_error_response(response,
                                           "HTTP/1.1 413 Payload Too Large",
                                           "\"code\":\"PAYLOAD_TOO_LARGE\"",
                                           "POST /query with oversized body");
    }

    if (remove(data_path) != 0) {
        fprintf(stderr, "[FAIL] remove API server test data file\n");
        failures++;
    } else {
        snprintf(request,
                 sizeof(request),
                 "POST /query HTTP/1.1\r\n"
                 "Host: 127.0.0.1\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n"
                 "%s",
                 strlen(json_body),
                 json_body);
        if (!send_http_request("127.0.0.1", port, request, response, sizeof(response))) {
            fprintf(stderr, "[FAIL] send POST /query with missing data file\n");
            failures++;
        } else {
            failures += !expect_error_response(response,
                                               "HTTP/1.1 500 Internal Server Error",
                                               "\"code\":\"STORAGE_IO_ERROR\"",
                                               "POST /query with missing data file");
        }
    }

    sqlapi_server_request_shutdown(server);
    sqlapi_server_wait(server);
    sqlapi_server_destroy(server);

    failures += run_api_server_queue_full_test();
    failures += run_api_server_error_mapping_tests();
    failures += run_api_server_boundary_tests();
    failures += run_api_server_concurrency_tests();
    failures += run_api_server_alias_lock_test();
    failures += run_api_server_shutdown_test();
    failures += run_api_server_restart_recovery_test();
    return failures == 0 ? 0 : 1;
}
