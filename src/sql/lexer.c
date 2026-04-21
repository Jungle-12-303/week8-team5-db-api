// lexer는 SQL 원문을 토큰 목록으로 바꿔 주는 단계다.
/*
 * sql/lexer.c
 *
 * lexer는 SQL 원문을 parser가 처리할 수 있는 토큰 배열로 바꾸는 단계다.
 * 글자 단위 입력을 의미 있는 조각(키워드, 식별자, 문자열, 기호)으로 분리한다고 이해하면 된다.
 */
#include "sqlparser/sql/lexer.h"

// 문자열 복사 같은 유틸 함수를 사용한다.
#include "sqlparser/common/util.h"

// 문자 종류 판별을 위해 포함한다.
#include <ctype.h>
// 에러 메시지 생성을 위해 포함한다.
#include <stdio.h>
// 동적 메모리 할당을 위해 포함한다.
#include <stdlib.h>
// memcpy를 쓰기 위해 포함한다.
#include <string.h>

/* 식별자의 첫 글자가 될 수 있는 문자인지 검사한다. */
static int is_identifier_start(unsigned char value) {
    // ASCII 영문, 언더스코어, 숫자 외에도 UTF-8 바이트를 식별자 시작으로 허용한다.
    return isalpha(value) || value == '_' || isdigit(value) || value >= 0x80;
}

/* 식별자의 두 번째 글자 이후에 올 수 있는 문자인지 검사한다. */
static int is_identifier_continue(unsigned char value) {
    // 식별자 중간 문자도 시작 문자와 같은 규칙으로 읽는다.
    return isalnum(value) || value == '_' || value >= 0x80;
}

/* TokenArray 동적 배열에 토큰 하나를 추가한다. */
static int push_token(TokenArray *tokens, TokenType type, const char *text, int position) {
    // 배열이 부족할 때 새로 잡을 크기다.
    int new_capacity;
    // realloc 결과를 임시로 담을 포인터다.
    Token *new_items;
    // 토큰 문자열 복사본을 담을 포인터다.
    char *copy;

    // 현재 토큰 배열이 꽉 찼으면 크기를 늘린다.
    if (tokens->count == tokens->capacity) {
        new_capacity = tokens->capacity == 0 ? 8 : tokens->capacity * 2;
        new_items = (Token *)realloc(tokens->items, (size_t)new_capacity * sizeof(Token));
        if (new_items == NULL) {
            return 0;
        }

        // 늘어난 배열을 실제 토큰 배열로 교체한다.
        tokens->items = new_items;
        tokens->capacity = new_capacity;
    }

    // 토큰 문자열은 원본 SQL과 독립적으로 보관하기 위해 복사한다.
    copy = copy_string(text);
    if (copy == NULL) {
        return 0;
    }

    // 새 토큰 한 개를 배열 끝에 채운다.
    tokens->items[tokens->count].type = type;
    tokens->items[tokens->count].text = copy;
    tokens->items[tokens->count].position = position;
    // 토큰 개수를 하나 늘린다.
    tokens->count++;
    return 1;
}

/* 한 글자짜리 기호 토큰을 쉽게 추가하는 래퍼 함수다. */
static int push_simple_token(TokenArray *tokens, TokenType type, char value, int position) {
    // 한 글자 토큰을 C 문자열 형태로 만들기 위한 작은 버퍼다.
    char text[2];

    // 첫 칸에는 실제 문자 하나를 넣는다.
    text[0] = value;
    // 두 번째 칸에는 문자열 끝 표시를 넣는다.
    text[1] = '\0';
    // 일반 토큰 추가 함수에 넘겨 재사용한다.
    return push_token(tokens, type, text, position);
}

