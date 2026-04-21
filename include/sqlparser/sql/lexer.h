/*
 * sql/lexer.h
 *
 * SQL 문자열을 parser가 읽기 쉬운 토큰 배열로 쪼개는 계층의 공개 헤더다.
 */
#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

/* lexer가 구분해 내는 토큰 종류다. */
typedef enum {
    TOKEN_IDENTIFIER,
    TOKEN_STRING,
    TOKEN_NUMBER,
    TOKEN_STAR,
    TOKEN_EQUALS,
    TOKEN_COMMA,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_SEMICOLON,
    TOKEN_END
} TokenType;

/* SQL 문자열에서 잘라 낸 토큰 한 개를 표현한다. */
typedef struct {
    /* 토큰의 종류 */
    TokenType type;
    /* 토큰 원문 문자열 */
    char *text;
    /* 원본 SQL 문자열에서 시작한 위치 */
    int position;
} Token;

/* 여러 토큰을 동적으로 저장하는 배열 구조체다. */
typedef struct {
    /* 실제 토큰이 저장되는 배열 */
    Token *items;
    /* 현재 저장된 토큰 개수 */
    int count;
    /* items 배열이 확보한 전체 슬롯 수 */
    int capacity;
} TokenArray;

/*
 * SQL 문자열을 읽어 TokenArray로 분해한다.
 * 실패하면 0을 반환하고 error 버퍼에 원인을 적는다.
 */
int lex_sql(const char *input, TokenArray *tokens, char *error, size_t error_size);

/* TokenArray 내부의 토큰 문자열과 배열 메모리를 정리한다. */
void free_tokens(TokenArray *tokens);

#endif
