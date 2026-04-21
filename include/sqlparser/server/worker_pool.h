#ifndef SQLPARSER_SERVER_WORKER_POOL_H
#define SQLPARSER_SERVER_WORKER_POOL_H

#include <pthread.h>
#include <stddef.h>

typedef void *(*ServerWorkerRoutine)(void *);

typedef struct {
    pthread_t *threads;
    int count;
} ServerWorkerPool;

int server_worker_pool_start(ServerWorkerPool *pool,
                             int worker_count,
                             ServerWorkerRoutine routine,
                             void *context,
                             char *error,
                             size_t error_size);
void server_worker_pool_join(ServerWorkerPool *pool);
void server_worker_pool_destroy(ServerWorkerPool *pool);

#endif
