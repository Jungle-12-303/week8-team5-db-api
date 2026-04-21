// parser는 lexer가 만든 토큰 목록을 SQL 의미 구조로 해석한다.
/*
 * sql/parser.c
 *
 * parser는 lexer가 만든 토큰 배열을 읽어 INSERT/SELECT AST 구조로 바꾼다.
 * 즉, "토큰이 어떤 SQL 문장을 의미하는가"를 해석하는 단계다.
 */
#include "sqlparser/sql/parser.h"

// 에러 메시지 생성을 위해 포함한다.
#include <stdio.h>
// free 함수를 쓰기 위해 포함한다.
#include <stdlib.h>
#include <string.h>

// 현재 어느 토큰을 보고 있는지 관리하기 위한 내부 상태 구조체다.
typedef struct {
    // 전체 토큰 배열을 가리킨다.
    const TokenArray *tokens;
    // 지금 읽고 있는 토큰 위치다.
    int index;
    // 에러 메시지를 채워 넣을 결과 구조체 주소다.
    ParseResult *result;
} ParserState;

static const Token *current_token(const ParserState *state);

/* TokenType enum 값을 에러 메시지용 문자열로 변환한다. */
static const char *token_type_name(TokenType type) {
    switch (type) {
        case TOKEN_IDENTIFIER:
            return "IDENTIFIER";
        case TOKEN_STRING:
            return "STRING";
        case TOKEN_NUMBER:
            return "NUMBER";
        case TOKEN_STAR:
            return "STAR";
        case TOKEN_EQUALS:
            return "EQUALS";
        case TOKEN_COMMA:
            return "COMMA";
        case TOKEN_LPAREN:
            return "LPAREN";
        case TOKEN_RPAREN:
            return "RPAREN";
        case TOKEN_SEMICOLON:
            return "SEMICOLON";
        case TOKEN_END:
            return "EOF";
    }

    return "UNKNOWN";
}

/* 현재 토큰을 사람이 읽기 쉬운 설명 문자열로 만든다. */
static void describe_token(const Token *token, char *buffer, size_t buffer_size) {
    if (token->type == TOKEN_END) {
        snprintf(buffer, buffer_size, "EOF at position %d", token->position);
        return;
    }

    if (token->text != NULL && token->text[0] != '\0') {
        snprintf(buffer, buffer_size, "%s(\"%s\") at position %d", token_type_name(token->type), token->text, token->position);
        return;
    }

    snprintf(buffer, buffer_size, "%s at position %d", token_type_name(token->type), token->position);
}

/* 기대한 토큰과 실제 토큰 차이를 자세한 파서 오류 메시지로 만든다. */
static void set_expected_token_error(ParserState *state, const char *expected) {
    char token_description[128];

    describe_token(current_token(state), token_description, sizeof(token_description));
    if (strncmp(expected, "expected ", 9) == 0 || strncmp(expected, "unexpected ", 11) == 0) {
        snprintf(state->result->message, sizeof(state->result->message), "%s, got %s", expected, token_description);
        return;
    }

    snprintf(state->result->message, sizeof(state->result->message), "expected %s, got %s", expected, token_description);
}

/* 현재 읽고 있는 토큰 하나를 반환한다. */
static const Token *current_token(const ParserState *state) {
    // 현재 위치의 토큰 주소를 바로 반환한다.
    return &state->tokens->items[state->index];
}

/* 현재 토큰 타입이 기대값과 같으면 소비하고, 아니면 그대로 둔다. */
static int match_type(ParserState *state, TokenType type) {
    // 지금 보고 있는 토큰 종류가 기대한 종류와 같으면 소비한다.
    if (current_token(state)->type == type) {
        state->index++;
        return 1;
    }

    // 다르면 아무것도 소비하지 않고 실패를 돌려준다.
    return 0;
}

/* 특정 타입 토큰이 반드시 와야 하는 위치에서 사용하는 검증 함수다. */
static int expect_type(ParserState *state, TokenType type, const char *message) {
    // 기대한 토큰이 맞으면 통과시킨다.
    if (!match_type(state, type)) {
        // 다르면 결과 구조체에 사람이 읽을 메시지를 남긴다.
        set_expected_token_error(state, message);
        return 0;
    }

    return 1;
}

/* 현재 토큰이 INSERT, SELECT, FROM 같은 특정 키워드인지 확인한다. */
static int expect_keyword(ParserState *state, const char *keyword) {
    // 현재 토큰을 잠시 읽기 쉽게 꺼내 둔다.
    const Token *token = current_token(state);

    // 식별자 토큰이 아니거나, 기대한 키워드와 다르면 실패다.
    if (token->type != TOKEN_IDENTIFIER || !strings_equal_ignore_case(token->text, keyword)) {
        char expected[64];
        snprintf(expected, sizeof(expected), "keyword %s", keyword);
        set_expected_token_error(state, expected);
        return 0;
    }

    // 키워드를 정상 소비했으므로 다음 토큰으로 이동한다.
    state->index++;
    return 1;
}

