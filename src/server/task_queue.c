/*
 * server/task_queue.c
 *
 * accept thread와 worker thread 사이를 잇는 bounded task queue 구현이다.
 *
 * accept thread는 새 연결을 push하고,
 * worker thread는 queue에서 연결을 pop해 요청을 처리한다.
 * 큐가 닫히면 더 이상 push되지 않고, 대기 중인 worker는 깨어나 종료 경로로 간다.
 */
#include "sqlparser/server/task_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 고정 용량 원형 큐와 동기화 primitive를 초기화한다.
 *
 * 이 큐는 linked list가 아니라 배열 기반 ring buffer다.
 * head는 pop 위치, tail은 push 위치, count는 현재 적재 수를 뜻한다.
 */
int server_task_queue_init(ServerTaskQueue *queue, int capacity, char *error, size_t error_size) {
    memset(queue, 0, sizeof(*queue));
    queue->items = (ServerTask *)calloc((size_t)capacity, sizeof(ServerTask));
    if (queue->items == NULL) {
        snprintf(error, error_size, "out of memory while creating task queue");
        return 0;
    }

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue->items);
        snprintf(error, error_size, "failed to initialize task queue mutex");
        return 0;
    }

    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        snprintf(error, error_size, "failed to initialize task queue condition");
        return 0;
    }

    queue->capacity = capacity;
    return 1;
}

/* 큐가 소유한 메모리와 mutex/condition variable을 정리한다. */
void server_task_queue_destroy(ServerTaskQueue *queue) {
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

/*
 * accept thread가 새 연결 작업을 큐에 넣으려 할 때 사용한다.
 *
 * 중요한 점:
 * - 이 함수는 기다리지 않는다.
 * - 자리가 없으면 즉시 0을 반환한다.
 *
 * 즉 queue full 정책은 "대기"가 아니라 "즉시 거절(503)"이다.
 */
int server_task_queue_try_push(ServerTaskQueue *queue, const ServerTask *task) {
    int pushed = 0;

    pthread_mutex_lock(&queue->mutex);
    if (!queue->closed && queue->count < queue->capacity) {
        /* tail 위치에 새 작업을 넣고, 원형 큐이므로 끝에 도달하면 0으로 감싼다. */
        queue->items[queue->tail] = *task;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->count++;
        /* 대기 중인 worker 하나를 깨워 방금 들어온 작업을 처리하게 한다. */
        pthread_cond_signal(&queue->not_empty);
        pushed = 1;
    }
    pthread_mutex_unlock(&queue->mutex);

    return pushed;
}

/*
 * worker thread가 다음 작업 하나를 가져갈 때 사용한다.
 *
 * 동작 방식:
 * - 큐가 비어 있으면 condition variable에서 잠든다.
 * - 작업이 들어오면 깨어나 하나를 pop한다.
 * - 큐가 닫히고 비어 있으면 0을 반환해 worker 종료 신호가 된다.
 */
int server_task_queue_pop(ServerTaskQueue *queue, ServerTask *task) {
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->closed) {
        /* 작업이 아직 없고 shutdown도 아니면 여기서 block된다. */
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->count == 0 && queue->closed) {
        /* 더 이상 받을 작업도 없고 큐도 닫혔으므로 worker는 종료해도 된다. */
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    /* head 위치의 가장 오래된 작업 하나를 꺼낸다. */
    *task = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

/*
 * shutdown 시 큐를 닫고, 비어 있는 큐에서 잠든 worker들을 모두 깨운다.
 *
 * broadcast를 쓰는 이유는 worker가 여러 개일 수 있기 때문이다.
 */
void server_task_queue_close(ServerTaskQueue *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->closed = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

/* 헬스체크와 테스트에서 현재 큐에 쌓여 있는 작업 수를 안전하게 읽는다. */
int server_task_queue_depth(ServerTaskQueue *queue) {
    int depth;

    pthread_mutex_lock(&queue->mutex);
    depth = queue->count;
    pthread_mutex_unlock(&queue->mutex);
    return depth;
}
