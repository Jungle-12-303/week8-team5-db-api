#ifndef SQLPARSER_SERVER_TASK_QUEUE_H
#define SQLPARSER_SERVER_TASK_QUEUE_H

#include "sqlparser/common/platform.h"

#include <pthread.h>
#include <stddef.h>

typedef struct {
    sql_socket_t client_socket;
} ServerTask;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    ServerTask *items;
    int capacity;
    int count;
    int head;
    int tail;
    int closed;
} ServerTaskQueue;

int server_task_queue_init(ServerTaskQueue *queue, int capacity, char *error, size_t error_size);
void server_task_queue_destroy(ServerTaskQueue *queue);
int server_task_queue_try_push(ServerTaskQueue *queue, const ServerTask *task);
int server_task_queue_pop(ServerTaskQueue *queue, ServerTask *task);
void server_task_queue_close(ServerTaskQueue *queue);
int server_task_queue_depth(ServerTaskQueue *queue);

#endif