/* 식별자나 숫자처럼 연속된 문자 묶음을 읽어 토큰 하나로 만든다. */
static int lex_word(const char *input, int *index, TokenArray *tokens) {
    // 이 단어가 시작된 위치다.
    int start = *index;
    // 단어 길이를 센다.
    int length = 0;
    // 단어 내용을 잠시 담아 둘 버퍼다.
    char *buffer;
    // 숫자인지 식별자인지 최종 토큰 타입이다.
    TokenType type;
    // push_token 성공 여부를 저장한다.
    int ok;

    // 영문자, 숫자, 언더스코어가 이어지는 동안 한 단어로 본다.
    while (is_identifier_continue((unsigned char)input[*index])) {
        (*index)++;
        length++;
    }

    // 길이만큼 새 버퍼를 만들어 단어를 복사한다.
    buffer = (char *)malloc((size_t)length + 1);
    if (buffer == NULL) {
        return 0;
    }

    // 원본 SQL에서 start부터 length만큼 복사한다.
    memcpy(buffer, input + start, (size_t)length);
    buffer[length] = '\0';

    // 첫 글자가 숫자면 숫자 토큰, 아니면 식별자 토큰으로 분류한다.
    type = isdigit((unsigned char)buffer[0]) ? TOKEN_NUMBER : TOKEN_IDENTIFIER;
    ok = push_token(tokens, type, buffer, start);
    // push_token 내부에서 복사했으니 임시 버퍼는 해제한다.
    free(buffer);
    return ok;
}

/* 작은따옴표 문자열 리터럴을 읽어 TOKEN_STRING으로 만든다. */
static int lex_string(const char *input, int *index, TokenArray *tokens, char *error, size_t error_size) {
    // 문자열 리터럴이 시작된 원래 위치다.
    int start = *index;
    // 문자열 내용을 쌓아 갈 동적 버퍼다.
    char *buffer = NULL;
    // 현재까지 저장한 문자 수다.
    size_t length = 0;
    // 현재 확보된 버퍼 크기다.
    size_t capacity = 0;

    // 시작 따옴표(')는 건너뛴다.
    (*index)++;

    // 닫는 따옴표를 만날 때까지 문자열 내용을 읽는다.
    while (input[*index] != '\0') {
        char current = input[*index];
        char *new_buffer;
        size_t new_capacity;

        // 작은따옴표를 만났을 때 문자열 종료인지 escaped quote인지 확인한다.
        if (current == '\'') {
            // SQL에서는 '' 두 개를 써서 문자열 안의 ' 하나를 표현한다.
            if (input[*index + 1] == '\'') {
                current = '\'';
                (*index)++;
            } else {
                // 진짜 종료 따옴표면 문자열 읽기를 끝낸다.
                (*index)++;
                break;
            }
        }

        // 버퍼가 부족하면 두 배씩 늘린다.
        if (length + 1 >= capacity) {
            new_capacity = capacity == 0 ? 16 : capacity * 2;
            new_buffer = (char *)realloc(buffer, new_capacity);
            if (new_buffer == NULL) {
                free(buffer);
                snprintf(error, error_size, "out of memory while lexing string");
                return 0;
            }

            buffer = new_buffer;
            capacity = new_capacity;
        }

        // 현재 문자를 버퍼에 추가한다.
        buffer[length++] = current;
        (*index)++;
    }

    // 마지막에 닫는 따옴표가 없었다면 잘못된 SQL 문자열이다.
    if (input[*index - 1] != '\'') {
        free(buffer);
        snprintf(error, error_size, "unterminated string literal at position %d", start);
        return 0;
    }

    // 빈 문자열이었다면 빈 문자열 버퍼를 직접 만든다.
    if (buffer == NULL) {
        buffer = copy_string("");
        if (buffer == NULL) {
            snprintf(error, error_size, "out of memory while lexing string");
            return 0;
        }
    } else {
        // 일반 문자열이었다면 마지막에 '\0'을 붙인다.
        buffer[length] = '\0';
    }

    // 완성된 문자열 내용을 TOKEN_STRING 토큰으로 저장한다.
    if (!push_token(tokens, TOKEN_STRING, buffer, start)) {
        free(buffer);
        snprintf(error, error_size, "out of memory while storing string token");
        return 0;
    }

    // push_token 안에서 다시 복사했으므로 임시 버퍼는 해제한다.
    free(buffer);
    return 1;
}

