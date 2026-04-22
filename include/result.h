#ifndef RESULT_H
#define RESULT_H

#include <stddef.h>
#include <stdint.h>

#include "schema.h"

/* 실행 결과가 INSERT 요약인지 SELECT 결과 집합인지 구분하는 열거형이다. */
typedef enum {
    RESULT_INSERT, /* INSERT 실행 결과다. */
    RESULT_SELECT  /* SELECT 실행 결과다. */
} ExecResultType;

/* SELECT 결과의 column 헤더와 row 집합을 담는 구조체다. */
typedef struct {
    char **columns;        /* 출력할 컬럼 이름 배열이다. */
    size_t column_count;   /* columns 배열 길이다. */
    Row *rows;             /* 출력할 row 배열이다. */
    size_t row_count;      /* rows 배열 길이다. */
} QueryResult;

/* executor가 한 문장 실행 후 caller에게 돌려주는 최종 결과 구조체다. */
typedef struct {
    ExecResultType type;       /* INSERT인지 SELECT인지 나타낸다. */
    size_t affected_rows;      /* INSERT 행 수 또는 SELECT 매칭 행 수다. */
    QueryResult query_result;  /* SELECT일 때 실제 결과 집합을 담는다. */
    int used_index;            /* SELECT가 B+Tree 인덱스를 사용했는지 나타낸다. */
    int has_generated_id;      /* INSERT에서 자동 생성 id가 유효한지 나타낸다. */
    uint64_t generated_id;     /* INSERT에서 생성한 auto-increment id 값이다. */
} ExecResult;

/* ExecResult를 CLI 표 형식 또는 INSERT 요약 문자열로 stdout에 출력한다. */
void print_exec_result(const ExecResult *result);

#endif
