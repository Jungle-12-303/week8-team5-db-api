/*
 * api/query_handler.c
 *
 * 이 파일은 POST /query 요청을 받아 실제 SQL 실행까지 연결하는 API 핸들러다.
 *
 * 핵심 역할:
 * - Content-Type, Content-Length 같은 HTTP 수준 조건을 검증한다.
 * - JSON body에서 sql 문자열만 추출한다.
 * - service 계층으로 SQL 실행을 위임한다.
 * - 실행 결과를 API 계약에 맞는 JSON 응답으로 바꾼다.
 *
 * 초심자 관점에서는 "브라우저/클라이언트 요청을 내부 SQL 실행 요청으로 번역하는 입구"다.
 */
#include "sqlparser/api/query_handler.h"
#include "sqlparser/common/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 성공 결과를 API 명세 형태의 JSON body 문자열로 조립한다. */
static char *build_success_body(const SqlEngineAdapterResult *result, const char *escaped_summary, const char *escaped_output) {
    int required = snprintf(NULL,
                            0,
                            "{\"ok\":true,\"statement_type\":\"%s\",\"affected_rows\":%d,"
                            "\"summary\":\"%s\",\"output\":\"%s\",\"elapsed_ms\":%.3f,\"wall_elapsed_ms\":%.3f}",
                            result->statement_type,
                            result->affected_rows,
                            escaped_summary,
                            escaped_output,
                            result->elapsed_ms,
                            result->wall_elapsed_ms);
    char *body;

    if (required < 0) {
        return NULL;
    }

    body = (char *)malloc((size_t)required + 1);
    if (body == NULL) {
        return NULL;
    }

    snprintf(body,
             (size_t)required + 1,
             "{\"ok\":true,\"statement_type\":\"%s\",\"affected_rows\":%d,"
             "\"summary\":\"%s\",\"output\":\"%s\",\"elapsed_ms\":%.3f,\"wall_elapsed_ms\":%.3f}",
             result->statement_type,
             result->affected_rows,
             escaped_summary,
             escaped_output,
             result->elapsed_ms,
             result->wall_elapsed_ms);
    return body;
}

/* 매우 작은 수동 JSON 파서를 위해 공백만 먼저 건너뛴다. */
static const char *skip_ws(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    return text;
}

/*
 * JSON 문자열 토큰 하나를 읽어 C 문자열로 복원한다.
 *
 * 이번 구현은 범위를 단순하게 유지하기 위해
 * sql 필드 추출에 필요한 최소한의 escape만 지원한다.
 */
static int parse_json_string(const char **cursor, char **out_value) {
    const char *input = *cursor;
    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;

    if (*input != '"') {
        return 0;
    }
    input++;

    while (*input != '\0' && *input != '"') {
        char ch = *input++;
        if (ch == '\\') {
            ch = *input++;
            switch (ch) {
                case '"': break;
                case '\\': break;
                case '/': break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                default:
                    free(buffer);
                    return 0;
            }
        }

        if (length + 2 > capacity) {
            size_t new_capacity = capacity == 0 ? 64 : capacity * 2;
            char *new_buffer = (char *)realloc(buffer, new_capacity);
            if (new_buffer == NULL) {
                free(buffer);
                return 0;
            }
            buffer = new_buffer;
            capacity = new_capacity;
        }

        buffer[length++] = ch;
    }

    if (*input != '"') {
        free(buffer);
        return 0;
    }

    if (buffer == NULL) {
        buffer = (char *)malloc(1);
        if (buffer == NULL) {
            return 0;
        }
    }
    buffer[length] = '\0';

    *cursor = input + 1;
    *out_value = buffer;
    return 1;
}

/*
 * sql 외의 다른 JSON 필드는 깊게 해석하지 않고 건너뛴다.
 *
 * 현재는 문자열, null, true, false, 단순 숫자만 넘길 수 있다.
 */
