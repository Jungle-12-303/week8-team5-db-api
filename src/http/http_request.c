#include "sqlparser/http/http_request.h"

#include "sqlparser/common/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_read_error(HttpRequestReadResult *result, SqlEngineErrorCode code, const char *message) {
    result->ok = 0;
    result->error_code = code;
    snprintf(result->message, sizeof(result->message), "%s", message);
}

static int recv_exact(sql_socket_t socket_fd, char *buffer, size_t length) {
    size_t offset = 0;

    while (offset < length) {
        int received = recv(socket_fd, buffer + offset, (int)(length - offset), 0);
        if (received <= 0) {
            return 0;
        }
        offset += (size_t)received;
    }

    return 1;
}

void http_request_init(HttpRequest *request) {
    memset(request, 0, sizeof(*request));
}

void http_request_free(HttpRequest *request) {
    int index;

    for (index = 0; index < request->header_count; index++) {
        free(request->headers[index].name);
        free(request->headers[index].value);
    }

    free(request->headers);
    free(request->body);
    memset(request, 0, sizeof(*request));
}

const char *http_request_get_header(const HttpRequest *request, const char *name) {
    int index;

    for (index = 0; index < request->header_count; index++) {
        if (strings_equal_ignore_case(request->headers[index].name, name)) {
            return request->headers[index].value;
        }
    }

    return NULL;
}

static int push_header(HttpRequest *request, const char *name, const char *value) {
    HttpHeader *new_headers = (HttpHeader *)realloc(request->headers,
                                                    (size_t)(request->header_count + 1) * sizeof(HttpHeader));
    if (new_headers == NULL) {
        return 0;
    }

    request->headers = new_headers;
    request->headers[request->header_count].name = copy_string(name);
    request->headers[request->header_count].value = copy_string(value);
    if (request->headers[request->header_count].name == NULL ||
        request->headers[request->header_count].value == NULL) {
        free(request->headers[request->header_count].name);
        free(request->headers[request->header_count].value);
        return 0;
    }

    request->header_count++;
    return 1;
}

static int parse_request_line(HttpRequest *request, char *line, HttpRequestReadResult *result) {
    char extra[2];
    if (sscanf(line, "%15s %127s %15s %1s",
               request->method,
               request->path,
               request->version,
               extra) != 3) {
        set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "invalid HTTP request line");
        return 0;
    }

    if (strcmp(request->version, "HTTP/1.1") != 0) {
        set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "only HTTP/1.1 is supported");
        return 0;
    }

    return 1;
}

int http_read_request(sql_socket_t socket_fd,
                      size_t header_limit,
                      size_t body_limit,
                      HttpRequest *request,
                      HttpRequestReadResult *result) {
    char *header_buffer = NULL;
    size_t total = 0;
    int complete = 0;
    char byte;
    char *cursor;
    char *line;
    const char *content_length_header;
    const char *transfer_encoding_header;
    int content_length = -1;

    http_request_init(request);
    memset(result, 0, sizeof(*result));

    header_buffer = (char *)malloc(header_limit + 1);
    if (header_buffer == NULL) {
        set_read_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, "out of memory while reading request");
        return 0;
    }

    while (total < header_limit) {
        int received = recv(socket_fd, &byte, 1, 0);
        if (received <= 0) {
            free(header_buffer);
            set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "truncated HTTP request");
            return 0;
        }

        header_buffer[total++] = byte;
        if (total >= 4 &&
            header_buffer[total - 4] == '\r' &&
            header_buffer[total - 3] == '\n' &&
            header_buffer[total - 2] == '\r' &&
            header_buffer[total - 1] == '\n') {
            complete = 1;
            break;
        }
    }

    if (!complete) {
        free(header_buffer);
        set_read_error(result, SQL_ENGINE_ERROR_HEADER_TOO_LARGE, "HTTP headers exceed configured size limit");
        return 0;
    }

    header_buffer[total] = '\0';
    cursor = header_buffer;
    line = strstr(cursor, "\r\n");
    if (line == NULL) {
        free(header_buffer);
        set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "malformed HTTP request");
        return 0;
    }

    *line = '\0';
    if (!parse_request_line(request, cursor, result)) {
        free(header_buffer);
        return 0;
    }

    cursor = line + 2;
    while (*cursor != '\0') {
        char *line_end = strstr(cursor, "\r\n");
        char *separator;
        char *name;
        char *value;

        if (line_end == NULL) {
            free(header_buffer);
            set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "malformed HTTP header line");
            return 0;
        }

        if (line_end == cursor) {
            break;
        }

        *line_end = '\0';
        if (*cursor == ' ' || *cursor == '\t') {
            free(header_buffer);
            set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "folded HTTP headers are not supported");
            return 0;
        }

        separator = strchr(cursor, ':');
        if (separator == NULL) {
            free(header_buffer);
            set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "malformed HTTP header line");
            return 0;
        }

        *separator = '\0';
        name = trim_whitespace(cursor);
        value = trim_whitespace(separator + 1);
        if (!push_header(request, name, value)) {
            free(header_buffer);
            set_read_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, "out of memory while storing HTTP headers");
            return 0;
        }

        cursor = line_end + 2;
    }

    free(header_buffer);

    transfer_encoding_header = http_request_get_header(request, "Transfer-Encoding");
    if (transfer_encoding_header != NULL) {
        char *copy = copy_string(transfer_encoding_header);
        char *token;
        if (copy == NULL) {
            set_read_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, "out of memory while parsing Transfer-Encoding");
            return 0;
        }

        token = strtok(copy, ",");
        while (token != NULL) {
            token = trim_whitespace(token);
            if (strings_equal_ignore_case(token, "chunked")) {
                free(copy);
                set_read_error(result, SQL_ENGINE_ERROR_CHUNKED_NOT_SUPPORTED,
                               "Transfer-Encoding: chunked is not supported");
                return 0;
            }
            token = strtok(NULL, ",");
        }
        free(copy);
    }

    content_length_header = http_request_get_header(request, "Content-Length");
    if (content_length_header != NULL) {
        if (!parse_int_strict(content_length_header, &content_length) || content_length < 0) {
            set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "Content-Length must be a non-negative integer");
            return 0;
        }
    }

    if (content_length > 0) {
        if ((size_t)content_length > body_limit) {
            set_read_error(result, SQL_ENGINE_ERROR_PAYLOAD_TOO_LARGE, "request body exceeds configured size limit");
            return 0;
        }

        request->body = (char *)malloc((size_t)content_length + 1);
        if (request->body == NULL) {
            set_read_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, "out of memory while reading request body");
            return 0;
        }

        if (!recv_exact(socket_fd, request->body, (size_t)content_length)) {
            set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "request body is shorter than Content-Length");
            return 0;
        }

        request->body[content_length] = '\0';
        request->body_length = (size_t)content_length;
    } else {
        request->body = copy_string("");
        if (request->body == NULL) {
            set_read_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, "out of memory while creating empty request body");
            return 0;
        }
    }

    result->ok = 1;
    return 1;
}