/* 테이블명 또는 컬럼명처럼 식별자 하나를 읽는다. */
static int parse_identifier(ParserState *state, char **value) {
    // 현재 토큰을 읽는다.
    const Token *token = current_token(state);

    // 테이블명이나 컬럼명은 반드시 식별자여야 한다.
    if (token->type != TOKEN_IDENTIFIER) {
        set_expected_token_error(state, "identifier");
        return 0;
    }

    // 토큰 문자열을 별도 메모리로 복사해 결과 구조체에 넘긴다.
    *value = copy_string(token->text);
    if (*value == NULL) {
        snprintf(state->result->message, sizeof(state->result->message), "out of memory while reading identifier");
        return 0;
    }

    // 식별자 하나를 읽었으니 다음 토큰으로 이동한다.
    state->index++;
    return 1;
}

/* 쉼표로 구분된 식별자 목록을 읽어 StringList에 담는다. */
static int parse_identifier_list(ParserState *state, StringList *list) {
    // 식별자 하나를 임시로 담을 포인터다.
    char *identifier = NULL;

    // 첫 번째 식별자를 읽는다.
    if (!parse_identifier(state, &identifier)) {
        return 0;
    }

    // 리스트에 복사해서 넣는다.
    if (!string_list_push(list, identifier)) {
        free(identifier);
        snprintf(state->result->message, sizeof(state->result->message), "out of memory while storing identifiers");
        return 0;
    }
    // 리스트가 이미 복사했으므로 임시 포인터는 해제한다.
    free(identifier);

    // 쉼표가 이어지면 식별자가 더 있다는 뜻이다.
    while (match_type(state, TOKEN_COMMA)) {
        if (!parse_identifier(state, &identifier)) {
            return 0;
        }

        if (!string_list_push(list, identifier)) {
            free(identifier);
            snprintf(state->result->message, sizeof(state->result->message), "out of memory while storing identifiers");
            return 0;
        }
        free(identifier);
    }

    // 더 이상 쉼표가 없으면 식별자 목록 파싱이 끝난다.
    return 1;
}

/* VALUES (...) 안에 들어가는 값 목록을 순서대로 읽는다. */
static int parse_value_list(ParserState *state, StringList *list) {
    // 현재 값을 가리키는 토큰 포인터다.
    const Token *token;

    // 첫 번째 값을 읽는다.
    token = current_token(state);
    // 현재 구현에서는 문자열, 숫자, 식별자만 값으로 허용한다.
    if (token->type != TOKEN_STRING && token->type != TOKEN_NUMBER && token->type != TOKEN_IDENTIFIER) {
        set_expected_token_error(state, "SQL value");
        return 0;
    }

    // 값 문자열을 리스트에 복사해서 저장한다.
    if (!string_list_push(list, token->text)) {
        snprintf(state->result->message, sizeof(state->result->message), "out of memory while storing values");
        return 0;
    }
    // 값 하나를 소비했으니 다음 토큰으로 이동한다.
    state->index++;

    // 쉼표가 이어지면 값이 더 있는 것이다.
    while (match_type(state, TOKEN_COMMA)) {
        token = current_token(state);
        if (token->type != TOKEN_STRING && token->type != TOKEN_NUMBER && token->type != TOKEN_IDENTIFIER) {
            set_expected_token_error(state, "SQL value");
            return 0;
        }

        if (!string_list_push(list, token->text)) {
            snprintf(state->result->message, sizeof(state->result->message), "out of memory while storing values");
            return 0;
        }
        state->index++;
    }

    return 1;
}

/* WHERE 절 오른쪽에 오는 비교값 하나를 읽는다. */
static int parse_condition_value(ParserState *state, char **value) {
    const Token *token = current_token(state);

    if (token->type != TOKEN_STRING && token->type != TOKEN_NUMBER && token->type != TOKEN_IDENTIFIER) {
        set_expected_token_error(state, "SQL value in WHERE clause");
        return 0;
    }

    *value = copy_string(token->text);
    if (*value == NULL) {
        snprintf(state->result->message, sizeof(state->result->message), "out of memory while reading WHERE value");
        return 0;
    }

    state->index++;
    return 1;
}

