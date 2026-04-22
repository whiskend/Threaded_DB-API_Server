#ifndef SCHEMA_H
#define SCHEMA_H

#include <stddef.h>

/* 한 테이블의 이름과 컬럼 순서를 메모리로 읽어 온 schema 구조체다. */
typedef struct {
    char *table_name;      /* schema가 속한 테이블 이름이다. */
    char **columns;        /* 파일에서 읽은 컬럼 이름 배열이다. */
    size_t column_count;   /* columns 배열 길이다. */
} TableSchema;

/* storage/executor가 공통으로 사용하는 row 값 배열 표현이다. */
typedef struct {
    char **values;       /* 컬럼 순서대로 저장된 필드 문자열 배열이다. */
    size_t value_count;  /* values 배열 길이다. */
} Row;

/* db_dir/table_name에 해당하는 `.schema` 파일을 읽어 out_schema를 채우고 상태를 반환한다. */
int load_table_schema(const char *db_dir, const char *table_name,
                      TableSchema *out_schema,
                      char *errbuf, size_t errbuf_size);

/* schema 안에서 column_name이 몇 번째 컬럼인지 찾아 index를 반환하고 없으면 -1을 반환한다. */
int schema_find_column_index(const TableSchema *schema, const char *column_name);
/* TableSchema 내부의 table_name, columns 배열 메모리를 모두 해제한다. */
void free_table_schema(TableSchema *schema);

#endif
