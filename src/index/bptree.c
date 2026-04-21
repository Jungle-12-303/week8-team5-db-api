/*
 * index/bptree.c
 *
 * 이 파일은 index 계층에서 사용하는 메모리 기반 B+ 트리를 구현한다.
 * table_index.c가 테이블 단위 인덱스를 언제 만들고 다시 로드할지 결정한다면,
 * 이 파일은 그 아래에서 노드 생성, 키/값 삽입, 노드 분할, 키 검색 같은
 * 트리 자체의 동작만 담당한다.
 */
#include "sqlparser/index/bptree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 삽입이 끝난 뒤 한 노드가 유지할 수 있는 최대 키 개수다.
 *
 * 아래 배열들은 이 한도보다 일부러 한 칸 더 크게 잡아 둔다.
 * 삽입 중에는 노드가 잠시 하나 더 많은 키를 가진 뒤,
 * overflow 상태를 해소하기 위해 두 노드로 분할되기 때문이다.
 */
#define BPTREE_MAX_KEYS 3

struct BPlusTreeNode {
    /*
     * 리프 노드면 1, 내부 노드면 0이다.
     *
     * 리프 노드는 실제 key/value 쌍을 저장하고,
     * 내부 노드는 자식 경계를 나타내는 분기 키와 자식 포인터를 저장한다.
     */
    int is_leaf;

    /* 현재 이 노드에 유효하게 들어 있는 키 개수다. */
    int key_count;

    /*
     * 이 노드가 유지하는 정렬된 키 배열이다.
     *
     * 배열 길이를 BPTREE_MAX_KEYS + 1로 둔 이유는
     * 삽입 시 새 키를 먼저 넣고 overflow가 발생하면 나중에 분할하기 위해서다.
     */
    int keys[BPTREE_MAX_KEYS + 1];

    /*
     * 내부 노드에서 사용하는 자식 포인터 배열이다.
     *
     * 내부 노드는 키가 N개면 최대 N + 1개의 자식을 가질 수 있고,
     * 여기에 삽입 중 임시 overflow 상태까지 감안해 한 칸을 더 둔다.
     */
    struct BPlusTreeNode *children[BPTREE_MAX_KEYS + 2];

    /*
     * 리프 노드에서 각 키와 짝을 이루는 값이다.
     *
     * 이 프로젝트에서는 internal id 키가 가리키는 CSV row offset을 저장한다.
     */
    long values[BPTREE_MAX_KEYS + 1];

    /*
     * 리프 노드끼리 옆으로 연결할 때 사용하는 포인터다.
     *
     * B+ 트리는 보통 리프를 왼쪽에서 오른쪽으로 연결해 두므로,
     * 범위 조회 시 내부 노드로 다시 올라가지 않고 리프만 따라 이동할 수 있다.
     */
    struct BPlusTreeNode *next;
};

typedef struct {
    /* 삽입이 성공적으로 끝났는지 나타낸다. */
    int ok;

    /*
     * 현재 노드가 분할되었는지 나타낸다.
     * 분할되었다면 부모 노드는 promoted_key와 새 오른쪽 자식을 받아 반영해야 한다.
     */
    int split;

    /*
     * 분할 후 부모에게 올려 보낼 분기 키다.
     *
     * 리프 분할에서는 새 오른쪽 리프의 첫 키가 되고,
     * 내부 노드 분할에서는 가운데 키를 부모로 승격시킨다.
     */
    int promoted_key;

    /* 분할 과정에서 새로 만들어진 오른쪽 노드다. */
    struct BPlusTreeNode *right_node;

    /* 공개 insert API가 그대로 전달할 수 있는 오류 메시지다. */
    char message[256];
} InsertState;

static InsertState insert_into_node(struct BPlusTreeNode *node, int key, long value);

/*
 * 0으로 초기화된 새 노드를 할당한다.
 *
 * calloc()을 사용하면 배열과 포인터가 모두 0으로 초기화되므로,
 * 비어 있는 자식 슬롯, 리프 값, next 포인터 상태를 예측 가능하게 유지할 수 있다.
 */
static struct BPlusTreeNode *create_node(int is_leaf) {
    struct BPlusTreeNode *node = (struct BPlusTreeNode *)calloc(1, sizeof(struct BPlusTreeNode));
    if (node != NULL) {
        node->is_leaf = is_leaf;
    }
    return node;
}

/*
 * 서브트리를 재귀적으로 해제한다.
 *
 * 내부 노드는 자신의 하위 노드를 소유하므로 자식부터 먼저 해제하고,
 * 마지막에 현재 노드를 해제한다. 리프 노드는 별도의 자식을 가지지 않는다.
 */
static void free_node(struct BPlusTreeNode *node) {
    int index;

    if (node == NULL) {
        return;
    }

    if (!node->is_leaf) {
        for (index = 0; index <= node->key_count; index++) {
            free_node(node->children[index]);
        }
    }

    free(node);
}

