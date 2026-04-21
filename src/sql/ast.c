// AST 구조체의 동적 메모리를 해제하는 구현 파일이다.
/*
 * sql/ast.c
 *
 * parser가 만든 Statement 구조체는 내부에 동적 메모리를 가진다.
 * 이 파일은 그 메모리를 문장 종류에 맞춰 정리하는 역할만 담당한다.
 */
#include "sqlparser/sql/ast.h"

// free 함수를 사용하기 위해 포함한다.
#include <stdlib.h>

/* Statement 내부 문자열과 리스트 메모리를 한 번에 해제한다. */
void free_statement(Statement *statement) {
    /*
     * Statement는 union 구조라서
     * 현재 어떤 문장 종류인지(type)를 먼저 보고
     * 그 문장이 실제로 사용한 메모리만 골라 해제해야 한다.
     */
    // INSERT 문장이라면 INSERT 전용 필드들을 해제한다.
    if (statement->type == STATEMENT_INSERT) {
        // 테이블 이름 문자열을 해제한다.
        free(statement->as.insert_statement.table_name);
        // 해제 후에는 NULL로 초기화해 두 번 해제하는 실수를 줄인다.
        statement->as.insert_statement.table_name = NULL;
        // 컬럼 목록 문자열들을 해제한다.
        string_list_free(&statement->as.insert_statement.columns);
        // 값 목록 문자열들도 해제한다.
        string_list_free(&statement->as.insert_statement.values);
    // SELECT 문장이라면 SELECT 전용 필드들을 해제한다.
    } else if (statement->type == STATEMENT_SELECT) {
        /* SELECT는 table_name, optional WHERE 문자열, 선택 컬럼 목록을 소유한다. */
        free(statement->as.select_statement.table_name);
        statement->as.select_statement.table_name = NULL;
        free(statement->as.select_statement.where_column);
        statement->as.select_statement.where_column = NULL;
        free(statement->as.select_statement.where_value);
        statement->as.select_statement.where_value = NULL;
        string_list_free(&statement->as.select_statement.columns);
    }
}
