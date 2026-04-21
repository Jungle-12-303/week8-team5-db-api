#include "sqlparser/api/health_handler.h"

#include <stdio.h>

int api_handle_health(const HttpRequest *request, const ApiContext *context, HttpResponse *response) {
    char body[256];
    int written;

    if (request->body_length > 0) {
        return http_response_set_error(response,
                                       SQL_ENGINE_ERROR_INVALID_JSON,
                                       "GET /health must not include a request body");
    }

    written = snprintf(body,
                       sizeof(body),
                       "{\"ok\":true,\"status\":\"ok\",\"worker_count\":%d,\"queue_depth\":%d}",
                       context->worker_count,
                       context->queue_depth);
    if (written < 0 || (size_t)written >= sizeof(body)) {
        return 0;
    }

    return http_response_set_json(response, 200, body);
}
