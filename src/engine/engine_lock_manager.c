#include "sqlparser/engine/engine_lock_manager.h"

#include "sqlparser/common/util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct EngineTableLockEntry {
    char *table_key;
    pthread_mutex_t mutex;
    EngineTableLockEntry *next;
};

int engine_lock_manager_init(EngineLockManager *manager, char *error, size_t error_size) {
    memset(manager, 0, sizeof(*manager));
    if (pthread_mutex_init(&manager->mutex, NULL) != 0) {
        snprintf(error, error_size, "failed to initialize engine lock manager mutex");
        return 0;
    }

    return 1;
}

void engine_lock_manager_destroy(EngineLockManager *manager) {
    EngineTableLockEntry *entry = manager->head;

    while (entry != NULL) {
        EngineTableLockEntry *next = entry->next;
        pthread_mutex_destroy(&entry->mutex);
        free(entry->table_key);
        free(entry);
        entry = next;
    }

    pthread_mutex_destroy(&manager->mutex);
    manager->head = NULL;
}

static EngineTableLockEntry *find_entry(EngineLockManager *manager, const char *table_key) {
    EngineTableLockEntry *entry = manager->head;

    while (entry != NULL) {
        if (strcmp(entry->table_key, table_key) == 0) {
            return entry;
        }
        entry = entry->next;
    }

    return NULL;
}

int engine_lock_manager_acquire(EngineLockManager *manager,
                                const char *table_key,
                                EngineTableLockHandle *handle,
                                char *error,
                                size_t error_size) {
    EngineTableLockEntry *entry;

    if (pthread_mutex_lock(&manager->mutex) != 0) {
        snprintf(error, error_size, "failed to acquire engine lock manager mutex");
        return 0;
    }

    entry = find_entry(manager, table_key);
    if (entry == NULL) {
        entry = (EngineTableLockEntry *)calloc(1, sizeof(*entry));
        if (entry == NULL) {
            pthread_mutex_unlock(&manager->mutex);
            snprintf(error, error_size, "out of memory while creating table lock");
            return 0;
        }

        entry->table_key = copy_string(table_key);
        if (entry->table_key == NULL) {
            free(entry);
            pthread_mutex_unlock(&manager->mutex);
            snprintf(error, error_size, "out of memory while copying table lock key");
            return 0;
        }

        if (pthread_mutex_init(&entry->mutex, NULL) != 0) {
            free(entry->table_key);
            free(entry);
            pthread_mutex_unlock(&manager->mutex);
            snprintf(error, error_size, "failed to initialize table lock mutex");
            return 0;
        }

        entry->next = manager->head;
        manager->head = entry;
    }

    pthread_mutex_unlock(&manager->mutex);

    if (pthread_mutex_lock(&entry->mutex) != 0) {
        snprintf(error, error_size, "failed to acquire table lock");
        return 0;
    }

    handle->entry = entry;
    return 1;
}

void engine_lock_manager_release(EngineTableLockHandle *handle) {
    EngineTableLockEntry *entry = (EngineTableLockEntry *)handle->entry;
    if (entry != NULL) {
        pthread_mutex_unlock(&entry->mutex);
        handle->entry = NULL;
    }
}
