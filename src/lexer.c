#include "lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

/* lexer.c 내부에서만 쓰는 상태 코드 별칭이다. */
enum {
    STATUS_OK = 0,        /* 토큰화가 정상적으로 끝났음을 뜻한다. */
    STATUS_LEX_ERROR = 3  /* 토큰화 단계에서 오류가 났음을 뜻한다. */
};

/* 단일 message를 errbuf에 복사해 lexer 오류 문자열을 기록한다. */
static void set_error(char *errbuf, size_t errbuf_size, const char *message) {
    if (errbuf != NULL && errbuf_size > 0) {
        snprintf(errbuf, errbuf_size, "%s", message);
    }
}

/* 문자 a와 b를 대소문자 무시 기준으로 비교해 같으면 1을 반환한다. */
static int chars_equal_ignore_case(char a, char b) {
    return toupper((unsigned char)a) == toupper((unsigned char)b);
}

/* lhs와 rhs를 대소문자 무시 기준으로 끝까지 비교해 완전히 같으면 1을 반환한다. */
static int strings_equal_ignore_case(const char *lhs, const char *rhs) {
    size_t i = 0;

    while (lhs[i] != '\0' && rhs[i] != '\0') {
        if (!chars_equal_ignore_case(lhs[i], rhs[i])) {
            return 0;
        }
        ++i;
    }

    return lhs[i] == '\0' && rhs[i] == '\0';
}

/* 식별자 text가 예약어면 해당 키워드 토큰 타입을, 아니면 TOKEN_IDENTIFIER를 반환한다. */
static TokenType keyword_type(const char *text) {
    if (strings_equal_ignore_case(text, "INSERT")) {
        return TOKEN_INSERT;
    }
    if (strings_equal_ignore_case(text, "INTO")) {
        return TOKEN_INTO;
    }
    if (strings_equal_ignore_case(text, "VALUES")) {
        return TOKEN_VALUES;
    }
    if (strings_equal_ignore_case(text, "SELECT")) {
        return TOKEN_SELECT;
    }
    if (strings_equal_ignore_case(text, "FROM")) {
        return TOKEN_FROM;
    }
    if (strings_equal_ignore_case(text, "WHERE")) {
        return TOKEN_WHERE;
    }
    return TOKEN_IDENTIFIER;
}

/* type/text를 가진 새 토큰을 tokens 동적 배열 끝에 추가하고 상태 코드를 반환한다. */
static int append_token(TokenArray *tokens, TokenType type, const char *text, char *errbuf, size_t errbuf_size) {
    Token *grown;

    if (tokens->count == tokens->capacity) {
        size_t new_capacity = tokens->capacity == 0 ? 8 : tokens->capacity * 2;
        grown = (Token *)realloc(tokens->items, sizeof(Token) * new_capacity);
        if (grown == NULL) {
            set_error(errbuf, errbuf_size, "LEX ERROR: out of memory");
            return STATUS_LEX_ERROR;
        }
        tokens->items = grown;
        tokens->capacity = new_capacity;
    }

    tokens->items[tokens->count].type = type;
    tokens->items[tokens->count].text = strdup_safe(text);
    if (tokens->items[tokens->count].text == NULL) {
        set_error(errbuf, errbuf_size, "LEX ERROR: out of memory");
        return STATUS_LEX_ERROR;
    }

    tokens->count += 1;
    return STATUS_OK;
}

/* start부터 length 길이만큼 복사한 새 NUL 종료 문자열을 heap에 만들어 반환한다. */
static char *copy_range(const char *start, size_t length) {
    char *text = (char *)xmalloc(length + 1);
    memcpy(text, start, length);
    text[length] = '\0';
    return text;
}

