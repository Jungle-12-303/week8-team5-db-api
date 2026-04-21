#include "sqlparser/http/http_response.h"

#include "sqlparser/common/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void http_response_init(HttpResponse *response) {
    memset(response, 0, sizeof(*response));
}

void http_response_free(HttpResponse *response) {
    free(response->body);
    memset(response, 0, sizeof(*response));
}

const char *http_reason_phrase(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
    }

    return "Internal Server Error";
}

int http_status_from_engine_error(SqlEngineErrorCode code) {
    switch (code) {
        case SQL_ENGINE_ERROR_INVALID_JSON:
        case SQL_ENGINE_ERROR_MISSING_SQL_FIELD:
        case SQL_ENGINE_ERROR_INVALID_CONTENT_TYPE:
        case SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH:
        case SQL_ENGINE_ERROR_SQL_LEX_ERROR:
        case SQL_ENGINE_ERROR_SQL_PARSE_ERROR:
        case SQL_ENGINE_ERROR_UNSUPPORTED_SQL:
        case SQL_ENGINE_ERROR_INVALID_SQL_ARGUMENT:
            return 400;
        case SQL_ENGINE_ERROR_NOT_FOUND:
            return 404;
        case SQL_ENGINE_ERROR_METHOD_NOT_ALLOWED:
            return 405;
        case SQL_ENGINE_ERROR_CONTENT_LENGTH_REQUIRED:
            return 411;
        case SQL_ENGINE_ERROR_PAYLOAD_TOO_LARGE:
            return 413;
        case SQL_ENGINE_ERROR_HEADER_TOO_LARGE:
            return 431;
        case SQL_ENGINE_ERROR_CHUNKED_NOT_SUPPORTED:
            return 501;
        case SQL_ENGINE_ERROR_QUEUE_FULL:
            return 503;
        case SQL_ENGINE_ERROR_SCHEMA_LOAD_ERROR:
        case SQL_ENGINE_ERROR_STORAGE_IO_ERROR:
        case SQL_ENGINE_ERROR_INDEX_REBUILD_ERROR:
        case SQL_ENGINE_ERROR_ENGINE_EXECUTION_ERROR:
        case SQL_ENGINE_ERROR_INTERNAL_ERROR:
        case SQL_ENGINE_ERROR_NONE:
            return 500;
    }

    return 500;
}

char *http_json_escape(const char *value) {
    size_t length = 0;
    char *escaped;
    size_t index;
    size_t offset = 0;

    for (index = 0; value[index] != '\0'; index++) {
        switch (value[index]) {
            case '\\':
            case '"':
            case '\n':
            case '\r':
            case '\t':
                length += 2;
                break;
            default:
                length += 1;
                break;
        }
    }

    escaped = (char *)malloc(length + 1);
    if (escaped == NULL) {
        return NULL;
    }

    for (index = 0; value[index] != '\0'; index++) {
        switch (value[index]) {
            case '\\':
                escaped[offset++] = '\\';
                escaped[offset++] = '\\';
                break;
            case '"':
                escaped[offset++] = '\\';
                escaped[offset++] = '"';
                break;
            case '\n':
                escaped[offset++] = '\\';
                escaped[offset++] = 'n';
                break;
            case '\r':
                escaped[offset++] = '\\';
                escaped[offset++] = 'r';
                break;
            case '\t':
                escaped[offset++] = '\\';
                escaped[offset++] = 't';
                break;
            default:
                escaped[offset++] = value[index];
                break;
        }
    }

    escaped[offset] = '\0';
    return escaped;
}

int http_response_set_json(HttpResponse *response, int status_code, const char *json_body) {
    response->status_code = status_code;
    response->body = copy_string(json_body);
    if (response->body == NULL) {
        return 0;
    }
    response->body_length = strlen(response->body);
    return 1;
}

int http_response_set_error(HttpResponse *response, SqlEngineErrorCode code, const char *message) {
    char *escaped_message = http_json_escape(message);
    char body[1024];
    int status_code = http_status_from_engine_error(code);
    int written;

    if (escaped_message == NULL) {
        return 0;
    }

    written = snprintf(body,
                       sizeof(body),
                       "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                       sql_engine_error_code_name(code),
                       escaped_message);
    free(escaped_message);
    if (written < 0 || (size_t)written >= sizeof(body)) {
        return 0;
    }

    return http_response_set_json(response, status_code, body);
}

int http_response_send(sql_socket_t socket_fd, const HttpResponse *response) {
    char header[256];
    int written = snprintf(header,
                           sizeof(header),
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: application/json; charset=utf-8\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           response->status_code,
                           http_reason_phrase(response->status_code),
                           response->body_length);
    if (written < 0 || (size_t)written >= sizeof(header)) {
        return 0;
    }

    if (!send_all(socket_fd, header, (size_t)written)) {
        return 0;
    }

    if (response->body_length > 0 && !send_all(socket_fd, response->body, response->body_length)) {
        return 0;
    }

    return 1;
}
