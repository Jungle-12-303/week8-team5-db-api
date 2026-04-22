/*
 * http/router.c
 *
 * 이 파일은 이미 파싱된 HttpRequest를 어느 API 핸들러로 보낼지 결정하는 라우터다.
 *
 * 핵심 역할:
 * - path를 기준으로 /, /health, /query 중 어디로 보낼지 고른다.
 * - path는 맞지만 method가 틀리면 405를 돌려준다.
 * - path 자체가 없으면 404를 돌려준다.
 */
#include "sqlparser/http/router.h"

#include "sqlparser/api/health_handler.h"
#include "sqlparser/api/query_handler.h"
#include "sqlparser/api/root_handler.h"

#include <string.h>

/*
 * 라우터는 SQL 실행을 직접 하지 않는다.
 * "어느 핸들러가 이 요청을 처리해야 하는가"만 판단한다.
 */
int http_route_request(const HttpRequest *request, const ApiContext *context, HttpResponse *response) {
    /* 브라우저 진입 페이지는 GET / 만 허용한다. */
    if (strcmp(request->path, "/") == 0) {
        if (strcmp(request->method, "GET") != 0) {
            return http_response_set_error(response,
                                           SQL_ENGINE_ERROR_METHOD_NOT_ALLOWED,
                                           "only GET is allowed for /");
        }
        return api_handle_root(request, context, response);
    }

    /* 상태 확인 엔드포인트도 GET 전용이다. */
    if (strcmp(request->path, "/health") == 0) {
        if (strcmp(request->method, "GET") != 0) {
            return http_response_set_error(response,
                                           SQL_ENGINE_ERROR_METHOD_NOT_ALLOWED,
                                           "only GET is allowed for /health");
        }
        return api_handle_health(request, context, response);
    }

    /* SQL 실행은 POST /query 로만 받는다. */
    if (strcmp(request->path, "/query") == 0) {
        if (strcmp(request->method, "POST") != 0) {
            return http_response_set_error(response,
                                           SQL_ENGINE_ERROR_METHOD_NOT_ALLOWED,
                                           "only POST is allowed for /query");
        }
        return api_handle_query(request, context, response);
    }

    /* 어떤 등록 경로와도 맞지 않으면 404다. */
    return http_response_set_error(response, SQL_ENGINE_ERROR_NOT_FOUND, "requested path does not exist");
}
