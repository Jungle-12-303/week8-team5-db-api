#include "sqlparser/api/query_handler.h"
#include "sqlparser/common/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *build_success_body(const SqlEngineAdapterResult *result, const char *escaped_summary, const char *escaped_output) {
    int required = snprintf(NULL,
                            0,
                            "{\"ok\":true,\"statement_type\":\"%s\",\"affected_rows\":%d,"
                            "\"summary\":\"%s\",\"output\":\"%s\",\"elapsed_ms\":%.3f}",
                            result->statement_type,
                            result->affected_rows,
                            escaped_summary,
                            escaped_output,
                            result->elapsed_ms);
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
             "\"summary\":\"%s\",\"output\":\"%s\",\"elapsed_ms\":%.3f}",
             result->statement_type,
             result->affected_rows,
             escaped_summary,
             escaped_output,
             result->elapsed_ms);
    return body;
}

static const char *skip_ws(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    return text;
}

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

int api_handle_query(const HttpRequest *request, const ApiContext *context, HttpResponse *response) {
    const char *content_type = http_request_get_header(request, "Content-Type");
    const char *content_length = http_request_get_header(request, "Content-Length");
    char *sql = NULL;
    SqlEngineAdapterResult result;
    char *escaped_summary = NULL;
    char *escaped_output = NULL;
    char *body = NULL;
    int ok;

    if (content_length == NULL) {
        return http_response_set_error(response,
                                       SQL_ENGINE_ERROR_CONTENT_LENGTH_REQUIRED,
                                       "Content-Length is required for POST /query");
    }

    if (!is_valid_query_content_type(content_type)) {
        return http_response_set_error(response,
                                       SQL_ENGINE_ERROR_INVALID_CONTENT_TYPE,
                                       "Content-Type must be application/json with optional charset=utf-8");
    }

    {
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
    if (!db_service_execute_sql(context->db_service, sql, &result)) {
        free(sql);
        return http_response_set_error(response,
                                       SQL_ENGINE_ERROR_INTERNAL_ERROR,
                                       "failed to execute SQL request");
    }
    free(sql);

    if (!result.ok) {
        int ok = http_response_set_error(response, result.error_code, result.error_message);
        sql_engine_adapter_result_free(&result);
        return ok;
    }

    escaped_summary = http_json_escape(result.summary);
    escaped_output = http_json_escape(result.output != NULL ? result.output : "");
    if (escaped_summary == NULL || escaped_output == NULL) {
        free(escaped_summary);
        free(escaped_output);
        sql_engine_adapter_result_free(&result);
        return 0;
    }

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
