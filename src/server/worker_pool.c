/*
 * server/worker_pool.c
 *
 * 요청 처리용 worker thread 묶음을 생성하고 join/destroy하는 작은 헬퍼다.
 * worker 본체 로직은 server.c에서 넘겨준 routine이 담당하고,
 * 이 파일은 스레드 배열의 생명주기만 관리한다.
 */
#include "sqlparser/server/worker_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* worker_count만큼 스레드를 만들고, 중간 실패 시 이미 만든 스레드는 join한다. */
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

/* 시작된 worker들을 순서대로 join한다. */
void server_worker_pool_join(ServerWorkerPool *pool) {
    int index;

    for (index = 0; index < pool->count; index++) {
        pthread_join(pool->threads[index], NULL);
    }
}

/* thread handle 배열만 해제한다. 실제 종료 대기는 join 단계에서 끝난다. */
void server_worker_pool_destroy(ServerWorkerPool *pool) {
    free(pool->threads);
    pool->threads = NULL;
    pool->count = 0;
}
