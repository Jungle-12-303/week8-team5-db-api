#include "sqlparser/engine/engine_lock_manager.h"

#include "sqlparser/common/util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



// 테이블 락 하나를 저장하는 구조
struct EngineTableLockEntry {
    char *table_key; // 테이블 이름
    pthread_mutex_t mutex; // 테이블 전용 락

    //링크드리스트로 다음 테이블락을 들고있음
    EngineTableLockEntry *next; // 다음 락 칸 연결
};

// 락 매니저를 처음 시작할 때 관리자용 락 생성
int engine_lock_manager_init(EngineLockManager *manager, char *error, size_t error_size) {
    memset(manager, 0, sizeof(*manager)); // manager 전체를 0으로 초기화

    //manager->mutex는 테이블 락 목록 자체를 안전하게 관리하는 락
    if (pthread_mutex_init(&manager->mutex, NULL) != 0) {
        snprintf(error, error_size, "failed to initialize engine lock manager mutex");
        return 0;
    }

    return 1;
}

//종료할 때 전부 치우는 함수
void engine_lock_manager_destroy(EngineLockManager *manager) {
    EngineTableLockEntry *entry = manager->head; // 리스트 첫 칸부터 시작

    // 각 테이블 락 엔트리에 대해
    while (entry != NULL) {
        EngineTableLockEntry *next = entry->next;
        pthread_mutex_destroy(&entry->mutex); // 테이블 뮤텍스 제거
        free(entry->table_key); // 테이블 이름 메모리 해제
        free(entry); // 엔트리 메모리 해제
        entry = next;
    }

    pthread_mutex_destroy(&manager->mutex); // manager->mutex 제거
    manager->head = NULL;
}

// 테이블 이름으로 기존 락이 있는지 찾는 함수
// 테이블 키로 "student" 들어오면 리스트 쭉 돌면서 "student"인 엔트리 찾는다
static EngineTableLockEntry *find_entry(EngineLockManager *manager, const char *table_key) {
    EngineTableLockEntry *entry = manager->head;

    while (entry != NULL) {
        if (strcmp(entry->table_key, table_key) == 0) { // 찾으면 돌려주고
            return entry;
        }
        entry = entry->next;
    }

    return NULL; // 없으면 NULL 반환
}


//이게 제일 중요하다
//해당 테이블 락을 잡아라! 하는 함수
// engine_lock_manager_acquire(manager, "student", ...) 하면 student용 락을 찾아서 실제로 잠근다
int engine_lock_manager_acquire(EngineLockManager *manager,
                                const char *table_key,
                                EngineTableLockHandle *handle,
                                char *error,
                                size_t error_size) {
    EngineTableLockEntry *entry;

    //먼저 manager->mutex를 잡는다
    //왜? 여러 스레드가 동시에 락 목록을 건드리면 안 되니까
    if (pthread_mutex_lock(&manager->mutex) != 0) {
        snprintf(error, error_size, "failed to acquire engine lock manager mutex");
        return 0;
    }

    //이미 그 테이블에 락이 있는지 확인 ex)student 테이블
    entry = find_entry(manager, table_key);
    if (entry == NULL) {
        entry = (EngineTableLockEntry *)calloc(1, sizeof(*entry)); // 없으면 새로 만든다
        if (entry == NULL) {
            pthread_mutex_unlock(&manager->mutex);
            snprintf(error, error_size, "out of memory while creating table lock");
            return 0;
        }

        //student 이름 복사
        entry->table_key = copy_string(table_key);

        if (entry->table_key == NULL) {
            free(entry);
            pthread_mutex_unlock(&manager->mutex);
            snprintf(error, error_size, "out of memory while copying table lock key");
            return 0;
        }

        // 이 테이블 전용 mutex 생성
        if (pthread_mutex_init(&entry->mutex, NULL) != 0) {
            free(entry->table_key);
            free(entry);
            pthread_mutex_unlock(&manager->mutex);
            snprintf(error, error_size, "failed to initialize table lock mutex");
            return 0;
        }

        //리스트 맨 앞에  붙인다
        entry->next = manager->head;
        manager->head = entry;
    }

    //관리자 락푼다
    pthread_mutex_unlock(&manager->mutex);

    //실제 테이블 락 잡기
    //해당 table mutex 잠근다
    if (pthread_mutex_lock(&entry->mutex) != 0) {
        snprintf(error, error_size, "failed to acquire table lock");
        return 0;
    }

    // 나중에 해제할 수 있게 '내가 어떤 락을 잡았는지' handle에 저장해둔다
    handle->entry = entry;
    return 1;
}

//잡아둔 테이블 락을 푸는 함수
//
void engine_lock_manager_release(EngineTableLockHandle *handle) {
    EngineTableLockEntry *entry = (EngineTableLockEntry *)handle->entry; // 어떤 엔트리였는지 꺼냄

    if (entry != NULL) {
        pthread_mutex_unlock(&entry->mutex); // 그 엔트리의 뮤텍스 언락
        handle->entry = NULL; // 핸들 비운다
    }
}
