/*
 * index/table_index.c
 *
 * 이 파일은 "테이블별 인덱스 운영"을 담당한다.
 * B+ 트리 자체는 bptree.c가 구현하고,
 * 여기서는 각 테이블마다:
 * - 인덱스가 이미 메모리에 있는지
 * - 아직 없으면 CSV에서 재구성해야 하는지
 * - 다음 자동 id가 무엇인지
 * 를 관리한다.
 */
#include "sqlparser/index/table_index.h"

#include "sqlparser/common/util.h"
#include "sqlparser/index/bptree.h"
#include "sqlparser/storage/storage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *table_name;
    int loaded;
    BPlusTree tree;
    int next_id;
} TableIndex;

typedef struct {
    TableIndex *items;
    int count;
    int capacity;
} TableIndexRegistry;

typedef struct {
    BPlusTree *tree;
    int next_id;
} RebuildContext;

static TableIndexRegistry registry = {0};
static int force_next_register_failure = 0;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static int registry_mutex_initialized = 0;

int table_index_registry_init(char *message, size_t message_size) {
    if (registry_mutex_initialized) {
        return 1;
    }

    if (pthread_mutex_init(&registry_mutex, NULL) != 0) {
        snprintf(message, message_size, "failed to initialize registry mutex");
        return 0;
    }

    registry_mutex_initialized = 1;
    return 1;
}

void table_index_registry_shutdown(void) {
    if (registry_mutex_initialized) {
        pthread_mutex_destroy(&registry_mutex);
        registry_mutex_initialized = 0;
    }
}

/* 레지스트리에서 특정 테이블 이름의 인덱스 엔트리를 찾는다. */
static TableIndex *find_table_index_locked(const char *table_name) {
    int index;

    /* 메모리에 이미 올라와 있는 테이블 인덱스가 있는지 선형 탐색으로 찾는다. */
    for (index = 0; index < registry.count; index++) {
        if (strcmp(registry.items[index].table_name, table_name) == 0) {
            return &registry.items[index];
        }
    }

    return NULL;
}

/* 테이블 인덱스 엔트리를 찾고, 없으면 새로 생성한다. */
static TableIndex *get_or_create_table_index(const char *table_name, char *message, size_t message_size) {
    TableIndex *existing = find_table_index_locked(table_name);
    TableIndex *new_items;
    int new_capacity;
    TableIndex *entry;

    /* 이미 있으면 새로 만들 필요 없이 기존 엔트리를 재사용한다. */
    if (existing != NULL) {
        return existing;
    }

    /* 레지스트리 배열이 꽉 차면 두 배씩 늘려 새 엔트리를 담을 공간을 만든다. */
    if (registry.count == registry.capacity) {
        new_capacity = registry.capacity == 0 ? 4 : registry.capacity * 2;
        new_items = (TableIndex *)realloc(registry.items, (size_t)new_capacity * sizeof(TableIndex));
        if (new_items == NULL) {
            snprintf(message, message_size, "out of memory while creating table index");
            return NULL;
        }

        registry.items = new_items;
        registry.capacity = new_capacity;
    }

    /* 새 엔트리는 0으로 초기화한 뒤 기본 next_id를 1로 시작한다. */
    entry = &registry.items[registry.count];
    memset(entry, 0, sizeof(*entry));
    entry->table_name = copy_string(table_name);
    if (entry->table_name == NULL) {
        snprintf(message, message_size, "out of memory while copying table name");
        return NULL;
    }

    bptree_init(&entry->tree);
    entry->next_id = 1;
    registry.count++;
    return entry;
}

/*
 * CSV 한 행을 읽을 때마다 호출되는 재구성 콜백이다.
 *
 * 각 행에서:
 * - 현재 행 순서에 대응하는 내부 id를 계산하고
 * - 그 내부 id와 CSV 오프셋을 B+ 트리에 삽입하고
 * - 다음 행이 사용할 next_id를 하나 증가시킨다.
 */
static int rebuild_row(const StringList *fields, long row_offset, void *context, char *error, size_t error_size) {
    RebuildContext *rebuild = (RebuildContext *)context;
    int internal_id = rebuild->next_id;

    (void)fields;
    if (!bptree_insert(rebuild->tree, internal_id, row_offset, error, error_size)) {
        return 0;
    }

    rebuild->next_id++;
    return 1;
}

/*
 * 특정 테이블 인덱스가 메모리에 준비돼 있는지 보장한다.
 *
 * 이미 있으면 그대로 사용하고,
 * 아직 없으면 CSV를 스캔해 B+ 트리를 다시 만든다.
 */
