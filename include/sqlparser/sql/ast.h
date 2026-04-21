/*
 * sql/ast.h
 *
 * parser가 해석한 SQL 문장을 구조체 형태로 담는 헤더다.
 * execution 계층은 문자열 원문 대신 여기 정의된 AST를 받아 실행한다.
 */
#ifndef AST_H
#define AST_H

#include "sqlparser/common/util.h"

/* 현재 프로젝트가 지원하는 문장 종류다. */
typedef enum {
    /* INSERT INTO ... VALUES (...) */
    STATEMENT_INSERT,
    /* SELECT ... FROM ... [WHERE column = value] */
    STATEMENT_SELECT
} StatementType;

/* INSERT 문장을 표현하는 AST 노드다. */
typedef struct {
    /* 데이터를 넣을 대상 테이블 이름 */
    char *table_name;
    /* INSERT에 명시된 컬럼 목록 */
    StringList columns;
    /* 각 컬럼에 대응하는 값 목록 */
    StringList values;
} InsertStatement;

/* SELECT 문장을 표현하는 AST 노드다. */
typedef struct {
    /* 데이터를 읽을 대상 테이블 이름 */
    char *table_name;
    /* 조회할 컬럼 목록. select_all이 1이면 사용하지 않는다. */
    StringList columns;
    /* 1이면 SELECT * 형태, 0이면 특정 컬럼만 조회 */
    int select_all;
    /* 1이면 WHERE 절이 포함된 문장 */
    int has_where;
    /* WHERE 절의 왼쪽 컬럼 이름 */
    char *where_column;
    /* WHERE 절의 오른쪽 비교 값 */
    char *where_value;
} SelectStatement;

/*
 * parser가 반환하는 공통 문장 구조체다.
 * type을 보고 union 안에서 어떤 문장 구조체가 유효한지 판단한다.
 */
typedef struct {
    /* 현재 문장이 INSERT인지 SELECT인지 구분한다. */
    StatementType type;
    /* 문장 종류에 따라 하나의 구조체만 실제로 사용된다. */
    union {
        InsertStatement insert_statement;
        SelectStatement select_statement;
    } as;
} Statement;

/*
 * Statement 내부에서 동적으로 할당한 문자열과 리스트 메모리를 해제한다.
 * Statement는 union 구조이므로 type에 맞는 필드만 정리한다.
 */
void free_statement(Statement *statement);

#endif