static int skip_json_value(const char **cursor) {
    const char *input = skip_ws(*cursor);
    char *ignored = NULL;

    if (*input == '"') {
        if (!parse_json_string(&input, &ignored)) {
            return 0;
        }
        free(ignored);
        *cursor = input;
        return 1;
    }

    if (strncmp(input, "null", 4) == 0) {
        *cursor = input + 4;
        return 1;
    }
    if (strncmp(input, "true", 4) == 0) {
        *cursor = input + 4;
        return 1;
    }
    if (strncmp(input, "false", 5) == 0) {
        *cursor = input + 5;
        return 1;
    }

    while ((*input >= '0' && *input <= '9') || *input == '-' || *input == '+') {
        input++;
    }
    if (input != *cursor) {
        *cursor = input;
        return 1;
    }

    return 0;
}

/*
 * 요청 JSON에서 "sql" 문자열 필드만 추출한다.
 *
 * 반환값:
 * - 1: 정상 추출
 * - 0: JSON 형식 오류
 * - -1: JSON은 맞지만 sql 필드가 없음
 */
static int extract_sql_field(const char *body, char **sql_out) {
    const char *cursor = skip_ws(body);
    int found_sql = 0;

    if (*cursor != '{') {
        return 0;
    }
    cursor++;

    while (1) {
        char *key = NULL;
        char *value = NULL;

        cursor = skip_ws(cursor);
        if (*cursor == '}') {
            cursor++;
            break;
        }

        if (!parse_json_string(&cursor, &key)) {
            return 0;
        }

        cursor = skip_ws(cursor);
        if (*cursor != ':') {
            free(key);
            return 0;
        }
        cursor++;
        cursor = skip_ws(cursor);

        if (strcmp(key, "sql") == 0) {
            if (!parse_json_string(&cursor, &value)) {
                free(key);
                return 0;
            }
            free(*sql_out);
            *sql_out = value;
            found_sql = 1;
        } else {
            if (!skip_json_value(&cursor)) {
                free(key);
                return 0;
            }
        }

        free(key);
        cursor = skip_ws(cursor);
        if (*cursor == ',') {
            cursor++;
            continue;
        }
        if (*cursor == '}') {
            cursor++;
            break;
        }
        return 0;
    }

    cursor = skip_ws(cursor);
    if (*cursor != '\0') {
        return 0;
    }
    if (!found_sql) {
        return -1;
    }
    return 1;
}

/*
 * POST /query 는 application/json 만 허용한다.
 * charset은 없거나 utf-8 하나만 허용한다.
 */
static int is_valid_query_content_type(const char *content_type) {
    char *copy;
    char *token;
    int seen_charset = 0;
    int valid = 1;

    if (content_type == NULL) {
        return 0;
    }

    copy = copy_string(content_type);
    if (copy == NULL) {
        return 0;
    }

    token = strtok(copy, ";");
    if (token == NULL || !strings_equal_ignore_case(trim_whitespace(token), "application/json")) {
        free(copy);
        return 0;
    }

    token = strtok(NULL, ";");
    while (token != NULL) {
        char *part = trim_whitespace(token);
        char *separator = strchr(part, '=');
        if (separator == NULL) {
            valid = 0;
            break;
        }

        *separator = '\0';
        separator++;
        if (!strings_equal_ignore_case(trim_whitespace(part), "charset") ||
            !strings_equal_ignore_case(trim_whitespace(separator), "utf-8") ||
            seen_charset) {
            valid = 0;
            break;
        }

        seen_charset = 1;
        token = strtok(NULL, ";");
    }

    free(copy);
    return valid;
}

/*
 * 엔진 내부 오류 메시지를 브라우저 사용자에게 조금 더 읽기 쉬운 문장으로 바꾼다.
 *
 * 핵심은 내부 구현 문장을 그대로 노출하기보다,
 * 사용자가 무엇을 고쳐야 하는지 바로 알 수 있게 하는 것이다.
 */