/* sql의 현재 index부터 식별자를 읽어 keyword/identifier 토큰 하나를 추가한다. */
static int tokenize_identifier(const char *sql, size_t *index, TokenArray *tokens, char *errbuf, size_t errbuf_size) {
    size_t start = *index;
    char *text;
    TokenType type;
    int status;

    while (isalnum((unsigned char)sql[*index]) || sql[*index] == '_') {
        ++(*index);
    }

    text = copy_range(sql + start, *index - start);
    type = keyword_type(text);
    status = append_token(tokens, type, text, errbuf, errbuf_size);
    free(text);
    return status;
}

/* sql의 현재 index부터 숫자 리터럴을 읽어 TOKEN_NUMBER를 추가한다. */
static int tokenize_number(const char *sql, size_t *index, TokenArray *tokens, char *errbuf, size_t errbuf_size) {
    size_t start = *index;
    int seen_dot = 0;
    char *text;
    int status;

    if (sql[*index] == '-') {
        ++(*index);
    }

    while (isdigit((unsigned char)sql[*index]) || (!seen_dot && sql[*index] == '.')) {
        if (sql[*index] == '.') {
            seen_dot = 1;
            if (!isdigit((unsigned char)sql[*index + 1])) {
                set_error(errbuf, errbuf_size, "LEX ERROR: invalid number literal");
                return STATUS_LEX_ERROR;
            }
        }
        ++(*index);
    }

    text = copy_range(sql + start, *index - start);
    status = append_token(tokens, TOKEN_NUMBER, text, errbuf, errbuf_size);
    free(text);
    return status;
}

/* 동적 문자열 buffer 뒤에 문자 ch를 append하고 성공 시 1, 실패 시 0을 반환한다. */
static int append_string_char(char **buffer, size_t *length, size_t *capacity, char ch) {
    char *grown;

    if (*length + 1 >= *capacity) {
        size_t new_capacity = *capacity == 0 ? 16 : *capacity * 2;
        grown = (char *)realloc(*buffer, new_capacity);
        if (grown == NULL) {
            return 0;
        }
        *buffer = grown;
        *capacity = new_capacity;
    }

    (*buffer)[(*length)++] = ch;
    (*buffer)[*length] = '\0';
    return 1;
}

/* 작은따옴표 문자열을 읽어 escape를 해석한 TOKEN_STRING 하나를 추가한다. */
static int tokenize_string(const char *sql, size_t *index, TokenArray *tokens, char *errbuf, size_t errbuf_size) {
    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;
    int status;

    ++(*index);

    while (sql[*index] != '\0') {
        char ch = sql[*index];

        if (ch == '\'') {
            if (sql[*index + 1] == '\'') {
                if (!append_string_char(&buffer, &length, &capacity, '\'')) {
                    free(buffer);
                    set_error(errbuf, errbuf_size, "LEX ERROR: out of memory");
                    return STATUS_LEX_ERROR;
                }
                *index += 2;
                continue;
            }

            ++(*index);
            status = append_token(tokens, TOKEN_STRING, buffer == NULL ? "" : buffer, errbuf, errbuf_size);
            free(buffer);
            return status;
        }

        if (!append_string_char(&buffer, &length, &capacity, ch)) {
            free(buffer);
            set_error(errbuf, errbuf_size, "LEX ERROR: out of memory");
            return STATUS_LEX_ERROR;
        }
        ++(*index);
    }

    free(buffer);
    set_error(errbuf, errbuf_size, "LEX ERROR: unterminated string literal");
    return STATUS_LEX_ERROR;
}

/* 단일 기호 문자 ch를 대응하는 punctuation token으로 바꿔 배열에 추가한다. */
static int tokenize_symbol(char ch, TokenArray *tokens, char *errbuf, size_t errbuf_size) {
    TokenType type;
    char text[2];

    text[0] = ch;
    text[1] = '\0';

    switch (ch) {
        case ',':
            type = TOKEN_COMMA;
            break;
        case '(':
            type = TOKEN_LPAREN;
            break;
        case ')':
            type = TOKEN_RPAREN;
            break;
        case ';':
            type = TOKEN_SEMICOLON;
            break;
        case '*':
            type = TOKEN_STAR;
            break;
        case '=':
            type = TOKEN_EQUAL;
            break;
        default:
            set_error(errbuf, errbuf_size, "LEX ERROR: invalid character");
            return STATUS_LEX_ERROR;
    }

    return append_token(tokens, type, text, errbuf, errbuf_size);
}

