#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

/* SQL 문자열을 어떤 토큰으로 쪼갰는지 나타내는 토큰 타입 열거형이다. */
typedef enum {
    TOKEN_EOF,        /* 입력 종료를 나타내는 sentinel 토큰이다. */
    TOKEN_IDENTIFIER, /* 테이블명/컬럼명 같은 식별자다. */
    TOKEN_STRING,     /* 작은따옴표 문자열 리터럴이다. */
    TOKEN_NUMBER,     /* 정수/실수 숫자 리터럴이다. */
    TOKEN_COMMA,      /* ',' 구분자다. */
    TOKEN_LPAREN,     /* '(' 기호다. */
    TOKEN_RPAREN,     /* ')' 기호다. */
    TOKEN_SEMICOLON,  /* ';' 문장 종료 기호다. */
    TOKEN_STAR,       /* '*' projection 기호다. */
    TOKEN_INSERT,     /* INSERT 키워드다. */
    TOKEN_INTO,       /* INTO 키워드다. */
    TOKEN_VALUES,     /* VALUES 키워드다. */
    TOKEN_SELECT,     /* SELECT 키워드다. */
    TOKEN_FROM,       /* FROM 키워드다. */
    TOKEN_WHERE,      /* WHERE 키워드다. */
    TOKEN_EQUAL       /* '=' 비교 연산자다. */
} TokenType;

/* 토큰 하나의 타입과 본문 텍스트를 함께 담는 구조체다. */
typedef struct {
    TokenType type; /* 현재 토큰의 분류다. */
    char *text;     /* 토큰 본문 문자열이다. */
} Token;

/* lexer가 동적으로 늘려 가며 채우는 토큰 배열 컨테이너다. */
typedef struct {
    Token *items;      /* 실제 Token 원소 배열 포인터다. */
    size_t count;      /* 현재 유효한 토큰 개수다. */
    size_t capacity;   /* 할당된 items 배열 용량이다. */
} TokenArray;

/* sql 전체를 토큰 배열로 분해해 out_tokens에 채우고 성공/실패 상태를 반환한다. */
int tokenize_sql(const char *sql, TokenArray *out_tokens, char *errbuf, size_t errbuf_size);
/* TokenArray 내부의 각 token text와 배열 메모리를 모두 해제한다. */
void free_token_array(TokenArray *tokens);
/* TokenType을 사람이 읽기 쉬운 문자열 이름으로 바꿔 반환한다. */
const char *token_type_name(TokenType type);

#endif