/* 현재 과제 범위에서는 WHERE 뒤에 조건을 하나만 둘 수 있다. */
static int reject_unsupported_where_connector(ParserState *state) {
    const Token *token = current_token(state);

    if (token->type != TOKEN_IDENTIFIER) {
        return 1;
    }

    if (!strings_equal_ignore_case(token->text, "AND") &&
        !strings_equal_ignore_case(token->text, "OR")) {
        return 1;
    }

    snprintf(state->result->message,
             sizeof(state->result->message),
             "AND/OR conditions are not supported; only a single WHERE condition is allowed, got IDENTIFIER(\"%s\") at position %d",
             token->text,
             token->position);
    return 0;
}
/* INSERT 문장을 AST의 InsertStatement 형태로 채운다. */
static int parse_insert(ParserState *state, Statement *statement) {
    // union 안의 INSERT 전용 영역을 읽기 쉽게 별칭으로 잡는다.
    InsertStatement *insert_statement = &statement->as.insert_statement;

    // INSERT INTO table (...) VALUES (...) 순서를 그대로 따라간다.
    if (!expect_keyword(state, "INSERT")) {
        return 0;
    }

    if (!expect_keyword(state, "INTO")) {
        return 0;
    }

    if (!parse_identifier(state, &insert_statement->table_name)) {
        return 0;
    }

    if (!expect_type(state, TOKEN_LPAREN, "expected '(' after table name")) {
        return 0;
    }

    if (!parse_identifier_list(state, &insert_statement->columns)) {
        return 0;
    }

    if (!expect_type(state, TOKEN_RPAREN, "expected ')' after column list")) {
        return 0;
    }

    if (!expect_keyword(state, "VALUES")) {
        return 0;
    }

    if (!expect_type(state, TOKEN_LPAREN, "expected '(' after VALUES")) {
        return 0;
    }

    if (!parse_value_list(state, &insert_statement->values)) {
        return 0;
    }

    if (!expect_type(state, TOKEN_RPAREN, "expected ')' after value list")) {
        return 0;
    }

    // 여기까지 왔으면 INSERT 문장은 정상 해석된 것이다.
    return 1;
}

/* SELECT 문장을 AST의 SelectStatement 형태로 채운다. */
static int parse_select(ParserState *state, Statement *statement) {
    // union 안의 SELECT 전용 영역을 읽기 쉽게 별칭으로 잡는다.
    SelectStatement *select_statement = &statement->as.select_statement;

    // SELECT 키워드를 먼저 소비한다.
    if (!expect_keyword(state, "SELECT")) {
        return 0;
    }

    // '*' 이면 전체 컬럼 조회다.
    if (match_type(state, TOKEN_STAR)) {
        select_statement->select_all = 1;
    // '*'가 아니면 컬럼 목록을 읽는다.
    } else if (!parse_identifier_list(state, &select_statement->columns)) {
        return 0;
    }

    if (!expect_keyword(state, "FROM")) {
        return 0;
    }

    if (!parse_identifier(state, &select_statement->table_name)) {
        return 0;
    }

    if (current_token(state)->type == TOKEN_IDENTIFIER && strings_equal_ignore_case(current_token(state)->text, "WHERE")) {
        state->index++;

        if (!parse_identifier(state, &select_statement->where_column)) {
            return 0;
        }

        if (!expect_type(state, TOKEN_EQUALS, "expected '=' in WHERE clause")) {
            return 0;
        }

        if (!parse_condition_value(state, &select_statement->where_value)) {
            return 0;
        }

        if (!reject_unsupported_where_connector(state)) {
            return 0;
        }

        select_statement->has_where = 1;
    }

    // SELECT ... FROM table 형태를 모두 읽었으니 성공이다.
    return 1;
}

/* 토큰 배열 하나를 문장 하나로 완성하는 parser의 진입점이다. */
ParseResult parse_statement(const TokenArray *tokens) {
    // 반환할 결과 구조체다. {0}으로 초기화해 ok=0, 포인터=NULL 상태로 시작한다.
    ParseResult result = {0};
    // 현재 파싱 위치를 관리할 상태 구조체다.
    ParserState state;

    // 토큰 배열과 시작 위치를 세팅한다.
    state.tokens = tokens;
    state.index = 0;
    state.result = &result;

    // 첫 토큰이 INSERT면 INSERT 문장으로 해석한다.
    if (current_token(&state)->type == TOKEN_IDENTIFIER &&
        strings_equal_ignore_case(current_token(&state)->text, "INSERT")) {
        result.statement.type = STATEMENT_INSERT;
        if (!parse_insert(&state, &result.statement)) {
            // 중간에 실패했더라도 일부 메모리가 잡혔을 수 있으니 정리한다.
            free_statement(&result.statement);
            return result;
        }
    // 첫 토큰이 SELECT면 SELECT 문장으로 해석한다.
    } else if (current_token(&state)->type == TOKEN_IDENTIFIER &&
               strings_equal_ignore_case(current_token(&state)->text, "SELECT")) {
        result.statement.type = STATEMENT_SELECT;
        if (!parse_select(&state, &result.statement)) {
            free_statement(&result.statement);
            return result;
        }
    } else {
        // 현재 버전은 INSERT와 SELECT만 허용한다.
        {
            char token_description[128];
            describe_token(current_token(&state), token_description, sizeof(token_description));
            snprintf(result.message, sizeof(result.message), "only INSERT and SELECT are supported, got %s", token_description);
        }
        return result;
    }

    // 문장 끝에는 반드시 세미콜론이 있어야 한다.
    if (!expect_type(&state, TOKEN_SEMICOLON, "expected ';' at end of SQL statement")) {
        free_statement(&result.statement);
        return result;
    }

    // 세미콜론 뒤에는 더 이상 다른 토큰이 없어야 한다.
    if (!expect_type(&state, TOKEN_END, "unexpected tokens after SQL statement")) {
        free_statement(&result.statement);
        return result;
    }

    // 여기까지 오면 파싱 성공이다.
    result.ok = 1;
    return result;
}
