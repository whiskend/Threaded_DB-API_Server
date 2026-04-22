#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"

/* lexer 테스트에서 기대하는 성공 상태 코드다. */
enum {
    /* 토큰화가 정상 완료됐음을 뜻한다. */
    STATUS_OK = 0,
    /* 잘못된 문자나 리터럴 형식으로 토큰화가 실패했음을 뜻한다. */
    STATUS_LEX_ERROR = 3
};

/* message를 출력하고 테스트를 즉시 종료한다. */
static void fail(const char *message) {
    fprintf(stderr, "test_lexer failed: %s\n", message);
    exit(1);
}

/* expected와 actual 정수가 같은지 검사하고 다르면 message와 함께 종료한다. */
static void assert_int_eq(int expected, int actual, const char *message) {
    if (expected != actual) {
        fprintf(stderr, "test_lexer failed: %s (expected=%d actual=%d)\n", message, expected, actual);
        exit(1);
    }
}

/* expected와 actual 문자열이 같은지 검사하고 다르면 상세 메시지와 함께 종료한다. */
static void assert_str_eq(const char *expected, const char *actual, const char *message) {
    if (strcmp(expected, actual) != 0) {
        fprintf(stderr, "test_lexer failed: %s (expected=%s actual=%s)\n", message, expected, actual);
        exit(1);
    }
}

/* INSERT 문이 키워드, 식별자, 숫자, 문자열, EOF까지 올바르게 분해되는지 검증한다. */
static void test_insert_tokens(void) {
    TokenArray tokens = {0};
    char errbuf[256] = {0};

    assert_int_eq(STATUS_OK,
                  tokenize_sql("INSERT INTO users VALUES (1, 'Alice');", &tokens, errbuf, sizeof(errbuf)),
                  "INSERT tokenization should succeed");
    assert_int_eq(TOKEN_INSERT, tokens.items[0].type, "first token should be INSERT");
    assert_int_eq(TOKEN_INTO, tokens.items[1].type, "second token should be INTO");
    assert_int_eq(TOKEN_IDENTIFIER, tokens.items[2].type, "table name should be identifier");
    assert_str_eq("users", tokens.items[2].text, "table name should match");
    assert_int_eq(TOKEN_VALUES, tokens.items[3].type, "VALUES token should exist");
    assert_int_eq(TOKEN_NUMBER, tokens.items[5].type, "numeric literal should be number");
    assert_str_eq("1", tokens.items[5].text, "numeric literal text should match");
    assert_int_eq(TOKEN_STRING, tokens.items[7].type, "string literal should be string token");
    assert_str_eq("Alice", tokens.items[7].text, "string literal should be unquoted");
    assert_int_eq(TOKEN_EOF, tokens.items[tokens.count - 1].type, "EOF token should be appended");

    free_token_array(&tokens);
}

/* SELECT * FROM 구문이 핵심 토큰으로 정확히 나뉘는지 검증한다. */
static void test_select_star_tokens(void) {
    TokenArray tokens = {0};
    char errbuf[256] = {0};

    assert_int_eq(STATUS_OK,
                  tokenize_sql("SELECT * FROM users;", &tokens, errbuf, sizeof(errbuf)),
                  "SELECT * should tokenize");
    assert_int_eq(TOKEN_SELECT, tokens.items[0].type, "SELECT keyword should match");
    assert_int_eq(TOKEN_STAR, tokens.items[1].type, "* token should match");
    assert_int_eq(TOKEN_FROM, tokens.items[2].type, "FROM keyword should match");
    assert_str_eq("users", tokens.items[3].text, "table name should match");

    free_token_array(&tokens);
}

/* projection 컬럼 목록과 키워드의 대소문자 무시 동작을 검증한다. */
static void test_select_column_tokens(void) {
    TokenArray tokens = {0};
    char errbuf[256] = {0};

    assert_int_eq(STATUS_OK,
                  tokenize_sql("select id, name FROM users;", &tokens, errbuf, sizeof(errbuf)),
                  "column projection should tokenize");
    assert_int_eq(TOKEN_SELECT, tokens.items[0].type, "keyword should be case-insensitive");
    assert_int_eq(TOKEN_IDENTIFIER, tokens.items[1].type, "first column should be identifier");
    assert_str_eq("id", tokens.items[1].text, "first column text should match");
    assert_int_eq(TOKEN_COMMA, tokens.items[2].type, "comma token should exist");
    assert_str_eq("name", tokens.items[3].text, "second column text should match");

    free_token_array(&tokens);
}

/* escaped string, 음수, 소수 리터럴이 원문 의미대로 토큰화되는지 검증한다. */
static void test_string_and_number_literals(void) {
    TokenArray tokens = {0};
    char errbuf[256] = {0};

    assert_int_eq(STATUS_OK,
                  tokenize_sql("INSERT INTO books VALUES ('O''Reilly', -7, 3.14);", &tokens, errbuf, sizeof(errbuf)),
                  "escaped string and numbers should tokenize");
    assert_str_eq("O'Reilly", tokens.items[5].text, "escaped string should be unescaped");
    assert_str_eq("-7", tokens.items[7].text, "negative integer should match");
    assert_str_eq("3.14", tokens.items[9].text, "decimal literal should match");

    free_token_array(&tokens);
}

/* 허용되지 않은 문자가 들어오면 LEX ERROR로 실패하는지 검증한다. */
static void test_invalid_character_fails(void) {
    TokenArray tokens = {0};
    char errbuf[256] = {0};

    assert_int_eq(STATUS_LEX_ERROR,
                  tokenize_sql("SELECT @ FROM users;", &tokens, errbuf, sizeof(errbuf)),
                  "invalid character should fail");
    if (strstr(errbuf, "LEX ERROR") == NULL) {
        fail("error message should mention LEX ERROR");
    }
}

/* 닫히지 않은 문자열 리터럴이 명확한 오류 메시지와 함께 거절되는지 확인한다. */
static void test_unterminated_string_fails(void) {
    TokenArray tokens = {0};
    char errbuf[256] = {0};

    assert_int_eq(STATUS_LEX_ERROR,
                  tokenize_sql("INSERT INTO users VALUES ('Alice);", &tokens, errbuf, sizeof(errbuf)),
                  "unterminated string should fail");
    if (strstr(errbuf, "unterminated string literal") == NULL) {
        fail("unterminated string message should be present");
    }
}

/* lexer 단위 테스트를 모두 실행하고 통과 시 OK를 출력한다. */
int main(void) {
    test_insert_tokens();
    test_select_star_tokens();
    test_select_column_tokens();
    test_string_and_number_literals();
    test_invalid_character_fails();
    test_unterminated_string_fails();
    printf("test_lexer: OK\n");
    return 0;
}
