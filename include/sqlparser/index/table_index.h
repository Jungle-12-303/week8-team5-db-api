/*
 * index/table_index.h
 *
 * 테이블별 B+ 트리 인덱스를 관리하는 상위 인덱스 계층의 공개 헤더다.
 * storage 계층과 연결되어 CSV 기준 lazy load / rebuild를 담당한다.
 */
#ifndef SQLPARSER_INDEX_TABLE_INDEX_H
#define SQLPARSER_INDEX_TABLE_INDEX_H

#include "sqlparser/storage/schema.h"

#include <stddef.h>

/* id 조회 결과를 표현하는 구조체다. */
typedef struct {
    /* 1이면 조회 과정 자체는 정상 */
    int ok;
    /* 1이면 id를 찾았고, 0이면 찾지 못함 */
    int found;
    /* 찾았을 때 해당 행의 CSV 오프셋 */
    long row_offset;
    /* 실패했을 때 사용자용 메시지 */
    char message[256];
} TableIndexLookupResult;

/* 메모리에 올라온 모든 테이블 인덱스를 해제한다. */
void table_index_registry_reset(void);
int table_index_registry_init(char *message, size_t message_size);
void table_index_registry_shutdown(void);

/* 특정 테이블 인덱스를 무효화해서 다음 조회 때 다시 만들게 한다. */
void table_index_invalidate(const char *table_name);

/* 특정 테이블 인덱스가 현재 메모리에 로드돼 있는지 확인한다. */
int table_index_is_loaded(const char *table_name);

/*
 * 현재 CSV를 기준으로 다음 자동 id 값을 계산한다.
 * 필요하면 인덱스를 로드하거나 재구성할 수 있다.
 */
int table_index_get_next_id(const Schema *schema, const char *data_dir, int *next_id, char *message, size_t message_size);

/*
 * 방금 append한 행을 인덱스에 등록한다.
 * row_offset은 append_row_csv가 돌려준 파일 오프셋이다.
 */
int table_index_register_row(const Schema *schema, const char *data_dir, int id, long row_offset, char *message, size_t message_size);

/*
 * id로 행 위치를 찾는다.
 * 인덱스가 아직 없거나 무효화된 상태면 CSV를 기준으로 재구성한 뒤 조회한다.
 */
TableIndexLookupResult table_index_find_row(const Schema *schema, const char *data_dir, int id);

/* 테스트용: 다음 register_row 호출을 강제로 실패시키는 플래그를 켠다. */
void table_index_force_next_register_failure(void);

#endif
