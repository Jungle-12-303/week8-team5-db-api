/*
 * http/http_request.c
 *
 * 이 파일은 소켓에서 들어온 바이트 스트림을 HTTP 요청 구조체로 바꾸는 모듈이다.
 *
 * 핵심 역할:
 * - request line을 읽어 method / path / version을 분리한다.
 * - header를 한 줄씩 파싱해 name / value 목록으로 저장한다.
 * - Content-Length, Transfer-Encoding 같은 핵심 헤더를 검증한다.
 * - 필요하면 request body를 정확한 길이만큼 추가로 읽는다.
 *
 * 초심자 관점에서는 "네트워크로 온 원시 문자열을 서버가 이해할 수 있는 HttpRequest로 정리하는 단계"다.
 */
#include "sqlparser/http/http_request.h"

#include "sqlparser/common/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 요청 파싱 실패 결과를 공통 형식으로 채우는 작은 헬퍼다. */
static void set_read_error(HttpRequestReadResult *result, SqlEngineErrorCode code, const char *message) {
    result->ok = 0;
    result->error_code = code;
    snprintf(result->message, sizeof(result->message), "%s", message);
}

/*
 * Content-Length 만큼 body를 끝까지 읽기 위한 루프다.
 *
 * recv()는 한 번에 필요한 만큼 모두 주지 않을 수 있으므로,
 * length 바이트를 다 받을 때까지 반복 호출한다.
 */
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

/* 요청 구조체를 0으로 초기화해 안전한 시작 상태를 만든다. */
void http_request_init(HttpRequest *request) {
    memset(request, 0, sizeof(*request));
}

/* 요청 파싱 중 확보한 header/body 메모리를 모두 반납한다. */
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

/* 헤더 이름은 대소문자를 구분하지 않으므로 선형 탐색으로 찾는다. */
const char *http_request_get_header(const HttpRequest *request, const char *name) {
    int index;

    for (index = 0; index < request->header_count; index++) {
        if (strings_equal_ignore_case(request->headers[index].name, name)) {
            return request->headers[index].value;
        }
    }

    return NULL;
}

/* 파싱한 헤더 한 줄을 동적 배열 뒤에 추가한다. */
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

/*
 * 첫 줄인 "METHOD PATH VERSION" 형식을 검사한다.
 *
 * 이번 과제 범위에서는 HTTP/1.1만 받으므로
 * 다른 버전이면 바로 실패시킨다.
 */
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

/*
 * 소켓에서 HTTP 요청 1개를 읽어 HttpRequest 구조체로 변환한다.
 *
 * 큰 흐름:
 * 1. \r\n\r\n 이 나올 때까지 header 구간을 읽는다.
 * 2. request line을 파싱한다.
 * 3. header를 한 줄씩 저장한다.
 * 4. Transfer-Encoding / Content-Length를 검증한다.
 * 5. body가 있으면 지정된 길이만큼 정확히 읽는다.
 */
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

    /* header 전체를 임시로 담을 버퍼를 준비한다. */
    header_buffer = (char *)malloc(header_limit + 1);
    if (header_buffer == NULL) {
        set_read_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, "out of memory while reading request");
        return 0;
    }

    /*
     * request header는 길이를 미리 모르므로 1바이트씩 읽는다.
     * \r\n\r\n 이 보이면 header가 끝난 것이다.
     */
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

    /* 끝 구분자를 못 찾은 채 제한에 도달하면 header가 너무 큰 요청으로 본다. */
    if (!complete) {
        free(header_buffer);
        set_read_error(result, SQL_ENGINE_ERROR_HEADER_TOO_LARGE, "HTTP headers exceed configured size limit");
        return 0;
    }

    /* 첫 줄(request line)을 잘라 method / path / version으로 해석한다. */
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

        /* 빈 줄을 만나면 header 구간이 끝난 것이다. */
        if (line_end == cursor) {
            break;
        }

        *line_end = '\0';
        /*
         * 줄 첫 글자가 공백이면 folded header인데,
         * 이번 최소 구현에서는 지원하지 않는다.
         */
        if (*cursor == ' ' || *cursor == '\t') {
            free(header_buffer);
            set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "folded HTTP headers are not supported");
            return 0;
        }

        /* "Name: Value" 형식이 아니면 잘못된 헤더다. */
        separator = strchr(cursor, ':');
        if (separator == NULL) {
            free(header_buffer);
            set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "malformed HTTP header line");
            return 0;
        }

        /* 양끝 공백을 정리한 뒤 헤더 배열에 저장한다. */
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

    /*
     * chunked 전송은 이번 과제 범위 밖이므로
     * Transfer-Encoding 헤더에 chunked가 있으면 거부한다.
     */
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

    /* Content-Length가 있으면 음수가 아닌 정수인지 확인한다. */
    content_length_header = http_request_get_header(request, "Content-Length");
    if (content_length_header != NULL) {
        if (!parse_int_strict(content_length_header, &content_length) || content_length < 0) {
            set_read_error(result, SQL_ENGINE_ERROR_INVALID_CONTENT_LENGTH, "Content-Length must be a non-negative integer");
            return 0;
        }
    }

    if (content_length > 0) {
        /* body도 서버 설정 제한을 넘으면 앞단에서 바로 차단한다. */
        if ((size_t)content_length > body_limit) {
            set_read_error(result, SQL_ENGINE_ERROR_PAYLOAD_TOO_LARGE, "request body exceeds configured size limit");
            return 0;
        }

        /* Content-Length가 가리키는 정확한 바이트 수만큼 body를 읽는다. */
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
        /* body가 없더라도 이후 코드가 NULL 검사 없이 다룰 수 있게 빈 문자열을 넣는다. */
        request->body = copy_string("");
        if (request->body == NULL) {
            set_read_error(result, SQL_ENGINE_ERROR_INTERNAL_ERROR, "out of memory while creating empty request body");
            return 0;
        }
    }

    /* 여기까지 왔으면 request line, header, body가 모두 일관되게 읽힌 상태다. */
    result->ok = 1;
    return 1;
}