/*
 * 내부 노드에서 어떤 자식 포인터를 따라 내려가야 하는지 계산한다.
 *
 * 내부 노드의 키는 분기 기준으로 쓰인다.
 * - keys[0]보다 작은 값은 children[0]으로 간다.
 * - keys[i - 1] 이상 keys[i] 미만인 값은 children[i]로 간다.
 * - 마지막 분기 키 이상인 값은 가장 오른쪽 자식으로 간다.
 */
static int find_child_index(const struct BPlusTreeNode *node, int key) {
    int index = 0;

    while (index < node->key_count && key >= node->keys[index]) {
        index++;
    }

    return index;
}

/* 재귀 삽입 과정 전체로 전파할 실패 결과를 만든다. */
static InsertState make_error(const char *message) {
    InsertState state = {0};
    state.ok = 0;
    snprintf(state.message, sizeof(state.message), "%s", message);
    return state;
}

/*
 * 리프 노드에 key/value 쌍을 삽입한다.
 *
 * 처리 순서는 다음과 같다.
 * 1. 정렬 순서를 유지할 삽입 위치를 찾는다.
 * 2. internal id는 유일해야 하므로 중복 키면 실패한다.
 * 3. 뒤쪽 항목들을 한 칸씩 오른쪽으로 민다.
 * 4. 새 key/value를 삽입한다.
 * 5. 키 수가 BPTREE_MAX_KEYS를 넘으면 리프를 왼쪽/오른쪽으로 분할한다.
 *
 * 리프 분할 후에는 다음 상태를 만든다.
 * - 왼쪽 노드는 작은 절반을 유지한다.
 * - 새 오른쪽 리프는 큰 절반을 가져간다.
 * - node->next를 다시 연결해 리프 연결 리스트를 유지한다.
 * - 오른쪽 리프의 첫 키를 부모에게 올릴 promoted_key로 사용한다.
 */
static InsertState insert_into_leaf(struct BPlusTreeNode *node, int key, long value) {
    InsertState state = {0};
    int insert_index = 0;
    int index;
    struct BPlusTreeNode *right;
    int split_index;

    while (insert_index < node->key_count && node->keys[insert_index] < key) {
        insert_index++;
    }

    if (insert_index < node->key_count && node->keys[insert_index] == key) {
        return make_error("duplicate id key");
    }

    for (index = node->key_count; index > insert_index; index--) {
        node->keys[index] = node->keys[index - 1];
        node->values[index] = node->values[index - 1];
    }

    node->keys[insert_index] = key;
    node->values[insert_index] = value;
    node->key_count++;

    state.ok = 1;
    if (node->key_count <= BPTREE_MAX_KEYS) {
        return state;
    }

    right = create_node(1);
    if (right == NULL) {
        return make_error("out of memory while splitting B+ tree leaf");
    }

    /* 가운데 부근에서 나눠 두 리프가 비슷한 크기를 유지하도록 한다. */
    split_index = node->key_count / 2;
    right->key_count = node->key_count - split_index;
    for (index = 0; index < right->key_count; index++) {
        right->keys[index] = node->keys[split_index + index];
        right->values[index] = node->values[split_index + index];
    }

    node->key_count = split_index;

    /* 이후 순차 스캔이 가능하도록 리프 연결 리스트를 보존한다. */
    right->next = node->next;
    node->next = right;

    state.split = 1;
    state.promoted_key = right->keys[0];
    state.right_node = right;
    return state;
}

/*
 * 내부 노드 삽입은 먼저 올바른 자식에게 재귀적으로 위임한다.
 *
 * 재귀 흐름은 다음과 같다.
 * 1. 해당 키가 들어가야 할 자식 서브트리를 고른다.
 * 2. 그 자식에게 삽입을 수행한다.
 * 3. 자식이 분할되지 않았다면 현재 노드는 구조 변경이 필요 없다.
 * 4. 자식이 분할되었다면 promoted_key와 새 오른쪽 자식을 현재 노드에 반영한다.
 * 5. 현재 노드도 overflow되면 다시 분할하고 그 결과를 부모로 올린다.
 */
