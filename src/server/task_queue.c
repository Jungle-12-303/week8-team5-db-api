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

/* 고정 용량 원형 큐와 동기화 primitive를 초기화한다. */
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

/* 큐가 소유한 메모리와 mutex/condition을 정리한다. */
void server_task_queue_destroy(ServerTaskQueue *queue) {
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

/* 큐가 닫히지 않았고 자리가 남아 있을 때만 즉시 push한다. */
int server_task_queue_try_push(ServerTaskQueue *queue, const ServerTask *task) {
    int pushed = 0;

    pthread_mutex_lock(&queue->mutex);
    if (!queue->closed && queue->count < queue->capacity) {
        queue->items[queue->tail] = *task;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->count++;
        pthread_cond_signal(&queue->not_empty);
        pushed = 1;
    }
    pthread_mutex_unlock(&queue->mutex);

    return pushed;
}

/* worker는 큐가 비어 있으면 대기하고, close되면 0을 반환하며 종료한다. */
int server_task_queue_pop(ServerTaskQueue *queue, ServerTask *task) {
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->closed) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->count == 0 65&& queue->closed) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    *task = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

/* shutdown 시 대기 중인 worker들을 모두 깨워 종료 경로로 보낸다. */
void server_task_queue_close(ServerTaskQueue *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->closed = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

/* 헬스체크와 테스트에서 현재 대기 중인 작업 수를 읽는다. */
int server_task_queue_depth(ServerTaskQueue *queue) {
    int depth;

    pthread_mutex_lock(&queue->mutex);
    depth = queue->count;
    pthread_mutex_unlock(&queue->mutex);
    return depth;
}
