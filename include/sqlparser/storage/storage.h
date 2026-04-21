/*
 * storage/storage.h
 *
 * CSV 파일에 행을 쓰고, 읽고, 순회하는 실제 저장소 계층의 공개 헤더다.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include "sqlparser/common/util.h"

#include <stddef.h>

/* CSV에 행을 추가하는 작업의 결과다. */
typedef struct {
    /* 1이면 성공, 0이면 실패 */
    int ok;
    /* 영향을 받은 행 수. append 성공 시 보통 1 */
    int affected_rows;
    /* 새로 쓴 행의 시작 오프셋. 인덱스 등록에 사용한다. */
    long row_offset;
    /* 사용자용 메시지 또는 오류 메시지 */
    char message[256];
} StorageResult;

/* CSV에서 한 행을 읽어 온 결과다. */
typedef struct {
    /* 1이면 성공, 0이면 실패 */
    int ok;
    /* 읽어 온 각 필드 값 */
    StringList fields;
    /* 실패 원인을 담는 메시지 */
    char message[256];
} StorageReadResult;

/*
 * CSV 전체 순회 중 각 행마다 호출할 콜백 타입이다.
 * row_offset은 해당 행이 파일에서 시작하는 바이트 위치다.
 * 1을 반환하면 순회를 계속하고, 0을 반환하면 중단한다.
 */
typedef int (*StorageRowVisitor)(const StringList *fields, long row_offset, void *context, char *error, size_t error_size);

/* CSV 한 줄을 필드 목록으로 파싱한다. */
int csv_parse_line(const char *line, StringList *fields, char *error, size_t error_size);

/* CSV 규칙에 맞게 한 필드 값을 escape한 새 문자열을 만든다. 호출자가 free() 해야 한다. */
char *csv_escape_field(const char *value);

/* table_name.csv 끝에 한 행을 append한다. */
StorageResult append_row_csv(const char *data_dir, const char *table_name, const StringList *row_values);

/* 특정 오프셋에서 시작하는 CSV 행 하나를 읽는다. */
StorageReadResult read_row_at_offset_csv(const char *data_dir, const char *table_name, long row_offset);

/* CSV의 모든 데이터 행을 처음부터 끝까지 순회하며 visitor를 호출한다. */
int scan_rows_csv(const char *data_dir, const char *table_name, StorageRowVisitor visitor, void *context, char *error, size_t error_size);

#endif