static InsertState insert_into_internal(struct BPlusTreeNode *node, int key, long value) {
    InsertState child_state;
    InsertState state = {0};
    int child_index = find_child_index(node, key);
    int index;
    int mid_index;
    struct BPlusTreeNode *right;

    child_state = insert_into_node(node->children[child_index], key, value);
    if (!child_state.ok) {
        return child_state;
    }

    state.ok = 1;
    if (!child_state.split) {
        return state;
    }

    /* 승격된 키와 새 자식 포인터가 들어갈 자리를 먼저 만든다. */
    for (index = node->key_count; index > child_index; index--) {
        node->keys[index] = node->keys[index - 1];
    }
    for (index = node->key_count + 1; index > child_index + 1; index--) {
        node->children[index] = node->children[index - 1];
    }

    node->keys[child_index] = child_state.promoted_key;
    node->children[child_index + 1] = child_state.right_node;
    node->key_count++;

    if (node->key_count <= BPTREE_MAX_KEYS) {
        return state;
    }

    right = create_node(0);
    if (right == NULL) {
        return make_error("out of memory while splitting B+ tree internal node");
    }

    /*
     * 가운데 분기 키를 기준으로 내부 노드를 분할한다.
     *
     * 가운데 키 자체는 부모로 올라가므로 왼쪽/오른쪽 어느 쪽에도 남지 않는다.
     * 가운데보다 오른쪽에 있는 키들과 그 자식 포인터들은 새 오른쪽 내부 노드로 이동한다.
     */
    mid_index = node->key_count / 2;
    state.split = 1;
    state.promoted_key = node->keys[mid_index];
    state.right_node = right;

    right->key_count = node->key_count - mid_index - 1;
    for (index = 0; index < right->key_count; index++) {
        right->keys[index] = node->keys[mid_index + 1 + index];
    }
    for (index = 0; index <= right->key_count; index++) {
        right->children[index] = node->children[mid_index + 1 + index];
    }

    node->key_count = mid_index;
    return state;
}

/*
 * 노드 종류에 따라 적절한 삽입 함수로 분기한다.
 *
 * 리프 노드는 실제 key/value를 저장하고,
 * 내부 노드는 아래 자식으로 재귀 호출한 뒤 분할 결과를 흡수할 수 있다.
 */
static InsertState insert_into_node(struct BPlusTreeNode *node, int key, long value) {
    if (node->is_leaf) {
        return insert_into_leaf(node, key, value);
    }

    return insert_into_internal(node, key, value);
}

/* 아직 루트가 없는 비어 있는 트리를 초기화한다. */
void bptree_init(BPlusTree *tree) {
    tree->root = NULL;
}

/* 트리 전체 메모리를 해제하고 다시 빈 상태로 되돌린다. */
void bptree_free(BPlusTree *tree) {
    free_node(tree->root);
    tree->root = NULL;
}

/*
 * key -> value 매핑을 삽입하는 공개 API다.
 *
 * 특별히 처리해야 하는 경우는 다음과 같다.
 * - 트리가 비어 있으면 단일 리프 루트를 만들어 첫 항목을 저장한다.
 * - 재귀 삽입 결과 루트가 분할되면 그 위에 새 루트를 만든다.
 *
 * 트리 높이가 증가하는 경우는 루트 분할이 일어날 때뿐이다.
 */
int bptree_insert(BPlusTree *tree, int key, long value, char *message, size_t message_size) {
    InsertState state;
    struct BPlusTreeNode *new_root;

    if (tree->root == NULL) {
        tree->root = create_node(1);
        if (tree->root == NULL) {
            snprintf(message, message_size, "out of memory while creating B+ tree root");
            return 0;
        }

        tree->root->keys[0] = key;
        tree->root->values[0] = value;
        tree->root->key_count = 1;
        return 1;
    }

    state = insert_into_node(tree->root, key, value);
    if (!state.ok) {
        snprintf(message, message_size, "%s", state.message);
        return 0;
    }

    if (!state.split) {
        return 1;
    }

    new_root = create_node(0);
    if (new_root == NULL) {
        snprintf(message, message_size, "out of memory while growing B+ tree root");
        return 0;
    }

    /*
     * 기존 루트는 새 루트의 왼쪽 자식이 되고,
     * 분할 결과가 제공한 오른쪽 자식과 분기 키를 함께 연결한다.
     */
    new_root->keys[0] = state.promoted_key;
    new_root->children[0] = tree->root;
    new_root->children[1] = state.right_node;
    new_root->key_count = 1;
    tree->root = new_root;
    return 1;
}

/*
 * key를 검색해 대응하는 payload value를 반환한다.
 *
 * 검색은 먼저 내부 노드를 따라 내려가 대상 리프를 찾은 뒤,
 * 해당 리프 안에서 선형 탐색을 수행한다.
 * 이 구현에서는 각 리프가 매우 작도록 유지되므로 짧은 선형 탐색이면 충분하다.
 */
int bptree_search(const BPlusTree *tree, int key, long *value) {
    struct BPlusTreeNode *node = tree->root;
    int index;

    while (node != NULL && !node->is_leaf) {
        node = node->children[find_child_index(node, key)];
    }

    if (node == NULL) {
        return 0;
    }

    for (index = 0; index < node->key_count; index++) {
        if (node->keys[index] == key) {
            if (value != NULL) {
                *value = node->values[index];
            }
            return 1;
        }
    }

    return 0;
}
