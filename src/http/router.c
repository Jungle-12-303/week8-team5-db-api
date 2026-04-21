#include "sqlparser/http/router.h"

#include "sqlparser/api/health_handler.h"
#include "sqlparser/api/query_handler.h"

#include <string.h>

int http_route_request(const HttpRequest *request, const ApiContext *context, HttpResponse *response) {
    if (strcmp(request->path, "/health") == 0) {
        if (strcmp(request->method, "GET") != 0) {
            return http_response_set_error(response,
                                           SQL_ENGINE_ERROR_METHOD_NOT_ALLOWED,
                                           "only GET is allowed for /health");
        }
        return api_handle_health(request, context, response);
    }

    if (strcmp(request->path, "/query") == 0) {
        if (strcmp(request->method, "POST") != 0) {
            return http_response_set_error(response,
                                           SQL_ENGINE_ERROR_METHOD_NOT_ALLOWED,
                                           "only POST is allowed for /query");
        }
        return api_handle_query(request, context, response);
    }

    return http_response_set_error(response, SQL_ENGINE_ERROR_NOT_FOUND, "requested path does not exist");
}
