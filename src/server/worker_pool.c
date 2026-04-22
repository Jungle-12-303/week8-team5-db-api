/*
 * server/worker_pool.c
 *
 * 이 파일은 요청 처리용 worker thread 묶음의 생명주기를 관리하는 모듈이다.
 *
 * worker가 "무슨 일을 하는지"는 server.c에서 넘겨준 routine이 결정하고,
 * 이 파일은 thread 배열을 만들고 기다리고 정리하는 책임만 가진다.
 */
#include "sqlparser/server/worker_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * worker_count 개수만큼 worker thread를 시작한다.
 *
 * 중간에 하나라도 실패하면,
 * 이미 만든 worker들만 join해서 반쯤 열린 상태가 남지 않게 정리한다.
 */
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
            /* 실제로 생성 성공한 개수만 남겨 join 범위를 정확히 맞춘다. */
            pool->count = index;
            server_worker_pool_join(pool);
            return 0;
        }
    }

    return 1;
}

/*
 * 시작된 worker들이 모두 끝날 때까지 기다린다.
 *
 * join은 "스레드 종료 대기"이고,
 * 메모리 해제는 destroy에서 따로 처리한다.
 */
void server_worker_pool_join(ServerWorkerPool *pool) {
    int index;

    for (index = 0; index < pool->count; index++) {
        pthread_join(pool->threads[index], NULL);
    }
}

/* thread handle 배열만 해제한다. 실제 종료 대기는 join 단계에서 이미 끝나 있어야 한다. */
void server_worker_pool_destroy(ServerWorkerPool *pool) {
    free(pool->threads);
    pool->threads = NULL;
    pool->count = 0;
}
