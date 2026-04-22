/*
 * engine/engine_lock_manager.c
 *
 * 이 파일은 "테이블 단위 직렬화"를 실제 pthread mutex로 구현하는 모듈이다.
 *
 * 역할:
 * - table_key마다 전용 mutex를 하나씩 관리한다.
 * - 같은 table_key 요청은 같은 mutex를 잡게 해 직렬화한다.
 * - 서로 다른 table_key 요청은 서로 다른 mutex를 사용하므로 병렬 실행 가능하다.
 *
 * 중요한 점:
 * - manager->mutex는 "락 엔트리 목록" 자체를 보호하는 전역 mutex다.
 * - entry->mutex는 "특정 테이블 하나"에 대한 실제 실행 직렬화 mutex다.
 *
 * 즉 전역 mutex와 테이블별 mutex는 역할이 다르다.
 */
#include "sqlparser/engine/engine_lock_manager.h"
#include "sqlparser/common/util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct EngineTableLockEntry {
    /* schema.storage_name 기준의 canonical table key다. */
    char *table_key;
    /* 같은 테이블 요청을 한 번에 하나만 통과시키는 실제 배타 mutex다. */
    pthread_mutex_t mutex;
    /* 단순 연결 리스트로 다음 엔트리를 가리킨다. */
    EngineTableLockEntry *next;
};

/* lock manager 구조체를 초기 상태로 만들고 전역 엔트리 보호용 mutex를 준비한다. */
int engine_lock_manager_init(EngineLockManager *manager, char *error, size_t error_size) {
    memset(manager, 0, sizeof(*manager));
    if (pthread_mutex_init(&manager->mutex, NULL) != 0) {
        snprintf(error, error_size, "failed to initialize engine lock manager mutex");
        return 0;
    }

    return 1;
}

/* 등록된 모든 테이블 락 엔트리를 순회하며 개별 mutex와 메모리를 함께 정리한다. */
void engine_lock_manager_destroy(EngineLockManager *manager) {
    EngineTableLockEntry *entry = manager->head;

    while (entry != NULL) {
        /* 다음 포인터를 먼저 빼 두어 현재 엔트리를 안전하게 해제한다. */
        EngineTableLockEntry *next = entry->next;
        pthread_mutex_destroy(&entry->mutex);
        free(entry->table_key);
        free(entry);
        entry = next;
    }

    pthread_mutex_destroy(&manager->mutex);
    manager->head = NULL;
}

/* 현재 레지스트리에서 같은 table_key를 가진 엔트리가 이미 있는지 선형 탐색한다. */
static EngineTableLockEntry *find_entry(EngineLockManager *manager, const char *table_key) {
    EngineTableLockEntry *entry = manager->head;

    while (entry != NULL) {
        /* canonical key가 같으면 같은 물리 테이블을 뜻하므로 기존 락을 재사용한다. */
        if (strcmp(entry->table_key, table_key) == 0) {
            return entry;
        }
        entry = entry->next;
    }

    return NULL;
}

/*
 * 요청이 사용할 테이블 락을 획득한다.
 *
 * 순서:
 * 1. 전역 mutex로 엔트리 목록 접근을 보호한다.
 * 2. table_key에 해당하는 엔트리를 찾거나 새로 만든다.
 * 3. 전역 mutex를 놓는다.
 * 4. 해당 테이블 전용 mutex를 잡아 실제 직렬화를 시작한다.
 */
int engine_lock_manager_acquire(EngineLockManager *manager,
                                const char *table_key,
                                EngineTableLockHandle *handle,
                                char *error,
                                size_t error_size) {
    EngineTableLockEntry *entry;

    /* 레지스트리 목록(head, next 포인터 조작)을 안전하게 다루기 위한 전역 mutex다. */
    if (pthread_mutex_lock(&manager->mutex) != 0) {
        snprintf(error, error_size, "failed to acquire engine lock manager mutex");
        return 0;
    }

    /* 이미 존재하는 테이블 엔트리가 있으면 그 락을 재사용한다. */
    entry = find_entry(manager, table_key);
    if (entry == NULL) {
        /* 처음 보는 테이블이면 새 엔트리를 만들고 전용 mutex를 준비한다. */
        entry = (EngineTableLockEntry *)calloc(1, sizeof(*entry));
        if (entry == NULL) {
            /* 실패 시 전역 mutex를 반드시 풀고 오류를 반환한다. */
            pthread_mutex_unlock(&manager->mutex);
            snprintf(error, error_size, "out of memory while creating table lock");
            return 0;
        }

        /* table_key는 호출자가 소유한 메모리일 수 있으므로 엔트리 내부에 복사해 둔다. */
        entry->table_key = copy_string(table_key);
        if (entry->table_key == NULL) {
            free(entry);
            pthread_mutex_unlock(&manager->mutex);
            snprintf(error, error_size, "out of memory while copying table lock key");
            return 0;
        }

        /* 새 테이블에 대한 실제 직렬화 mutex를 초기화한다. */
        if (pthread_mutex_init(&entry->mutex, NULL) != 0) {
            free(entry->table_key);
            free(entry);
            pthread_mutex_unlock(&manager->mutex);
            snprintf(error, error_size, "failed to initialize table lock mutex");
            return 0;
        }

        /* 연결 리스트 머리에 새 엔트리를 붙여 이후 요청이 재사용할 수 있게 한다. */
        entry->next = manager->head;
        manager->head = entry;
    }

    /*
     * 여기서 전역 mutex는 놓는다.
     * 이후부터는 "엔트리 목록 보호"가 아니라 "특정 테이블 직렬화"가 목적이므로
     * 테이블 전용 mutex만 잡으면 된다.
     */
    pthread_mutex_unlock(&manager->mutex);

    /*
     * 실제로 같은 테이블 요청을 기다리게 만드는 지점이다.
     * 이미 다른 스레드가 이 mutex를 잡고 있으면 여기서 block된다.
     */
    if (pthread_mutex_lock(&entry->mutex) != 0) {
        snprintf(error, error_size, "failed to acquire table lock");
        return 0;
    }

    /* release 시 다시 같은 엔트리를 찾아갈 수 있도록 handle에 저장한다. */
    handle->entry = entry;
    return 1;
}

/* acquire에서 잡아 둔 테이블 전용 mutex를 해제한다. */
void engine_lock_manager_release(EngineTableLockHandle *handle) {
    EngineTableLockEntry *entry = (EngineTableLockEntry *)handle->entry;
    if (entry != NULL) {
        /* handle이 비어 있지 않을 때만 unlock하여 중복 해제를 피한다. */
        pthread_mutex_unlock(&entry->mutex);
        handle->entry = NULL;
    }
}
