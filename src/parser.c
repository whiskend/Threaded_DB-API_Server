#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

/* parser.c 내부에서만 쓰는 상태 코드 별칭이다. */
enum {
    STATUS_OK = 0,          /* 파싱이 정상적으로 끝났음을 뜻한다. */
    STATUS_PARSE_ERROR = 4  /* SQL 문법 해석 중 오류가 났음을 뜻한다. */
};

/* 현재 파싱 중인 토큰 배열과 cursor 위치를 묶어 다루는 내부 상태 구조체다. */
typedef struct {
    const TokenArray *tokens; /* 파서가 읽고 있는 전체 토큰 배열이다. */
    size_t current;           /* 다음에 읽을 토큰 인덱스다. */
} Parser;

/* 단일 message를 errbuf에 복사해 parser 오류 문자열을 기록한다. */
static void set_error(char *errbuf, size_t errbuf_size, const char *message) {
    if (errbuf != NULL && errbuf_size > 0) {
        snprintf(errbuf, errbuf_size, "%s", message);
    }
}

/* char* 배열 items/count가 소유한 문자열과 배열 메모리를 모두 해제한다. */
static void free_string_list(char **items, size_t count) {
    size_t i;

    if (items == NULL) {
        return;
    }

    for (i = 0; i < count; ++i) {
        free(items[i]);
    }
    free(items);
}

/* LiteralValue 배열 items/count가 소유한 text와 배열 메모리를 모두 해제한다. */
static void free_literal_list(LiteralValue *items, size_t count) {
    size_t i;

    if (items == NULL) {
        return;
    }

    for (i = 0; i < count; ++i) {
        free(items[i].text);
    }
    free(items);
}

/* parser.current가 가리키는 현재 토큰 포인터를 반환한다. */
static const Token *parser_peek(Parser *parser) {
    return &parser->tokens->items[parser->current];
}

/* 방금 소비한 이전 토큰 포인터를 반환한다. */
static const Token *parser_previous(Parser *parser) {
    return &parser->tokens->items[parser->current - 1];
}

/* EOF가 아니면 cursor를 한 칸 전진시키고, 소비된 토큰 포인터를 반환한다. */
static const Token *parser_advance(Parser *parser) {
    if (parser_peek(parser)->type != TOKEN_EOF) {
        ++parser->current;
    }
    return parser_previous(parser);
}

/* 현재 토큰 타입이 type이면 소비하고 1, 아니면 그대로 두고 0을 반환한다. */
static int parser_match(Parser *parser, TokenType type) {
    if (parser_peek(parser)->type != type) {
        return 0;
    }
    parser_advance(parser);
    return 1;
}

/* 문장 사이에 있는 연속 세미콜론을 모두 건너뛰어 empty statement를 무시한다. */
static void parser_skip_semicolons(Parser *parser) {
    while (parser_match(parser, TOKEN_SEMICOLON)) {
        /* empty statement separators are ignored */
    }
}

/* 현재 토큰이 기대한 type이 아니면 message를 기록하고 parse error를 반환한다. */
static int parser_expect_with_message(Parser *parser, TokenType type, const char *message, char *errbuf, size_t errbuf_size) {
    if (parser_match(parser, type)) {
        return STATUS_OK;
    }

    set_error(errbuf, errbuf_size, message);
    return STATUS_PARSE_ERROR;
}

/* 식별자 text를 out_items 동적 배열 뒤에 복제해 붙이고 성공 시 1을 반환한다. */
static int append_identifier(char ***out_items, size_t *out_count, const char *text) {
    char **grown = (char **)realloc(*out_items, sizeof(char *) * (*out_count + 1));
    if (grown == NULL) {
        return 0;
    }

    *out_items = grown;
    (*out_items)[*out_count] = strdup_safe(text);
    if ((*out_items)[*out_count] == NULL) {
        return 0;
    }
    ++(*out_count);
    return 1;
}

