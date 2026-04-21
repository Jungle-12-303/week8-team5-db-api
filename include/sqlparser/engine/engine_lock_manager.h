#ifndef SQLPARSER_ENGINE_LOCK_MANAGER_H
#define SQLPARSER_ENGINE_LOCK_MANAGER_H

#include <stddef.h>
#include <pthread.h>

typedef struct EngineTableLockEntry EngineTableLockEntry;

typedef struct EngineLockManager {
    pthread_mutex_t mutex;
    EngineTableLockEntry *head;
} EngineLockManager;

typedef struct {
    void *entry;
} EngineTableLockHandle;

int engine_lock_manager_init(EngineLockManager *manager, char *error, size_t error_size);
void engine_lock_manager_destroy(EngineLockManager *manager);
int engine_lock_manager_acquire(EngineLockManager *manager,
                                const char *table_key,
                                EngineTableLockHandle *handle,
                                char *error,
                                size_t error_size);
void engine_lock_manager_release(EngineTableLockHandle *handle);

#endif
