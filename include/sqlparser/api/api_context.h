#ifndef SQLPARSER_API_API_CONTEXT_H
#define SQLPARSER_API_API_CONTEXT_H

#include "sqlparser/service/db_service.h"

typedef struct {
    DbService *db_service;
    int worker_count;
    int queue_depth;
} ApiContext;

#endif