/* 리터럴 type/text를 out_items 동적 배열 뒤에 복제해 붙이고 성공 시 1을 반환한다. */
static int append_literal(LiteralValue **out_items, size_t *out_count, ValueType type, const char *text) {
    LiteralValue *grown = (LiteralValue *)realloc(*out_items, sizeof(LiteralValue) * (*out_count + 1));
    if (grown == NULL) {
        return 0;
    }

    *out_items = grown;
    (*out_items)[*out_count].type = type;
    (*out_items)[*out_count].text = strdup_safe(text);
    if ((*out_items)[*out_count].text == NULL) {
        return 0;
    }
    ++(*out_count);
    return 1;
}

/* 현재 토큰이 식별자면 out_text에 복제하고, 아니면 parse error를 반환한다. */
static int parse_identifier(Parser *parser, char **out_text, char *errbuf, size_t errbuf_size) {
    if (parser_peek(parser)->type != TOKEN_IDENTIFIER) {
        set_error(errbuf, errbuf_size, "PARSE ERROR: expected identifier");
        return STATUS_PARSE_ERROR;
    }

    *out_text = strdup_safe(parser_advance(parser)->text);
    if (*out_text == NULL) {
        set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
        return STATUS_PARSE_ERROR;
    }
    return STATUS_OK;
}

/* 현재 토큰이 문자열/숫자 리터럴이면 out_value에 복제하고 상태를 반환한다. */
static int parse_literal(Parser *parser, LiteralValue *out_value, char *errbuf, size_t errbuf_size) {
    const Token *token = parser_peek(parser);

    if (token->type == TOKEN_STRING) {
        out_value->type = VALUE_STRING;
        out_value->text = strdup_safe(token->text);
        if (out_value->text == NULL) {
            set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
            return STATUS_PARSE_ERROR;
        }
        parser_advance(parser);
        return STATUS_OK;
    }

    if (token->type == TOKEN_NUMBER) {
        out_value->type = VALUE_NUMBER;
        out_value->text = strdup_safe(token->text);
        if (out_value->text == NULL) {
            set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
            return STATUS_PARSE_ERROR;
        }
        parser_advance(parser);
        return STATUS_OK;
    }

    set_error(errbuf, errbuf_size, "PARSE ERROR: expected literal");
    return STATUS_PARSE_ERROR;
}

