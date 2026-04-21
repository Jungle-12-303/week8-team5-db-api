/*
 * sql/parser.h
 *
 * lexer가 만든 토큰 배열을 AST 구조체로 해석하는 parser 계층의 공개 헤더다.
 */
#ifndef PARSER_H
#define PARSER_H

#include "sqlparser/sql/ast.h"
#include "sqlparser/sql/lexer.h"

/* parser가 한 문장을 해석한 결과다. */
typedef struct {
    /* 1이면 파싱 성공, 0이면 실패 */
    int ok;
    /* 성공했을 때 채워지는 AST */
    Statement statement;
    /* 실패했을 때 사용자에게 보여줄 오류 메시지 */
    char message[256];
} ParseResult;

/*
 * 토큰 배열 하나를 Statement 하나로 파싱한다.
 * 현재는 INSERT와 SELECT만 지원한다.
 */
ParseResult parse_statement(const TokenArray *tokens);

#endif
