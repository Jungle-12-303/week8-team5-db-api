#ifndef SQLPARSER_API_QUERY_HANDLER_H
#define SQLPARSER_API_QUERY_HANDLER_H

#include "sqlparser/api/api_context.h"
#include "sqlparser/http/http_request.h"
#include "sqlparser/http/http_response.h"

int api_handle_query(const HttpRequest *request, const ApiContext *context, HttpResponse *response);

#endif