/* `a, b, c` 형태 식별자 목록을 읽어 out_items/out_count에 채운다. */
static int parse_identifier_list(Parser *parser, char ***out_items, size_t *out_count,
                                 char *errbuf, size_t errbuf_size) {
    char *identifier = NULL;
    int status;

    *out_items = NULL;
    *out_count = 0;

    status = parse_identifier(parser, &identifier, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    if (!append_identifier(out_items, out_count, identifier)) {
        free(identifier);
        set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
        return STATUS_PARSE_ERROR;
    }
    free(identifier);

    while (parser_match(parser, TOKEN_COMMA)) {
        status = parse_identifier(parser, &identifier, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            free_string_list(*out_items, *out_count);
            *out_items = NULL;
            *out_count = 0;
            return status;
        }

        if (!append_identifier(out_items, out_count, identifier)) {
            free(identifier);
            free_string_list(*out_items, *out_count);
            *out_items = NULL;
            *out_count = 0;
            set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
            return STATUS_PARSE_ERROR;
        }
        free(identifier);
    }

    return STATUS_OK;
}

/* `(1, 'x')` 안쪽의 리터럴 목록을 읽어 out_items/out_count에 채운다. */
static int parse_literal_list(Parser *parser, LiteralValue **out_items, size_t *out_count,
                              char *errbuf, size_t errbuf_size) {
    LiteralValue literal;
    int status;

    *out_items = NULL;
    *out_count = 0;

    literal.type = VALUE_STRING;
    literal.text = NULL;
    status = parse_literal(parser, &literal, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    if (!append_literal(out_items, out_count, literal.type, literal.text)) {
        free(literal.text);
        set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
        return STATUS_PARSE_ERROR;
    }
    free(literal.text);

    while (parser_match(parser, TOKEN_COMMA)) {
        literal.type = VALUE_STRING;
        literal.text = NULL;
        status = parse_literal(parser, &literal, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            free_literal_list(*out_items, *out_count);
            *out_items = NULL;
            *out_count = 0;
            return status;
        }

        if (!append_literal(out_items, out_count, literal.type, literal.text)) {
            free(literal.text);
            free_literal_list(*out_items, *out_count);
            *out_items = NULL;
            *out_count = 0;
            set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
            return STATUS_PARSE_ERROR;
        }
        free(literal.text);
    }

    return STATUS_OK;
}

/* 선택적으로 붙는 `WHERE column = literal` 절을 읽어 out_clause에 채운다. */
static int parse_where_clause(Parser *parser, WhereClause *out_clause, char *errbuf, size_t errbuf_size) {
    int status;

    memset(out_clause, 0, sizeof(*out_clause));

    if (!parser_match(parser, TOKEN_WHERE)) {
        return STATUS_OK;
    }

    out_clause->has_condition = 1;

    status = parse_identifier(parser, &out_clause->column_name, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    status = parser_expect_with_message(parser, TOKEN_EQUAL,
                                        "PARSE ERROR: expected '=' in WHERE clause",
                                        errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    status = parse_literal(parser, &out_clause->value, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    return STATUS_OK;
}

/* INSERT 문 토큰 시퀀스를 읽어 out_stmt->insert_stmt를 완성한다. */
static int parse_insert(Parser *parser, Statement *out_stmt, char *errbuf, size_t errbuf_size) {
    InsertStatement *stmt = &out_stmt->insert_stmt;
    int status;

    memset(stmt, 0, sizeof(*stmt));

    status = parser_expect_with_message(parser, TOKEN_INTO, "PARSE ERROR: expected INTO after INSERT", errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    status = parse_identifier(parser, &stmt->table_name, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    if (parser_match(parser, TOKEN_LPAREN)) {
        status = parse_identifier_list(parser, &stmt->columns, &stmt->column_count, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            free_statement(out_stmt);
            return status;
        }

        status = parser_expect_with_message(parser, TOKEN_RPAREN, "PARSE ERROR: expected ')' after column list", errbuf, errbuf_size);
        if (status != STATUS_OK) {
            free_statement(out_stmt);
            return status;
        }
    }

    status = parser_expect_with_message(parser, TOKEN_VALUES, "PARSE ERROR: expected VALUES in INSERT statement", errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    status = parser_expect_with_message(parser, TOKEN_LPAREN, "PARSE ERROR: expected '(' before value list", errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    status = parse_literal_list(parser, &stmt->values, &stmt->value_count, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    status = parser_expect_with_message(parser, TOKEN_RPAREN, "PARSE ERROR: expected ')' after value list", errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    out_stmt->type = STMT_INSERT;
    return STATUS_OK;
}

/* SELECT 문 토큰 시퀀스를 읽어 projection/table/where 정보를 out_stmt에 채운다. */
static int parse_select(Parser *parser, Statement *out_stmt, char *errbuf, size_t errbuf_size) {
    SelectStatement *stmt = &out_stmt->select_stmt;
    int status;

    memset(stmt, 0, sizeof(*stmt));

    if (parser_match(parser, TOKEN_STAR)) {
        stmt->select_all = 1;
    } else {
        status = parse_identifier_list(parser, &stmt->columns, &stmt->column_count, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            return status;
        }
    }

    status = parser_expect_with_message(parser, TOKEN_FROM, "PARSE ERROR: expected FROM after select list", errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    status = parse_identifier(parser, &stmt->table_name, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    status = parse_where_clause(parser, &stmt->where_clause, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    out_stmt->type = STMT_SELECT;
    return STATUS_OK;
}

/* cursor 이후 다음 statement 하나만 파싱해 out_stmt에 채우고 cursor를 앞으로 이동시킨다. */
int parse_next_statement(const TokenArray *tokens, size_t *cursor,
                         Statement *out_stmt, char *errbuf, size_t errbuf_size) {
    Parser parser;
    int status;

    if (tokens == NULL || cursor == NULL || out_stmt == NULL || tokens->count == 0 || tokens->items == NULL) {
        set_error(errbuf, errbuf_size, "PARSE ERROR: invalid token stream");
        return STATUS_PARSE_ERROR;
    }

    if (*cursor >= tokens->count) {
        set_error(errbuf, errbuf_size, "PARSE ERROR: parser cursor out of range");
        return STATUS_PARSE_ERROR;
    }

    if (errbuf != NULL && errbuf_size > 0) {
        errbuf[0] = '\0';
    }

    memset(out_stmt, 0, sizeof(*out_stmt));
    parser.tokens = tokens;
    parser.current = *cursor;
    parser_skip_semicolons(&parser);

    if (parser_peek(&parser)->type == TOKEN_EOF) {
        set_error(errbuf, errbuf_size, "PARSE ERROR: expected INSERT or SELECT");
        *cursor = parser.current;
        return STATUS_PARSE_ERROR;
    }

    if (parser_match(&parser, TOKEN_INSERT)) {
        out_stmt->type = STMT_INSERT;
        status = parse_insert(&parser, out_stmt, errbuf, errbuf_size);
    } else if (parser_match(&parser, TOKEN_SELECT)) {
        out_stmt->type = STMT_SELECT;
        status = parse_select(&parser, out_stmt, errbuf, errbuf_size);
    } else {
        set_error(errbuf, errbuf_size, "PARSE ERROR: expected INSERT or SELECT");
        return STATUS_PARSE_ERROR;
    }

    if (status != STATUS_OK) {
        return status;
    }

    if (parser_match(&parser, TOKEN_SEMICOLON)) {
        /* statement terminator is optional */
    }

    *cursor = parser.current;
    return STATUS_OK;
}

/* tokens 전체가 정확히 하나의 문장일 때만 파싱을 허용하고 out_stmt에 AST를 채운다. */
int parse_statement(const TokenArray *tokens, Statement *out_stmt, char *errbuf, size_t errbuf_size) {
    size_t cursor = 0;
    int status;

    status = parse_next_statement(tokens, &cursor, out_stmt, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    while (cursor < tokens->count && tokens->items[cursor].type == TOKEN_SEMICOLON) {
        ++cursor;
    }

    if (cursor >= tokens->count || tokens->items[cursor].type != TOKEN_EOF) {
        free_statement(out_stmt);
        set_error(errbuf, errbuf_size, "PARSE ERROR: unexpected trailing tokens");
        return STATUS_PARSE_ERROR;
    }

    return STATUS_OK;
}

/* stmt 타입에 따라 내부 동적 메모리를 해제해 AST 하나를 정리한다. */
void free_statement(Statement *stmt) {
    if (stmt == NULL) {
        return;
    }

    if (stmt->type == STMT_INSERT) {
        free(stmt->insert_stmt.table_name);
        free_string_list(stmt->insert_stmt.columns, stmt->insert_stmt.column_count);
        free_literal_list(stmt->insert_stmt.values, stmt->insert_stmt.value_count);
        stmt->insert_stmt.table_name = NULL;
        stmt->insert_stmt.columns = NULL;
        stmt->insert_stmt.column_count = 0;
        stmt->insert_stmt.values = NULL;
        stmt->insert_stmt.value_count = 0;
    } else if (stmt->type == STMT_SELECT) {
        free(stmt->select_stmt.table_name);
        free_string_list(stmt->select_stmt.columns, stmt->select_stmt.column_count);
        free(stmt->select_stmt.where_clause.column_name);
        free(stmt->select_stmt.where_clause.value.text);
        stmt->select_stmt.table_name = NULL;
        stmt->select_stmt.columns = NULL;
        stmt->select_stmt.column_count = 0;
        stmt->select_stmt.select_all = 0;
        stmt->select_stmt.where_clause.has_condition = 0;
        stmt->select_stmt.where_clause.column_name = NULL;
        stmt->select_stmt.where_clause.value.type = VALUE_STRING;
        stmt->select_stmt.where_clause.value.text = NULL;
    }
}
