#ifndef SQLPARSER_HTTP_HTTP_REQUEST_H
#define SQLPARSER_HTTP_HTTP_REQUEST_H

#include "sqlparser/common/platform.h"
#include "sqlparser/engine/sql_engine_adapter.h"

#include <stddef.h>

typedef struct {
    char *name;
    char *value;
} HttpHeader;

typedef struct {
    char method[16];
    char path[128];
    char version[16];
    HttpHeader *headers;
    int header_count;
    char *body;
    size_t body_length;
} HttpRequest;

typedef struct {
    int ok;
    SqlEngineErrorCode error_code;
    char message[256];
} HttpRequestReadResult;

void http_request_init(HttpRequest *request);
void http_request_free(HttpRequest *request);
const char *http_request_get_header(const HttpRequest *request, const char *name);
int http_read_request(sql_socket_t socket_fd,
                      size_t header_limit,
                      size_t body_limit,
                      HttpRequest *request,
                      HttpRequestReadResult *result);

#endif