/* SQL 원문 전체를 순회하며 토큰 배열을 완성하는 lexer 진입점이다. */
int lex_sql(const char *input, TokenArray *tokens, char *error, size_t error_size) {
    // 원본 SQL 문자열을 왼쪽부터 훑어 갈 인덱스다.
    int index;

    // 문자열 끝('\0')을 만날 때까지 계속 토큰을 만든다.
    for (index = 0; input[index] != '\0'; ) {
        char current = input[index];

        // 공백은 의미 없는 구분자이므로 건너뛴다.
        if (isspace((unsigned char)current)) {
            index++;
            continue;
        }

        // 식별자나 숫자라면 단어 토큰으로 읽는다.
        if (is_identifier_start((unsigned char)current)) {
            if (!lex_word(input, &index, tokens)) {
                snprintf(error, error_size, "out of memory while lexing SQL");
                return 0;
            }
            continue;
        }

        // 작은따옴표로 시작하면 문자열 리터럴로 읽는다.
        if (current == '\'') {
            if (!lex_string(input, &index, tokens, error, error_size)) {
                return 0;
            }
            continue;
        }

        // * 는 단독 토큰이다.
        if (current == '*') {
            if (!push_simple_token(tokens, TOKEN_STAR, current, index)) {
                snprintf(error, error_size, "out of memory while lexing SQL");
                return 0;
            }
            index++;
            continue;
        }

        // = 은 WHERE 비교 연산자 토큰이다.
        if (current == '=') {
            if (!push_simple_token(tokens, TOKEN_EQUALS, current, index)) {
                snprintf(error, error_size, "out of memory while lexing SQL");
                return 0;
            }
            index++;
            continue;
        }

        // , 는 컬럼/값 구분 토큰이다.
        if (current == ',') {
            if (!push_simple_token(tokens, TOKEN_COMMA, current, index)) {
                snprintf(error, error_size, "out of memory while lexing SQL");
                return 0;
            }
            index++;
            continue;
        }

        // 왼쪽 괄호도 단독 토큰이다.
        if (current == '(') {
            if (!push_simple_token(tokens, TOKEN_LPAREN, current, index)) {
                snprintf(error, error_size, "out of memory while lexing SQL");
                return 0;
            }
            index++;
            continue;
        }

        // 오른쪽 괄호도 단독 토큰이다.
        if (current == ')') {
            if (!push_simple_token(tokens, TOKEN_RPAREN, current, index)) {
                snprintf(error, error_size, "out of memory while lexing SQL");
                return 0;
            }
            index++;
            continue;
        }

        // 세미콜론은 SQL 문장 끝을 나타낸다.
        if (current == ';') {
            if (!push_simple_token(tokens, TOKEN_SEMICOLON, current, index)) {
                snprintf(error, error_size, "out of memory while lexing SQL");
                return 0;
            }
            index++;
            continue;
        }

        // 여기까지 어느 규칙에도 안 맞으면 지원하지 않는 문자다.
        snprintf(error, error_size, "unexpected character '%c' at position %d", current, index);
        return 0;
    }

    // parser가 끝을 알 수 있도록 마지막에 TOKEN_END를 하나 넣는다.
    if (!push_token(tokens, TOKEN_END, "", index)) {
        snprintf(error, error_size, "out of memory while finalizing SQL tokens");
        return 0;
    }

    return 1;
}

/* lexer가 만든 토큰 배열과 내부 문자열 메모리를 모두 정리한다. */
void free_tokens(TokenArray *tokens) {
    // 토큰 배열 안의 각 문자열을 순회할 인덱스다.
    int index;

    // 토큰 하나마다 text 문자열을 먼저 해제한다.
    for (index = 0; index < tokens->count; index++) {
        free(tokens->items[index].text);
    }

    // 그다음 토큰 배열 자체를 해제한다.
    free(tokens->items);
    // 안전하게 초기 상태로 되돌린다.
    tokens->items = NULL;
    tokens->count = 0;
    tokens->capacity = 0;
}
