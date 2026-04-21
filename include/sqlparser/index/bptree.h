/*
 * index/bptree.h
 *
 * id -> row_offset 매핑을 저장하는 메모리 기반 B+ 트리의 공개 헤더다.
 * SQL 문법을 직접 알지 않고, 정수 키와 long 값만 관리한다.
 */
#ifndef SQLPARSER_INDEX_BPTREE_H
#define SQLPARSER_INDEX_BPTREE_H

#include <stddef.h>

/* 내부 노드 구조체는 구현 파일에 감춘다. */
typedef struct BPlusTreeNode BPlusTreeNode;

/* 외부에 공개되는 B+ 트리 핸들이다. */
typedef struct {
    /* 현재 트리의 루트 노드 */
    BPlusTreeNode *root;
} BPlusTree;

/* 빈 트리로 초기화한다. */
void bptree_init(BPlusTree *tree);

/* 트리 전체 노드를 해제한다. */
void bptree_free(BPlusTree *tree);

/*
 * 새 key -> value를 트리에 넣는다.
 * 중복 key가 들어오면 실패하며 message에 이유를 남긴다.
 */
int bptree_insert(BPlusTree *tree, int key, long value, char *message, size_t message_size);

/* key를 찾아 value를 돌려준다. 찾으면 1, 없으면 0이다. */
int bptree_search(const BPlusTree *tree, int key, long *value);

#endif
