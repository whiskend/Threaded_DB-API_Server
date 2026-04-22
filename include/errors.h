#ifndef ERRORS_H
#define ERRORS_H

/* 모듈 간 공통으로 사용하는 전체 프로그램 상태 코드 열거형이다. */
typedef enum {
    STATUS_OK = 0,          /* 정상 처리 완료 상태다. */
    STATUS_INVALID_ARGS = 1, /* CLI 또는 함수 인자가 잘못됐음을 뜻한다. */
    STATUS_FILE_ERROR = 2,   /* 파일 자체를 읽지 못했거나 비어 있는 경우다. */
    STATUS_LEX_ERROR = 3,    /* SQL 토큰화 단계에서 오류가 난 경우다. */
    STATUS_PARSE_ERROR = 4,  /* AST 파싱 단계에서 문법 오류가 난 경우다. */
    STATUS_SCHEMA_ERROR = 5, /* schema 로드/검증 단계에서 오류가 난 경우다. */
    STATUS_STORAGE_ERROR = 6, /* data 파일 입출력이나 row 복원에서 오류가 난 경우다. */
    STATUS_EXEC_ERROR = 7,   /* executor의 비즈니스 규칙 검증/실행 단계 오류다. */
    STATUS_INDEX_ERROR = 8   /* B+Tree나 runtime index 처리 단계 오류다. */
} StatusCode;

#endif
