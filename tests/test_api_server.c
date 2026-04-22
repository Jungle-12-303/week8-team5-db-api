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
#include "sqlparser/server/server.h"

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

int run_api_server_tests(void) {
    char root[160];
    char schema_dir[192];
    char data_dir[192];
    char schema_path[224];
    char data_path[224];
    char response[8192];
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
    return failures == 0 ? 0 : 1;
}