static void build_user_error_message(SqlEngineErrorCode code,
                                     const char *original,
                                     char *buffer,
                                     size_t buffer_size) {
    const char *message = original != NULL ? original : "request failed";

    switch (code) {
        case SQL_ENGINE_ERROR_UNSUPPORTED_SQL:
            if (strstr(message, "only INSERT and SELECT are supported") != NULL) {
                snprintf(buffer,
                         buffer_size,
                         "SQL must start with SELECT or INSERT. Check the first keyword for a typo.");
                return;
            }
            break;
        case SQL_ENGINE_ERROR_SQL_PARSE_ERROR:
            if (strstr(message, "expected identifier") != NULL ||
                strstr(message, "expected keyword") != NULL) {
                snprintf(buffer,
                         buffer_size,
                         "SQL syntax is invalid. Check that required table and column names are present.");
                return;
            }
            break;
        case SQL_ENGINE_ERROR_INVALID_SQL_ARGUMENT:
            if (strstr(message, "WHERE id value must be an integer") != NULL) {
                snprintf(buffer,
                         buffer_size,
                         "WHERE id = ... requires an integer value.");
                return;
            }
            if (strstr(message, "SQL statement must not be blank") != NULL) {
                snprintf(buffer,
                         buffer_size,
                         "SQL statement must not be blank. Enter a SELECT or INSERT statement.");
                return;
            }
            break;
        default:
            break;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

/*
 * POST /query 전체 처리 흐름:
 * 1. 필수 헤더 검증
 * 2. JSON body에서 sql 추출
 * 3. service 계층으로 SQL 실행 위임
 * 4. 성공/실패를 API JSON 응답으로 변환
 */
int api_handle_query(const HttpRequest *request, const ApiContext *context, HttpResponse *response) {
    const char *content_type = http_request_get_header(request, "Content-Type");
    const char *content_length = http_request_get_header(request, "Content-Length");
    char *sql = NULL;
    SqlEngineAdapterResult result;
    char *escaped_summary = NULL;
    char *escaped_output = NULL;
    char *body = NULL;
    char user_message[256];
    int ok;

    /* 1 connection = 1 request 구조라도 POST body 길이는 명시되어 있어야 한다. */
    if (content_length == NULL) {
        return http_response_set_error(response,
                                       SQL_ENGINE_ERROR_CONTENT_LENGTH_REQUIRED,
                                       "Content-Length is required for POST /query");
    }

    /* API 명세에 맞는 JSON content type인지 먼저 확인한다. */
    if (!is_valid_query_content_type(content_type)) {
        return http_response_set_error(response,
                                       SQL_ENGINE_ERROR_INVALID_CONTENT_TYPE,
                                       "Content-Type must be application/json with optional charset=utf-8");
    }

    {
        /* body JSON에서 sql 필드만 뽑아 낸다. */
        int extract_status = extract_sql_field(request->body, &sql);
        if (extract_status == -1) {
            free(sql);
            return http_response_set_error(response,
                                           SQL_ENGINE_ERROR_MISSING_SQL_FIELD,
                                           "field 'sql' is required");
        }
        if (extract_status == 0) {
            free(sql);
            return http_response_set_error(response,
                                           SQL_ENGINE_ERROR_INVALID_JSON,
                                           "request body must be a JSON object containing a string field named 'sql'");
        }
    }

    memset(&result, 0, sizeof(result));
    /* 실제 SQL 실행은 service -> engine adapter 아래 계층으로 위임한다. */
    if (!db_service_execute_sql(context->db_service, sql, &result)) {
        free(sql);
        return http_response_set_error(response,
                                       SQL_ENGINE_ERROR_INTERNAL_ERROR,
                                       "failed to execute SQL request");
    }
    free(sql);

    /* 엔진은 실행까지 했지만 SQL 자체가 실패한 경우다. */
    if (!result.ok) {
        build_user_error_message(result.error_code, result.error_message, user_message, sizeof(user_message));
        int ok = http_response_set_error(response, result.error_code, user_message);
        sql_engine_adapter_result_free(&result);
        return ok;
    }

    /* 성공 결과는 summary/output을 JSON 안전 문자열로 escape해서 응답한다. */
    escaped_summary = http_json_escape(result.summary);
    escaped_output = http_json_escape(result.output != NULL ? result.output : "");
    if (escaped_summary == NULL || escaped_output == NULL) {
        free(escaped_summary);
        free(escaped_output);
        sql_engine_adapter_result_free(&result);
        return 0;
    }

    /* 최종적으로 200 OK JSON 응답 본문을 조립한다. */
    body = build_success_body(&result, escaped_summary, escaped_output);
    free(escaped_summary);
    free(escaped_output);
    if (body == NULL) {
        sql_engine_adapter_result_free(&result);
        return 0;
    }

    ok = http_response_set_json(response, 200, body);
    free(body);
    sql_engine_adapter_result_free(&result);
    return ok;
}
