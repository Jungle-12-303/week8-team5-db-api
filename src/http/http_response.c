/*
 * http/http_response.c
 *
 * 이 파일은 서버 내부 결과를 실제 HTTP 응답 문자열로 바꾸고 소켓으로 보내는 모듈이다.
 *
 * 핵심 역할:
 * - 내부 오류 코드를 HTTP 상태 코드로 바꾼다.
 * - JSON 문자열에 필요한 escape를 적용한다.
 * - status line, header, body를 합쳐 HTTP/1.1 응답을 만든다.
 * - 조기 종료된 클라이언트에게 보내다가 프로세스가 죽지 않도록 안전하게 전송한다.
 */
#include "sqlparser/http/http_response.h"

#include "sqlparser/common/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 헤더와 body를 부분 전송 없이 끝까지 쓰기 위한 루프다.
 *
 * send 계열 함수도 한 번에 length 전체를 보내준다는 보장이 없으므로,
 * 남은 바이트가 없어질 때까지 반복 전송한다.
 */
static int send_all(sql_socket_t socket_fd, const char *buffer, size_t length) {
    size_t offset = 0;

    while (offset < length) {
        /* 플랫폼 래퍼를 사용해 SIGPIPE 같은 플랫폼별 문제를 흡수한다. */
        int sent = sql_platform_send_socket(socket_fd, buffer + offset, length - offset);
        if (sent <= 0) {
            return 0;
        }

        offset += (size_t)sent;
    }

    return 1;
}

/* 응답 구조체를 초기 상태로 만든다. */
void http_response_init(HttpResponse *response) {
    memset(response, 0, sizeof(*response));
}

/* body 버퍼를 해제하고 재사용 가능한 상태로 되돌린다. */
void http_response_free(HttpResponse *response) {
    free(response->content_type);
    free(response->body);
    memset(response, 0, sizeof(*response));
}

/* 상태 코드 숫자를 사람이 읽는 reason phrase로 바꾼다. */
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

/*
 * 내부 엔진/HTTP 오류 코드를 외부 HTTP 상태 코드로 매핑한다.
 *
 * 예를 들어 잘못된 JSON과 잘못된 SQL 인자는 400,
 * queue full은 503, 내부 실행 실패는 500으로 묶는다.
 */
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

/*
 * 일반 문자열을 JSON 문자열 값으로 안전하게 넣기 위해 escape한다.
 *
 * 따옴표, 역슬래시, 개행 문자 등을 그대로 넣으면 JSON 문법이 깨질 수 있다.
 */
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

/* content type, body, 길이를 응답 구조체에 복사 저장한다. */
int http_response_set_body(HttpResponse *response, int status_code, const char *content_type, const char *body) {
    response->status_code = status_code;
    response->content_type = copy_string(content_type);
    response->body = copy_string(body);
    if (response->content_type == NULL || response->body == NULL) {
        free(response->content_type);
        free(response->body);
        response->content_type = NULL;
        response->body = NULL;
        response->body_length = 0;
        return 0;
    }
    response->body_length = strlen(response->body);
    return 1;
}

/* JSON 응답은 공통 content type을 사용하므로 얇은 래퍼로 분리했다. */
int http_response_set_json(HttpResponse *response, int status_code, const char *json_body) {
    return http_response_set_body(response,
                                  status_code,
                                  "application/json; charset=utf-8",
                                  json_body);
}

/* HTML 응답도 같은 구조를 재사용하되 content type만 다르게 준다. */
int http_response_set_html(HttpResponse *response, int status_code, const char *html_body) {
    return http_response_set_body(response,
                                  status_code,
                                  "text/html; charset=utf-8",
                                  html_body);
}

/*
 * 내부 오류를 API 공통 형식의 JSON 오류 응답으로 감싼다.
 *
 * 결과 예:
 * {"ok":false,"error":{"code":"INVALID_JSON","message":"..."}}
 */
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

/*
 * HttpResponse 구조체를 실제 HTTP/1.1 바이트열로 만들어 전송한다.
 *
 * 이번 서버는 keep-alive를 지원하지 않으므로
 * 항상 Connection: close 헤더를 붙인다.
 */
int http_response_send(sql_socket_t socket_fd, const HttpResponse *response) {
    char header[256];
    int written = snprintf(header,
                           sizeof(header),
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           response->status_code,
                           http_reason_phrase(response->status_code),
                           response->content_type != NULL ? response->content_type : "application/json; charset=utf-8",
                           response->body_length);
    if (written < 0 || (size_t)written >= sizeof(header)) {
        return 0;
    }

    /* 먼저 상태줄과 헤더를 보내고, */
    if (!send_all(socket_fd, header, (size_t)written)) {
        return 0;
    }

    /* body가 있으면 이어서 전송한다. */
    if (response->body_length > 0 && !send_all(socket_fd, response->body, response->body_length)) {
        return 0;
    }

    return 1;
}
