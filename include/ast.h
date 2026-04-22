#ifndef AST_H
#define AST_H

#include <stddef.h>

/* LiteralValue.text가 문자열인지 숫자 문자열인지 구분하는 리터럴 분류 열거형이다. */
typedef enum {
    VALUE_STRING, /* 작은따옴표로 들어온 문자열 리터럴을 뜻한다. */
    VALUE_NUMBER  /* 숫자 형태로 들어온 리터럴을 뜻한다. */
} ValueType;

/* SQL 리터럴 하나를 type과 원본 텍스트 형태로 보관하는 AST 노드다. */
typedef struct {
    ValueType type; /* 리터럴이 문자열인지 숫자인지 나타낸다. */
    char *text;     /* 파싱된 리터럴 본문을 NUL 종료 문자열로 담는다. */
} LiteralValue;

/* INSERT 문 전체를 테이블명, 컬럼 목록, 값 목록으로 표현하는 AST 노드다. */
typedef struct {
    char *table_name;      /* INSERT 대상 테이블 이름이다. */
    char **columns;        /* 명시적 column list가 있을 때 컬럼 이름 배열을 담는다. */
    size_t column_count;   /* columns 배열에 들어 있는 컬럼 수다. */
    LiteralValue *values;  /* VALUES 절에 들어온 리터럴 배열이다. */
    size_t value_count;    /* values 배열에 들어 있는 리터럴 수다. */
} InsertStatement;

/* WHERE 절의 단일 equality 조건 하나를 표현하는 AST 노드다. */
typedef struct {
    int has_condition; /* WHERE 절이 실제로 존재하는지 여부다. */
    char *column_name; /* 비교 대상 컬럼 이름이다. */
    LiteralValue value; /* '=' 오른쪽 리터럴 값이다. */
} WhereClause;

/* SELECT 문의 projection 정보와 WHERE 절을 담는 AST 노드다. */
typedef struct {
    char *table_name;        /* SELECT 대상 테이블 이름이다. */
    int select_all;          /* '*'을 사용했는지 여부다. */
    char **columns;          /* 선택된 컬럼 이름 배열이다. */
    size_t column_count;     /* columns 배열 길이다. */
    WhereClause where_clause; /* 단일 WHERE 조건을 담는다. */
} SelectStatement;

/* Statement union 안에 실제로 어떤 SQL 문이 들어 있는지 구분하는 열거형이다. */
typedef enum {
    STMT_INSERT, /* union이 InsertStatement를 담고 있음을 뜻한다. */
    STMT_SELECT  /* union이 SelectStatement를 담고 있음을 뜻한다. */
} StatementType;

/* 지원하는 SQL 문 하나를 INSERT 또는 SELECT AST로 담는 최상위 노드다. */
typedef struct {
    StatementType type; /* union 안에 어떤 statement 타입이 들어 있는지 나타낸다. */
    union {
        InsertStatement insert_stmt; /* INSERT 문일 때 사용하는 payload다. */
        SelectStatement select_stmt; /* SELECT 문일 때 사용하는 payload다. */
    };
} Statement;

#endif
