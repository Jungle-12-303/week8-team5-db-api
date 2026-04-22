#ifndef SQLPARSER_HTTP_HTTP_RESPONSE_H
#define SQLPARSER_HTTP_HTTP_RESPONSE_H

#include "sqlparser/common/platform.h"
#include "sqlparser/engine/sql_engine_adapter.h"

#include <stddef.h>

typedef struct {
    int status_code;
    char *content_type;
    char *body;
    size_t body_length;
} HttpResponse;

void http_response_init(HttpResponse *response);
void http_response_free(HttpResponse *response);
int http_response_send(sql_socket_t socket_fd, const HttpResponse *response);
int http_response_set_body(HttpResponse *response, int status_code, const char *content_type, const char *body);
int http_response_set_json(HttpResponse *response, int status_code, const char *json_body);
int http_response_set_html(HttpResponse *response, int status_code, const char *html_body);
int http_response_set_error(HttpResponse *response, SqlEngineErrorCode code, const char *message);
int http_status_from_engine_error(SqlEngineErrorCode code);
const char *http_reason_phrase(int status_code);
char *http_json_escape(const char *value);

#endif
