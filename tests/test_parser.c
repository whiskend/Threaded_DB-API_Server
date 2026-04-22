#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"

/* parser 테스트에서 기대하는 상태 코드 집합이다. */
enum {
    /* 토큰화와 파싱이 정상 완료됐음을 뜻한다. */
    STATUS_OK = 0,
    /* 파싱 전에 lexer 단계에서 실패했음을 뜻한다. */
    STATUS_LEX_ERROR = 3,
    /* 토큰은 만들었지만 SQL 문법 규칙에 맞지 않아 파싱이 실패했음을 뜻한다. */
    STATUS_PARSE_ERROR = 4
};

/* message를 stderr에 출력하고 테스트를 종료한다. */
static void fail(const char *message) {
    fprintf(stderr, "test_parser failed: %s\n", message);
    exit(1);
}

/* 정수 expected와 actual이 같은지 검사하고 다르면 실패시킨다. */
static void assert_int_eq(int expected, int actual, const char *message) {
    if (expected != actual) {
        fprintf(stderr, "test_parser failed: %s (expected=%d actual=%d)\n", message, expected, actual);
        exit(1);
    }
}

/* size_t expected와 actual이 같은지 검사해 파싱 결과 개수를 검증한다. */
static void assert_size_eq(size_t expected, size_t actual, const char *message) {
    if (expected != actual) {
        fprintf(stderr, "test_parser failed: %s (expected=%zu actual=%zu)\n", message, expected, actual);
        exit(1);
    }
}

/* 문자열 expected와 actual이 같은지 검사해 AST 내부 텍스트를 검증한다. */
static void assert_str_eq(const char *expected, const char *actual, const char *message) {
    if (strcmp(expected, actual) != 0) {
        fprintf(stderr, "test_parser failed: %s (expected=%s actual=%s)\n", message, expected, actual);
        exit(1);
    }
}