/* sql 전체를 순회하며 TokenArray를 채우고 마지막에 EOF 토큰까지 붙여 상태를 반환한다. */
int tokenize_sql(const char *sql, TokenArray *out_tokens, char *errbuf, size_t errbuf_size) {
    size_t index = 0;
    int status = STATUS_OK;

    if (out_tokens == NULL || sql == NULL) {
        set_error(errbuf, errbuf_size, "LEX ERROR: invalid input");
        return STATUS_LEX_ERROR;
    }

    out_tokens->items = NULL;
    out_tokens->count = 0;
    out_tokens->capacity = 0;

    if (errbuf != NULL && errbuf_size > 0) {
        errbuf[0] = '\0';
    }

    while (sql[index] != '\0') {
        char ch = sql[index];

        if (isspace((unsigned char)ch)) {
            ++index;
            continue;
        }

        if (isalpha((unsigned char)ch) || ch == '_') {
            status = tokenize_identifier(sql, &index, out_tokens, errbuf, errbuf_size);
        } else if (isdigit((unsigned char)ch) || (ch == '-' && isdigit((unsigned char)sql[index + 1]))) {
            status = tokenize_number(sql, &index, out_tokens, errbuf, errbuf_size);
        } else if (ch == '\'') {
            status = tokenize_string(sql, &index, out_tokens, errbuf, errbuf_size);
        } else {
            status = tokenize_symbol(ch, out_tokens, errbuf, errbuf_size);
            ++index;
        }

        if (status != STATUS_OK) {
            free_token_array(out_tokens);
            return status;
        }
    }

    status = append_token(out_tokens, TOKEN_EOF, "", errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_token_array(out_tokens);
        return status;
    }

    return STATUS_OK;
}

/* tokens가 소유한 각 token text와 items 배열 메모리를 모두 해제한다. */
void free_token_array(TokenArray *tokens) {
    size_t i;

    if (tokens == NULL) {
        return;
    }

    for (i = 0; i < tokens->count; ++i) {
        free(tokens->items[i].text);
    }

    free(tokens->items);
    tokens->items = NULL;
    tokens->count = 0;
    tokens->capacity = 0;
}

/* TokenType 값을 사람이 읽기 쉬운 디버그 문자열 이름으로 변환해 반환한다. */
const char *token_type_name(TokenType type) {
    switch (type) {
        case TOKEN_EOF:
            return "TOKEN_EOF";
        case TOKEN_IDENTIFIER:
            return "TOKEN_IDENTIFIER";
        case TOKEN_STRING:
            return "TOKEN_STRING";
        case TOKEN_NUMBER:
            return "TOKEN_NUMBER";
        case TOKEN_COMMA:
            return "TOKEN_COMMA";
        case TOKEN_LPAREN:
            return "TOKEN_LPAREN";
        case TOKEN_RPAREN:
            return "TOKEN_RPAREN";
        case TOKEN_SEMICOLON:
            return "TOKEN_SEMICOLON";
        case TOKEN_STAR:
            return "TOKEN_STAR";
        case TOKEN_INSERT:
            return "TOKEN_INSERT";
        case TOKEN_INTO:
            return "TOKEN_INTO";
        case TOKEN_VALUES:
            return "TOKEN_VALUES";
        case TOKEN_SELECT:
            return "TOKEN_SELECT";
        case TOKEN_FROM:
            return "TOKEN_FROM";
        case TOKEN_WHERE:
            return "TOKEN_WHERE";
        case TOKEN_EQUAL:
            return "TOKEN_EQUAL";
        default:
            return "TOKEN_UNKNOWN";
    }
}
