#include "sqlparser/server/worker_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int server_worker_pool_start(ServerWorkerPool *pool,
                             int worker_count,
                             ServerWorkerRoutine routine,
                             void *context,
                             char *error,
                             size_t error_size) {
    int index;

    memset(pool, 0, sizeof(*pool));
    pool->threads = (pthread_t *)calloc((size_t)worker_count, sizeof(pthread_t));
    if (pool->threads == NULL) {
        snprintf(error, error_size, "out of memory while creating worker pool");
        return 0;
    }

    pool->count = worker_count;
    for (index = 0; index < worker_count; index++) {
        if (pthread_create(&pool->threads[index], NULL, routine, context) != 0) {
            snprintf(error, error_size, "failed to create worker thread");
            pool->count = index;
            server_worker_pool_join(pool);
            return 0;
        }
    }

    return 1;
}

void server_worker_pool_join(ServerWorkerPool *pool) {
    int index;

    for (index = 0; index < pool->count; index++) {
        pthread_join(pool->threads[index], NULL);
    }
}

void server_worker_pool_destroy(ServerWorkerPool *pool) {
    free(pool->threads);
    pool->threads = NULL;
    pool->count = 0;
}