/* sql을 먼저 tokenize한 뒤 parse_statement까지 수행해 단일 statement AST를 만든다. */
static int tokenize_and_parse(const char *sql, Statement *stmt, char *errbuf, size_t errbuf_size) {
    TokenArray tokens = {0};
    int status = tokenize_sql(sql, &tokens, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    status = parse_statement(&tokens, stmt, errbuf, errbuf_size);
    free_token_array(&tokens);
    return status;
}

/* column list 없는 INSERT가 올바른 AST와 literal 타입으로 파싱되는지 검증한다. */
static void test_insert_without_column_list(void) {
    Statement stmt;
    char errbuf[256] = {0};

    assert_int_eq(STATUS_OK,
                  tokenize_and_parse("INSERT INTO users VALUES (1, 'Alice', 20);", &stmt, errbuf, sizeof(errbuf)),
                  "INSERT without column list should parse");
    assert_int_eq(STMT_INSERT, stmt.type, "statement type should be INSERT");
    assert_str_eq("users", stmt.insert_stmt.table_name, "table name should match");
    assert_size_eq(0, stmt.insert_stmt.column_count, "column list should be omitted");
    assert_size_eq(3, stmt.insert_stmt.value_count, "value count should match");
    assert_int_eq(VALUE_NUMBER, stmt.insert_stmt.values[0].type, "first value should be number");
    assert_str_eq("1", stmt.insert_stmt.values[0].text, "first number text should match");
    assert_int_eq(VALUE_STRING, stmt.insert_stmt.values[1].type, "second value should be string");
    assert_str_eq("Alice", stmt.insert_stmt.values[1].text, "string text should match");
    free_statement(&stmt);
}

/* projection 컬럼과 WHERE가 있는 SELECT가 기대한 AST 구조로 파싱되는지 검증한다. */
static void test_select_column_list_with_where(void) {
    Statement stmt;
    char errbuf[256] = {0};

    assert_int_eq(STATUS_OK,
                  tokenize_and_parse("SELECT id, name FROM users WHERE id = 2;", &stmt, errbuf, sizeof(errbuf)),
                  "SELECT with WHERE should parse");
    assert_int_eq(STMT_SELECT, stmt.type, "statement type should be SELECT");
    assert_int_eq(0, stmt.select_stmt.select_all, "projection should not be select_all");
    assert_size_eq(2, stmt.select_stmt.column_count, "projection column count should match");
    assert_str_eq("users", stmt.select_stmt.table_name, "table name should match");
    assert_int_eq(1, stmt.select_stmt.where_clause.has_condition, "WHERE clause should be enabled");
    assert_str_eq("id", stmt.select_stmt.where_clause.column_name, "WHERE column should match");
    assert_str_eq("2", stmt.select_stmt.where_clause.value.text, "WHERE literal should match");
    free_statement(&stmt);
}

/* 하나의 토큰 스트림에서 parse_next_statement가 여러 SQL을 순서대로 분리하는지 확인한다. */
static void test_parse_next_statement_handles_multiple_sqls(void) {
    TokenArray tokens = {0};
    Statement stmt = {0};
    size_t cursor = 0;
    char errbuf[256] = {0};

    assert_int_eq(STATUS_OK,
                  tokenize_sql("INSERT INTO users VALUES (1, 'Alice', 20); SELECT name FROM users WHERE id = 1;",
                               &tokens, errbuf, sizeof(errbuf)),
                  "multiple statements should tokenize");

    assert_int_eq(STATUS_OK,
                  parse_next_statement(&tokens, &cursor, &stmt, errbuf, sizeof(errbuf)),
                  "first statement should parse");
    assert_int_eq(STMT_INSERT, stmt.type, "first statement should be INSERT");
    free_statement(&stmt);

    assert_int_eq(STATUS_OK,
                  parse_next_statement(&tokens, &cursor, &stmt, errbuf, sizeof(errbuf)),
                  "second statement should parse");
    assert_int_eq(STMT_SELECT, stmt.type, "second statement should be SELECT");
    assert_str_eq("name", stmt.select_stmt.columns[0], "projection should match");
    assert_int_eq(1, stmt.select_stmt.where_clause.has_condition, "WHERE clause should remain available");
    free_statement(&stmt);

    while (cursor < tokens.count && tokens.items[cursor].type == TOKEN_SEMICOLON) {
        ++cursor;
    }
    assert_int_eq(TOKEN_EOF, tokens.items[cursor].type, "cursor should end at EOF after both statements");
    free_token_array(&tokens);
}

/* projection 없는 SELECT가 PARSE ERROR로 거절되는지 검증한다. */
static void test_select_missing_projection_fails(void) {
    Statement stmt;
    char errbuf[256] = {0};

    assert_int_eq(STATUS_PARSE_ERROR,
                  tokenize_and_parse("SELECT FROM users;", &stmt, errbuf, sizeof(errbuf)),
                  "missing projection should fail");
    if (strstr(errbuf, "PARSE ERROR") == NULL) {
        fail("parse error message should be present");
    }
}

/* INSERT 문에서 INTO가 빠졌을 때 적절한 parse error가 나는지 확인한다. */
static void test_insert_missing_into_fails(void) {
    Statement stmt;
    char errbuf[256] = {0};

    assert_int_eq(STATUS_PARSE_ERROR,
                  tokenize_and_parse("INSERT users VALUES (1);", &stmt, errbuf, sizeof(errbuf)),
                  "missing INTO should fail");
    if (strstr(errbuf, "expected INTO") == NULL) {
        fail("missing INTO message should be present");
    }
}

/* SELECT 문에서 FROM이 빠졌을 때 parser가 오류를 보고하는지 검증한다. */
static void test_missing_from_fails(void) {
    Statement stmt;
    char errbuf[256] = {0};

    assert_int_eq(STATUS_PARSE_ERROR,
                  tokenize_and_parse("SELECT id, name users;", &stmt, errbuf, sizeof(errbuf)),
                  "missing FROM should fail");
    if (strstr(errbuf, "expected FROM") == NULL) {
        fail("missing FROM message should be present");
    }
}

/* WHERE 절에서 '=' 가 빠졌을 때 parser가 문법 오류를 내는지 확인한다. */
static void test_missing_equal_in_where_fails(void) {
    Statement stmt;
    char errbuf[256] = {0};

    assert_int_eq(STATUS_PARSE_ERROR,
                  tokenize_and_parse("SELECT * FROM users WHERE id 1;", &stmt, errbuf, sizeof(errbuf)),
                  "missing '=' in WHERE should fail");
    if (strstr(errbuf, "expected '='") == NULL) {
        fail("missing '=' message should be present");
    }
}

/* INSERT의 column list 구두점 오류가 모두 PARSE ERROR로 잡히는지 검증한다. */
static void test_missing_comma_or_paren_fails(void) {
    Statement stmt;
    char errbuf[256] = {0};

    assert_int_eq(STATUS_PARSE_ERROR,
                  tokenize_and_parse("INSERT INTO users (id name) VALUES (1, 'Alice');", &stmt, errbuf, sizeof(errbuf)),
                  "missing comma in column list should fail");
    assert_int_eq(STATUS_PARSE_ERROR,
                  tokenize_and_parse("INSERT INTO users (id, name VALUES (1, 'Alice');", &stmt, errbuf, sizeof(errbuf)),
                  "missing right paren should fail");
}

/* parser 단위 테스트를 모두 실행하고 통과 시 OK를 출력한다. */
int main(void) {
    test_insert_without_column_list();
    test_select_column_list_with_where();
    test_parse_next_statement_handles_multiple_sqls();
    test_select_missing_projection_fails();
    test_insert_missing_into_fails();
    test_missing_from_fails();
    test_missing_equal_in_where_fails();
    test_missing_comma_or_paren_fails();
    printf("test_parser: OK\n");
    return 0;
}