static int ensure_loaded(const Schema *schema, const char *data_dir, TableIndex **out_index, char *message, size_t message_size) {
    TableIndex *entry;
    RebuildContext rebuild;

    pthread_mutex_lock(&registry_mutex);
    entry = get_or_create_table_index(schema->storage_name, message, message_size);
    pthread_mutex_unlock(&registry_mutex);
    if (entry == NULL) {
        return 0;
    }

    /*
     * entry->loaded == 0 이면 아직 메모리 인덱스가 비어 있다는 뜻이다.
     * 이 경우 CSV 전체를 다시 훑어 B+ 트리를 재구성한다.
     */
    if (!entry->loaded) {
        bptree_free(&entry->tree);
        bptree_init(&entry->tree);
        rebuild.tree = &entry->tree;
        rebuild.next_id = 1;

        /* CSV 각 행마다 행 순서 기반 내부 id -> offset 매핑을 트리에 쌓는다. */
        if (!scan_rows_csv(data_dir, schema->storage_name, rebuild_row, &rebuild, message, message_size)) {
            bptree_free(&entry->tree);
            bptree_init(&entry->tree);
            entry->loaded = 0;
            entry->next_id = 1;
            return 0;
        }

        /* 재구성이 끝나면 이후 INSERT 자동 id 계산용 next_id도 함께 갱신한다. */
        entry->loaded = 1;
        entry->next_id = rebuild.next_id;
    }

    *out_index = entry;
    return 1;
}

/* 모든 테이블 인덱스를 메모리에서 제거하고 레지스트리를 초기 상태로 되돌린다. */
void table_index_registry_reset(void) {
    int index;

    pthread_mutex_lock(&registry_mutex);
    for (index = 0; index < registry.count; index++) {
        free(registry.items[index].table_name);
        bptree_free(&registry.items[index].tree);
    }

    free(registry.items);
    registry.items = NULL;
    registry.count = 0;
    registry.capacity = 0;
    force_next_register_failure = 0;
    pthread_mutex_unlock(&registry_mutex);
}

/* 특정 테이블 인덱스를 "다시 재구성 필요" 상태로 무효화한다. */
void table_index_invalidate(const char *table_name) {
    TableIndex *entry;

    pthread_mutex_lock(&registry_mutex);
    entry = find_table_index_locked(table_name);
    if (entry == NULL) {
        pthread_mutex_unlock(&registry_mutex);
        return;
    }

    bptree_free(&entry->tree);
    bptree_init(&entry->tree);
    entry->loaded = 0;
    entry->next_id = 1;
    pthread_mutex_unlock(&registry_mutex);
}

/* 특정 테이블 인덱스가 현재 메모리에 로드돼 있는지 확인한다. */
int table_index_is_loaded(const char *table_name) {
    TableIndex *entry;
    int loaded;

    pthread_mutex_lock(&registry_mutex);
    entry = find_table_index_locked(table_name);
    loaded = entry != NULL && entry->loaded;
    pthread_mutex_unlock(&registry_mutex);
    return loaded;
}

/* 자동 증가 id에 사용할 next_id 값을 구한다. */
int table_index_get_next_id(const Schema *schema, const char *data_dir, int *next_id, char *message, size_t message_size) {
    TableIndex *entry;

    if (!ensure_loaded(schema, data_dir, &entry, message, message_size)) {
        return 0;
    }

    *next_id = entry->next_id;
    return 1;
}

/* CSV에 새 행이 추가된 뒤, 그 id와 오프셋을 메모리 인덱스에 등록한다. */
int table_index_register_row(const Schema *schema, const char *data_dir, int id, long row_offset, char *message, size_t message_size) {
    TableIndex *entry;

    /* 테스트에서는 한 번의 등록 실패를 강제로 유도해 복구 경로를 검증할 수 있다. */
    if (force_next_register_failure) {
        force_next_register_failure = 0;
        snprintf(message, message_size, "forced index registration failure");
        return 0;
    }

    /* 등록 전에 해당 테이블 인덱스가 메모리에 준비돼 있는지 먼저 보장한다. */
    if (!ensure_loaded(schema, data_dir, &entry, message, message_size)) {
        return 0;
    }

    /* 새로 append한 행의 id와 파일 오프셋을 B+ 트리에 즉시 반영한다. */
    if (!bptree_insert(&entry->tree, id, row_offset, message, message_size)) {
        return 0;
    }

    /* 방금 들어온 id가 현재 next_id 이상이면 다음 자동 id도 밀어 올린다. */
    if (id >= entry->next_id) {
        entry->next_id = id + 1;
    }

    return 1;
}

/* id 하나를 받아 인덱스에서 해당 CSV 오프셋을 찾는다. */
TableIndexLookupResult table_index_find_row(const Schema *schema, const char *data_dir, int id) {
    TableIndexLookupResult result = {0};
    TableIndex *entry;

    /* 첫 조회라면 여기서 CSV 기반 재구성이 일어날 수 있다. */
    if (!ensure_loaded(schema, data_dir, &entry, result.message, sizeof(result.message))) {
        return result;
    }

    /* 재구성된 B+ 트리에서 id를 검색해 대응하는 CSV 오프셋을 찾는다. */
    result.ok = 1;
    result.found = bptree_search(&entry->tree, id, &result.row_offset);
    return result;
}

/* 테스트용: 다음 인덱스 등록 한 번을 강제로 실패시키도록 설정한다. */
void table_index_force_next_register_failure(void) {
    pthread_mutex_lock(&registry_mutex);
    force_next_register_failure = 1;
    pthread_mutex_unlock(&registry_mutex);
}
