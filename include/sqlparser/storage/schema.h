/*
 * storage/schema.h
 *
 * 테이블 스키마(.meta)와 CSV 헤더를 읽고 검증하는 storage 하위 계층의 공개 헤더다.
 */
#ifndef SCHEMA_H
#define SCHEMA_H

#include "sqlparser/common/util.h"

/* 실행에 필요한 테이블 스키마 정보다. */
typedef struct {
    /* SQL에서 사용하는 논리적 테이블 이름 */
    char *table_name;
    /* 실제 CSV 파일 이름에 쓰이는 저장소 이름 */
    char *storage_name;
    /* 테이블 컬럼 순서 */
    StringList columns;
} Schema;

/* 스키마 로딩 결과 구조체다. */
typedef struct {
    /* 1이면 로딩/검증 성공, 0이면 실패 */
    int ok;
    /* 성공했을 때 채워지는 스키마 */
    Schema schema;
    /* 실패 원인을 담는 메시지 */
    char message[256];
} SchemaResult;

/*
 * schema 디렉터리와 data 디렉터리를 기준으로 테이블 스키마를 읽는다.
 * .meta 내용과 CSV 헤더가 서로 맞는지도 함께 검증한다.
 */
SchemaResult load_schema(const char *schema_dir, const char *data_dir, const char *table_name);

/* 특정 컬럼 이름의 위치를 찾아 인덱스를 돌려준다. 없으면 -1이다. */
int schema_find_column(const Schema *schema, const char *column_name);

/*
 * 사용자 스키마에 예약 이름 `id`가 포함되어 있는지 검사한다.
 *
 * 현재 설계에서 `id`는 숨은 내부 PK `__internal_id`를 가리키는 SQL 표면 이름으로
 * 예약되어 있으므로, 사용자 스키마 컬럼명으로는 사용할 수 없다.
 *
 * 반환값:
 * - 1: 예약 이름 `id`가 있음
 * - 0: 예약 이름 `id`가 없음
 */
int schema_has_reserved_id_column(const Schema *schema);

/* Schema 내부의 동적 메모리를 정리한다. */
void free_schema(Schema *schema);

#endif
