/*
 * api/health_handler.c
 *
 * 이 파일은 GET /health 요청을 처리하는 가장 단순한 API 핸들러다.
 *
 * 핵심 역할:
 * - 서버가 살아 있는지 확인하는 JSON 응답을 만든다.
 * - 현재 worker 수와 queue 깊이를 함께 보여준다.
 * - GET /health 에 body가 오면 잘못된 요청으로 거절한다.
 */
#include "sqlparser/api/health_handler.h"

#include <stdio.h>

/*
 * health 엔드포인트는 SQL 실행과 무관하다.
 * 서버 런타임 상태를 아주 가볍게 확인하는 용도다.
 */
int api_handle_health(const HttpRequest *request, const ApiContext *context, HttpResponse *response) {
    char body[256];
    int written;

    /* 명세상 GET /health는 request body를 허용하지 않는다. */
    if (request->body_length > 0) {
        return http_response_set_error(response,
                                       SQL_ENGINE_ERROR_INVALID_JSON,
                                       "GET /health must not include a request body");
    }

    /* worker 개수와 현재 queue 적재 수를 JSON으로 직렬화한다. */
    written = snprintf(body,
                       sizeof(body),
                       "{\"ok\":true,\"status\":\"ok\",\"worker_count\":%d,\"queue_depth\":%d}",
                       context->worker_count,
                       context->queue_depth);
    if (written < 0 || (size_t)written >= sizeof(body)) {
        return 0;
    }

    /* 성공 시 200 OK + application/json 응답을 보낸다. */
    return http_response_set_json(response, 200, body);
}
