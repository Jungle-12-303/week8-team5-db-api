/*
 * execution/executor.h
 *
 * parser가 만든 AST를 실제 저장소 동작으로 연결하는 실행 계층의 공개 헤더다.
 */
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "sqlparser/sql/ast.h"

#include <stdio.h>

/* 문장 실행 결과를 요약해 돌려주는 구조체다. */
typedef struct {
    /* 1이면 실행 성공, 0이면 실패 */
    int ok;
    /* 영향을 받은 행 수. 예: INSERT 1, SELECT 3 */
    int affected_rows;
    /* 사용자에게 보여줄 실행 결과 또는 오류 메시지 */
    char message[256];
} ExecResult;

/*
 * AST 문장을 실제 schema/data 디렉터리에 대해 실행한다.
 * out은 SELECT 결과 표처럼 사용자에게 보여 줄 출력을 쓰는 스트림이다.
 */
ExecResult execute_statement(const Statement *statement, const char *schema_dir, const char *data_dir, FILE *out);

/*
 * execution 계층이 메모리에 들고 있는 런타임 상태를 정리한다.
 * 현재는 주로 테이블 인덱스 레지스트리 해제에 사용된다.
 */
void execution_runtime_reset(void);

#endif
