#include "sqlparser/server/task_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void server_task_queue_destroy(ServerTaskQueue *queue) {
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

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

int server_task_queue_pop(ServerTaskQueue *queue, ServerTask *task) {
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->closed) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->count == 0 && queue->closed) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    *task = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

void server_task_queue_close(ServerTaskQueue *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->closed = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

int server_task_queue_depth(ServerTaskQueue *queue) {
    int depth;

    pthread_mutex_lock(&queue->mutex);
    depth = queue->count;
    pthread_mutex_unlock(&queue->mutex);
    return depth;
}
