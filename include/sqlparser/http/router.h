#ifndef SQLPARSER_HTTP_ROUTER_H
#define SQLPARSER_HTTP_ROUTER_H

#include "sqlparser/api/api_context.h"
#include "sqlparser/http/http_request.h"
#include "sqlparser/http/http_response.h"

int http_route_request(const HttpRequest *request, const ApiContext *context, HttpResponse *response);

#endif
