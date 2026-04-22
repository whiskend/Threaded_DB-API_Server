#include "executor.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "schema.h"
#include "storage.h"

/* 선형 SELECT 경로에서 스캔 중 공유할 상태로, schema/statement/result와 WHERE 컬럼 인덱스를 함께 묶는다. */
typedef struct {
    /* projection과 컬럼명 해석에 사용할 현재 테이블 스키마다. */
    const TableSchema *schema;
    /* 현재 실행 중인 SELECT AST로, WHERE와 projection 정보를 담는다. */
    const SelectStatement *stmt;
    /* 조건을 만족한 projection row를 누적할 QueryResult 버퍼다. */
    QueryResult *result;
    /* WHERE 대상 컬럼의 schema index로, 조건이 없으면 -1이다. */
    int where_index;
} SelectFullScanState;

/* INSERT AST를 받아 runtime cache, auto-id, storage append, index 반영까지 수행하고 ExecResult를 채운다. */
static int execute_insert(ExecutionContext *ctx, const InsertStatement *stmt,
                          ExecResult *out_result,
                          char *errbuf, size_t errbuf_size);
/* SELECT AST를 받아 인덱스 경로 또는 full scan 경로를 고른 뒤 QueryResult를 구성해 반환한다. */
static int execute_select(ExecutionContext *ctx, const SelectStatement *stmt,
                          ExecResult *out_result,
                          char *errbuf, size_t errbuf_size);
/* id 자동 생성이 없는 테이블에서 schema/column list 규칙대로 최종 저장 Row를 만들고 성공 여부를 반환한다. */
static int build_insert_row_existing_behavior(const TableSchema *schema, const InsertStatement *stmt,
                                              Row *out_row,
                                              char *errbuf, size_t errbuf_size);
/* auto-id 테이블의 INSERT가 id 직접 지정 금지, 컬럼 중복 금지, value 개수 일치를 지키는지 검사해 상태 코드를 반환한다. */
static int validate_insert_columns_for_auto_id(const TableRuntime *table,
                                               const InsertStatement *stmt,
                                               char *errbuf, size_t errbuf_size);
/* auto-id 테이블용 최종 Row를 만들어 id 위치에는 generated_id 문자열을 넣고 나머지 컬럼을 채운 뒤 상태 코드를 반환한다. */
static int build_insert_row_with_generated_id(const TableRuntime *table,
                                              const InsertStatement *stmt,
                                              uint64_t generated_id,
                                              Row *out_row,
                                              char *errbuf, size_t errbuf_size);
/* SELECT projection 컬럼과 WHERE 컬럼이 schema에 존재하는지 검증하고, 문제 없으면 STATUS_OK를 반환한다. */
static int validate_select_columns(const TableSchema *schema, const SelectStatement *stmt,
                                   char *errbuf, size_t errbuf_size);
/* 현재 SELECT가 canonical integer 형태의 WHERE id = ? 인지 판정하고, 가능하면 out_id_key에 검색 키를 돌려준다. */
static int can_use_id_index(const TableRuntime *table, const SelectStatement *stmt, uint64_t *out_id_key);
/* B+Tree에서 id_key를 찾아 해당 row offset만 읽어 projection 결과를 만들고 used_index를 1로 설정한다. */
static int execute_select_with_id_index(ExecutionContext *ctx,
                                        TableRuntime *table,
                                        const SelectStatement *stmt,
                                        uint64_t id_key,
                                        ExecResult *out_result,
                                        char *errbuf, size_t errbuf_size);
/* 테이블 전체를 스트리밍 스캔하면서 WHERE와 projection을 적용해 결과를 쌓고 used_index를 0으로 유지한다. */
static int execute_select_with_full_scan(ExecutionContext *ctx,
                                         TableRuntime *table,
                                         const SelectStatement *stmt,
                                         ExecResult *out_result,
                                         char *errbuf, size_t errbuf_size);
/* source_row에서 SELECT projection에 해당하는 컬럼만 복사해 out_projected를 만들고 상태 코드를 반환한다. */
static int project_single_row(const TableSchema *schema,
                              const SelectStatement *stmt,
                              const Row *source_row,
                              Row *out_projected,
                              char *errbuf, size_t errbuf_size);
/* 이미 projection된 Row 하나를 QueryResult 뒤에 deep copy로 붙이고 성공 여부를 반환한다. */
static int append_result_row(QueryResult *result,
                             const Row *projected_row,
                             char *errbuf, size_t errbuf_size);
/* 단일 Row가 WHERE literal과 문자열 완전 일치하는지 검사해 1 또는 0을 반환한다. */
static int row_matches_where_clause(const SelectStatement *stmt, int where_index, const Row *row);
/* SELECT 결과 헤더에 들어갈 컬럼명 배열을 schema와 projection 정보로 초기화하고 상태 코드를 반환한다. */
static int initialize_query_result_columns(const TableSchema *schema,
                                           const SelectStatement *stmt,
                                           QueryResult *out_result,
                                           char *errbuf, size_t errbuf_size);
/* Row 하나를 문자열 단위로 deep copy해 dst에 복제하고, 성공 시 0 실패 시 1을 반환한다. */
static int copy_row(const Row *src, Row *dst);
/* NULL-safe 문자열 복사본을 새로 할당해 반환하며, 메모리 부족 시 NULL을 반환한다. */
static char *dup_string(const char *src);
/* full scan 중 각 row를 받아 WHERE 필터와 projection을 적용하고 result에 append하는 storage callback이다. */
static int select_full_scan_callback(const Row *row,
                                     long row_offset,
                                     void *user_data,
                                     char *errbuf,
                                     size_t errbuf_size);

/* printf 형식 문자열과 가변 인자를 받아 errbuf에 executor 계층 에러 메시지를 기록한다. */
static void set_error(char *errbuf, size_t errbuf_size, const char *fmt, ...)
{
    va_list args;

    if (errbuf == NULL || errbuf_size == 0U) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(errbuf, errbuf_size, fmt, args);
    va_end(args);
}

/* 하위 모듈 상태 코드를 executor 관점의 상태와 prefix 붙은 메시지로 변환해 caller에게 반환한다. */
static int translate_module_status(int raw_status, int mapped_status,
                                   const char *prefix,
                                   char *errbuf, size_t errbuf_size)
{
    char *copy;

    if (raw_status == STATUS_OK) {
        return STATUS_OK;
    }

    if (errbuf == NULL || errbuf_size == 0U) {
        return mapped_status;
    }

    copy = dup_string(errbuf);
    if (copy == NULL) {
        set_error(errbuf, errbuf_size, "%s", prefix);
        return mapped_status;
    }

    if (copy[0] != '\0') {
        set_error(errbuf, errbuf_size, "%s: %s", prefix, copy);
    } else {
        set_error(errbuf, errbuf_size, "%s", prefix);
    }

    free(copy);
    return mapped_status;
}

/* src의 복사본을 heap에 만들고 반환하며, src가 NULL이면 빈 문자열을 복사한다. */
static char *dup_string(const char *src)
{
    size_t length;
    char *copy;

    if (src == NULL) {
        src = "";
    }

    length = strlen(src);
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, src, length + 1U);
    return copy;
}

/* src Row의 각 필드를 heap 복사해 dst에 채우고, 성공 시 0 실패 시 1을 반환한다. */
static int copy_row(const Row *src, Row *dst)
{
    size_t i;

    if (src == NULL || dst == NULL) {
        return 1;
    }

    memset(dst, 0, sizeof(*dst));
    if (src->value_count == 0U) {
        return 0;
    }

    dst->values = (char **)calloc(src->value_count, sizeof(char *));
    if (dst->values == NULL) {
        return 1;
    }
    dst->value_count = src->value_count;

    for (i = 0U; i < src->value_count; ++i) {
        dst->values[i] = dup_string(src->values[i]);
        if (dst->values[i] == NULL) {
            free_row(dst);
            return 1;
        }
    }

    return 0;
}

/* QueryResult 내부의 columns/rows 메모리를 모두 해제하고 비어 있는 상태로 되돌린다. */
static void free_query_result_contents(QueryResult *query_result)
{
    size_t row_index;
    size_t column_index;

    if (query_result == NULL) {
        return;
    }

    if (query_result->columns != NULL) {
        for (column_index = 0U; column_index < query_result->column_count; ++column_index) {
            free(query_result->columns[column_index]);
        }
        free(query_result->columns);
    }

    if (query_result->rows != NULL) {
        for (row_index = 0U; row_index < query_result->row_count; ++row_index) {
            free_row(&query_result->rows[row_index]);
        }
        free(query_result->rows);
    }

    query_result->columns = NULL;
    query_result->column_count = 0U;
    query_result->rows = NULL;
    query_result->row_count = 0U;
}

/* Statement 타입을 보고 INSERT/SELECT 실행 함수로 위임해 최종 ExecResult와 상태 코드를 반환한다. */
int execute_statement(ExecutionContext *ctx, const Statement *stmt,
                      ExecResult *out_result,
                      char *errbuf, size_t errbuf_size)
{
    if (ctx == NULL || stmt == NULL || out_result == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid execute arguments");
        return STATUS_EXEC_ERROR;
    }

    memset(out_result, 0, sizeof(*out_result));

    switch (stmt->type) {
        case STMT_INSERT:
            return execute_insert(ctx, &stmt->insert_stmt, out_result, errbuf, errbuf_size);
        case STMT_SELECT:
            return execute_select(ctx, &stmt->select_stmt, out_result, errbuf, errbuf_size);
        default:
            set_error(errbuf, errbuf_size, "EXEC ERROR: unsupported statement type");
            return STATUS_EXEC_ERROR;
    }
}

/* ExecResult가 소유한 query_result 메모리를 해제하고 메타데이터를 초기 상태로 정리한다. */
void free_exec_result(ExecResult *result)
{
    if (result == NULL) {
        return;
    }

    free_query_result_contents(&result->query_result);
    result->affected_rows = 0U;
    result->used_index = 0;
    result->has_generated_id = 0;
    result->generated_id = 0U;
}

/* auto-id가 없는 기존 INSERT 규칙대로 최종 Row를 만들고, 규칙 위반 시 EXEC ERROR를 반환한다. */
static int build_insert_row_existing_behavior(const TableSchema *schema, const InsertStatement *stmt,
                                              Row *out_row,
                                              char *errbuf, size_t errbuf_size)
{
    size_t i;
    int *seen_columns;

    if (schema == NULL || stmt == NULL || out_row == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid insert arguments");
        return STATUS_EXEC_ERROR;
    }

    memset(out_row, 0, sizeof(*out_row));
    if (stmt->column_count == 0U) {
        if (stmt->value_count != schema->column_count) {
            set_error(errbuf, errbuf_size,
                      "EXEC ERROR: expected %zu values but got %zu",
                      schema->column_count, stmt->value_count);
            return STATUS_EXEC_ERROR;
        }
    } else if (stmt->column_count != stmt->value_count) {
        set_error(errbuf, errbuf_size,
                  "EXEC ERROR: column count %zu does not match value count %zu",
                  stmt->column_count, stmt->value_count);
        return STATUS_EXEC_ERROR;
    }

    out_row->values = (char **)calloc(schema->column_count, sizeof(char *));
    if (out_row->values == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }
    out_row->value_count = schema->column_count;

    for (i = 0U; i < schema->column_count; ++i) {
        out_row->values[i] = dup_string("");
        if (out_row->values[i] == NULL) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            free_row(out_row);
            return STATUS_EXEC_ERROR;
        }
    }

    if (stmt->column_count == 0U) {
        for (i = 0U; i < schema->column_count; ++i) {
            free(out_row->values[i]);
            out_row->values[i] = dup_string(stmt->values[i].text);
            if (out_row->values[i] == NULL) {
                set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
                free_row(out_row);
                return STATUS_EXEC_ERROR;
            }
        }
        return STATUS_OK;
    }

    seen_columns = (int *)calloc(schema->column_count, sizeof(int));
    if (seen_columns == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        free_row(out_row);
        return STATUS_EXEC_ERROR;
    }

    for (i = 0U; i < stmt->column_count; ++i) {
        int column_index = schema_find_column_index(schema, stmt->columns[i]);

        if (column_index < 0) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: unknown column '%s'", stmt->columns[i]);
            free(seen_columns);
            free_row(out_row);
            return STATUS_EXEC_ERROR;
        }
        if (seen_columns[column_index] != 0) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: duplicate column '%s'", stmt->columns[i]);
            free(seen_columns);
            free_row(out_row);
            return STATUS_EXEC_ERROR;
        }

        seen_columns[column_index] = 1;
        free(out_row->values[column_index]);
        out_row->values[column_index] = dup_string(stmt->values[i].text);
        if (out_row->values[column_index] == NULL) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            free(seen_columns);
            free_row(out_row);
            return STATUS_EXEC_ERROR;
        }
    }

    free(seen_columns);
    return STATUS_OK;
}

/* auto-id 테이블에서 사용자가 id를 넣지 않았는지와 컬럼/value 구성이 유효한지 검사한다. */
static int validate_insert_columns_for_auto_id(const TableRuntime *table,
                                               const InsertStatement *stmt,
                                               char *errbuf, size_t errbuf_size)
{
    size_t i;
    size_t non_id_column_count;
    int *seen_columns;

    if (table == NULL || stmt == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid auto-id insert arguments");
        return STATUS_EXEC_ERROR;
    }

    if (!table->has_id_column) {
        return STATUS_OK;
    }

    non_id_column_count = table->schema.column_count - 1U;
    if (stmt->column_count == 0U) {
        if (stmt->value_count != non_id_column_count) {
            set_error(errbuf, errbuf_size,
                      "EXEC ERROR: expected %zu values for auto-id insert but got %zu",
                      non_id_column_count, stmt->value_count);
            return STATUS_EXEC_ERROR;
        }
        return STATUS_OK;
    }

    if (stmt->column_count != stmt->value_count) {
        set_error(errbuf, errbuf_size,
                  "EXEC ERROR: column count %zu does not match value count %zu",
                  stmt->column_count, stmt->value_count);
        return STATUS_EXEC_ERROR;
    }

    seen_columns = (int *)calloc(table->schema.column_count, sizeof(int));
    if (seen_columns == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }

    for (i = 0U; i < stmt->column_count; ++i) {
        int column_index = schema_find_column_index(&table->schema, stmt->columns[i]);

        if (column_index < 0) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: unknown column '%s'", stmt->columns[i]);
            free(seen_columns);
            return STATUS_EXEC_ERROR;
        }
        if (column_index == table->id_column_index) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: explicit 'id' values are not allowed");
            free(seen_columns);
            return STATUS_EXEC_ERROR;
        }
        if (seen_columns[column_index] != 0) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: duplicate column '%s'", stmt->columns[i]);
            free(seen_columns);
            return STATUS_EXEC_ERROR;
        }

        seen_columns[column_index] = 1;
    }

    free(seen_columns);
    return STATUS_OK;
}

/* generated_id를 schema의 id 위치에 넣고 나머지 값을 배치해 auto-id INSERT용 최종 Row를 만든다. */
static int build_insert_row_with_generated_id(const TableRuntime *table,
                                              const InsertStatement *stmt,
                                              uint64_t generated_id,
                                              Row *out_row,
                                              char *errbuf, size_t errbuf_size)
{
    char id_buffer[32];
    size_t i;
    size_t value_index = 0U;

    if (table == NULL || stmt == NULL || out_row == NULL || !table->has_id_column) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid generated-id row arguments");
        return STATUS_EXEC_ERROR;
    }

    memset(out_row, 0, sizeof(*out_row));
    out_row->values = (char **)calloc(table->schema.column_count, sizeof(char *));
    if (out_row->values == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }
    out_row->value_count = table->schema.column_count;

    for (i = 0U; i < table->schema.column_count; ++i) {
        out_row->values[i] = dup_string("");
        if (out_row->values[i] == NULL) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            free_row(out_row);
            return STATUS_EXEC_ERROR;
        }
    }

    snprintf(id_buffer, sizeof(id_buffer), "%" PRIu64, generated_id);
    free(out_row->values[table->id_column_index]);
    out_row->values[table->id_column_index] = dup_string(id_buffer);
    if (out_row->values[table->id_column_index] == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        free_row(out_row);
        return STATUS_EXEC_ERROR;
    }

    if (stmt->column_count == 0U) {
        for (i = 0U; i < table->schema.column_count; ++i) {
            if ((int)i == table->id_column_index) {
                continue;
            }

            free(out_row->values[i]);
            out_row->values[i] = dup_string(stmt->values[value_index].text);
            if (out_row->values[i] == NULL) {
                set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
                free_row(out_row);
                return STATUS_EXEC_ERROR;
            }
            value_index += 1U;
        }
        return STATUS_OK;
    }

    for (i = 0U; i < stmt->column_count; ++i) {
        int column_index = schema_find_column_index(&table->schema, stmt->columns[i]);

        if (column_index < 0 || column_index == table->id_column_index) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: invalid auto-id insert column");
            free_row(out_row);
            return STATUS_EXEC_ERROR;
        }

        free(out_row->values[column_index]);
        out_row->values[column_index] = dup_string(stmt->values[i].text);
        if (out_row->values[column_index] == NULL) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            free_row(out_row);
            return STATUS_EXEC_ERROR;
        }
    }

    return STATUS_OK;
}

/* SELECT가 참조하는 projection/WHERE 컬럼이 모두 schema에 존재하는지 검사해 상태 코드를 반환한다. */
static int validate_select_columns(const TableSchema *schema, const SelectStatement *stmt,
                                   char *errbuf, size_t errbuf_size)
{
    size_t i;

    if (schema == NULL || stmt == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid select arguments");
        return STATUS_EXEC_ERROR;
    }

    if (!stmt->select_all && stmt->column_count == 0U) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: empty select column list");
        return STATUS_EXEC_ERROR;
    }

    if (!stmt->select_all) {
        for (i = 0U; i < stmt->column_count; ++i) {
            if (schema_find_column_index(schema, stmt->columns[i]) < 0) {
                set_error(errbuf, errbuf_size, "EXEC ERROR: unknown column '%s'", stmt->columns[i]);
                return STATUS_EXEC_ERROR;
            }
        }
    }

    if (stmt->where_clause.has_condition &&
        schema_find_column_index(schema, stmt->where_clause.column_name) < 0) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: unknown column '%s'", stmt->where_clause.column_name);
        return STATUS_EXEC_ERROR;
    }

    return STATUS_OK;
}

/* SELECT 결과 헤더에 표시할 컬럼명을 복사해 QueryResult.columns를 초기화한다. */
static int initialize_query_result_columns(const TableSchema *schema,
                                           const SelectStatement *stmt,
                                           QueryResult *out_result,
                                           char *errbuf, size_t errbuf_size)
{
    size_t i;
    size_t column_count;

    if (schema == NULL || stmt == NULL || out_result == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid query result initialization arguments");
        return STATUS_EXEC_ERROR;
    }

    column_count = stmt->select_all ? schema->column_count : stmt->column_count;
    out_result->columns = (char **)calloc(column_count, sizeof(char *));
    if (column_count > 0U && out_result->columns == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }

    out_result->column_count = column_count;
    for (i = 0U; i < column_count; ++i) {
        const char *column_name = stmt->select_all ? schema->columns[i] : stmt->columns[i];

        out_result->columns[i] = dup_string(column_name);
        if (out_result->columns[i] == NULL) {
            free_query_result_contents(out_result);
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            return STATUS_EXEC_ERROR;
        }
    }

    return STATUS_OK;
}

/* Row의 where_index 필드가 WHERE literal과 문자열로 정확히 같은지 판정해 1 또는 0을 반환한다. */
static int row_matches_where_clause(const SelectStatement *stmt, int where_index, const Row *row)
{
    const char *current_value;

    if (stmt == NULL || row == NULL) {
        return 0;
    }

    if (!stmt->where_clause.has_condition) {
        return 1;
    }

    if (where_index < 0 || (size_t)where_index >= row->value_count) {
        return 0;
    }

    current_value = row->values[where_index] == NULL ? "" : row->values[where_index];
    return strcmp(current_value, stmt->where_clause.value.text) == 0;
}

/* source_row에서 projection 대상 컬럼만 뽑아 out_projected로 복사하고 상태 코드를 반환한다. */
static int project_single_row(const TableSchema *schema,
                              const SelectStatement *stmt,
                              const Row *source_row,
                              Row *out_projected,
                              char *errbuf, size_t errbuf_size)
{
    size_t i;
    size_t column_count;

    if (schema == NULL || stmt == NULL || source_row == NULL || out_projected == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid row projection arguments");
        return STATUS_EXEC_ERROR;
    }

    memset(out_projected, 0, sizeof(*out_projected));
    column_count = stmt->select_all ? schema->column_count : stmt->column_count;
    out_projected->values = (char **)calloc(column_count, sizeof(char *));
    if (column_count > 0U && out_projected->values == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }
    out_projected->value_count = column_count;

    for (i = 0U; i < column_count; ++i) {
        int column_index = stmt->select_all ? (int)i : schema_find_column_index(schema, stmt->columns[i]);
        const char *value = "";

        if (column_index >= 0 && (size_t)column_index < source_row->value_count) {
            value = source_row->values[column_index];
        }

        out_projected->values[i] = dup_string(value);
        if (out_projected->values[i] == NULL) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            free_row(out_projected);
            return STATUS_EXEC_ERROR;
        }
    }

    return STATUS_OK;
}

/* projected_row를 QueryResult.rows 뒤에 확장 저장해 결과 집합을 한 행 늘린다. */
static int append_result_row(QueryResult *result,
                             const Row *projected_row,
                             char *errbuf, size_t errbuf_size)
{
    Row *grown_rows;

    if (result == NULL || projected_row == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid result append arguments");
        return STATUS_EXEC_ERROR;
    }

    grown_rows = (Row *)realloc(result->rows, (result->row_count + 1U) * sizeof(Row));
    if (grown_rows == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }

    result->rows = grown_rows;
    if (copy_row(projected_row, &result->rows[result->row_count]) != 0) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }

    result->row_count += 1U;
    return STATUS_OK;
}

/* storage 스캔 callback으로, 전달된 row에 WHERE를 적용하고 맞으면 projection 후 결과에 붙인다. */
static int select_full_scan_callback(const Row *row,
                                     long row_offset,
                                     void *user_data,
                                     char *errbuf,
                                     size_t errbuf_size)
{
    SelectFullScanState *state = (SelectFullScanState *)user_data;
    Row projected_row = {0};
    int status;

    (void)row_offset;

    if (row == NULL || state == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid select scan callback arguments");
        return -1;
    }

    if (!row_matches_where_clause(state->stmt, state->where_index, row)) {
        return 0;
    }

    status = project_single_row(state->schema, state->stmt, row, &projected_row, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return -1;
    }

    status = append_result_row(state->result, &projected_row, errbuf, errbuf_size);
    free_row(&projected_row);
    if (status != STATUS_OK) {
        return -1;
    }

    return 0;
}

/* 현재 SELECT가 인덱스 가능한 WHERE id = canonical_integer 형태인지 검사한다. */
static int can_use_id_index(const TableRuntime *table, const SelectStatement *stmt, uint64_t *out_id_key)
{
    if (out_id_key != NULL) {
        *out_id_key = 0U;
    }

    if (table == NULL || stmt == NULL || out_id_key == NULL) {
        return 0;
    }

    if (!table->has_id_column || !table->id_index_ready) {
        return 0;
    }

    if (!stmt->where_clause.has_condition) {
        return 0;
    }

    if (strcmp(stmt->where_clause.column_name, "id") != 0) {
        return 0;
    }

    return try_parse_indexable_id_literal(&stmt->where_clause.value, out_id_key);
}

/* B+Tree로 id_key를 찾아 단 하나의 row offset만 읽고 projection 결과를 구성한다. */
static int execute_select_with_id_index(ExecutionContext *ctx,
                                        TableRuntime *table,
                                        const SelectStatement *stmt,
                                        uint64_t id_key,
                                        ExecResult *out_result,
                                        char *errbuf, size_t errbuf_size)
{
    Row source_row = {0};
    Row projected_row = {0};
    long row_offset = 0L;
    int found = 0;
    int status;

    status = initialize_query_result_columns(&table->schema, stmt, &out_result->query_result, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    if (pthread_rwlock_rdlock(table->index_lock) != 0) {
        free_query_result_contents(&out_result->query_result);
        set_error(errbuf, errbuf_size, "EXEC ERROR: failed to acquire index read lock");
        return STATUS_EXEC_ERROR;
    }
    status = bptree_search(&table->id_index, id_key, &row_offset, &found, errbuf, errbuf_size);
    pthread_rwlock_unlock(table->index_lock);
    if (status != STATUS_OK) {
        free_query_result_contents(&out_result->query_result);
        return translate_module_status(status, STATUS_INDEX_ERROR, "INDEX ERROR", errbuf, errbuf_size);
    }

    out_result->used_index = 1;
    if (!found) {
        return STATUS_OK;
    }

    if (pthread_rwlock_rdlock(table->data_lock) != 0) {
        free_query_result_contents(&out_result->query_result);
        set_error(errbuf, errbuf_size, "EXEC ERROR: failed to acquire data read lock");
        return STATUS_EXEC_ERROR;
    }
    status = read_row_at_offset(ctx->db_dir, table->table_name, row_offset,
                                table->schema.column_count, &source_row, errbuf, errbuf_size);
    pthread_rwlock_unlock(table->data_lock);
    if (status != STATUS_OK) {
        free_query_result_contents(&out_result->query_result);
        return translate_module_status(status, STATUS_STORAGE_ERROR, "STORAGE ERROR", errbuf, errbuf_size);
    }

    if (!row_matches_where_clause(stmt, table->id_column_index, &source_row)) {
        free_row(&source_row);
        return STATUS_OK;
    }

    status = project_single_row(&table->schema, stmt, &source_row, &projected_row, errbuf, errbuf_size);
    free_row(&source_row);
    if (status != STATUS_OK) {
        free_query_result_contents(&out_result->query_result);
        return status;
    }

    status = append_result_row(&out_result->query_result, &projected_row, errbuf, errbuf_size);
    free_row(&projected_row);
    if (status != STATUS_OK) {
        free_query_result_contents(&out_result->query_result);
        return status;
    }

    return STATUS_OK;
}

/* storage의 스트리밍 row scan을 이용해 WHERE와 projection을 적용하는 기존 SELECT 경로다. */
static int execute_select_with_full_scan(ExecutionContext *ctx,
                                         TableRuntime *table,
                                         const SelectStatement *stmt,
                                         ExecResult *out_result,
                                         char *errbuf, size_t errbuf_size)
{
    SelectFullScanState state;
    int status;

    status = initialize_query_result_columns(&table->schema, stmt, &out_result->query_result, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    state.schema = &table->schema;
    state.stmt = stmt;
    state.result = &out_result->query_result;
    state.where_index = stmt->where_clause.has_condition
        ? schema_find_column_index(&table->schema, stmt->where_clause.column_name)
        : -1;

    if (pthread_rwlock_rdlock(table->data_lock) != 0) {
        free_query_result_contents(&out_result->query_result);
        set_error(errbuf, errbuf_size, "EXEC ERROR: failed to acquire data read lock");
        return STATUS_EXEC_ERROR;
    }
    status = scan_table_rows_with_offsets(ctx->db_dir, table->table_name, table->schema.column_count,
                                          select_full_scan_callback, &state,
                                          errbuf, errbuf_size);
    pthread_rwlock_unlock(table->data_lock);
    if (status != STATUS_OK) {
        free_query_result_contents(&out_result->query_result);
        return translate_module_status(status, STATUS_STORAGE_ERROR, "STORAGE ERROR", errbuf, errbuf_size);
    }

    out_result->used_index = 0;
    return STATUS_OK;
}

/* INSERT를 실행해 auto-id 생성, 파일 append, 인덱스 갱신과 ExecResult 메타데이터 기록까지 처리한다. */
static int execute_insert(ExecutionContext *ctx, const InsertStatement *stmt,
                          ExecResult *out_result,
                          char *errbuf, size_t errbuf_size)
{
    TableRuntime *table = NULL;
    Row row = {0};
    long row_offset = 0L;
    int status;

    status = get_or_load_table_runtime(ctx, stmt->table_name, &table, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        if (status == STATUS_SCHEMA_ERROR) {
            return translate_module_status(status, STATUS_SCHEMA_ERROR, "SCHEMA ERROR", errbuf, errbuf_size);
        }
        if (status == STATUS_STORAGE_ERROR) {
            return translate_module_status(status, STATUS_STORAGE_ERROR, "STORAGE ERROR", errbuf, errbuf_size);
        }
        if (status == STATUS_INDEX_ERROR) {
            return translate_module_status(status, STATUS_INDEX_ERROR, "INDEX ERROR", errbuf, errbuf_size);
        }
        return status;
    }

    if (table->has_id_column) {
        RuntimeRangeLockSet range_lock_set = {{0}, 0U};
        uint64_t generated_id;
        int range_locked = 0;

        status = validate_insert_columns_for_auto_id(table, stmt, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            return status;
        }

        if (pthread_mutex_lock(table->next_id_lock) != 0) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: failed to acquire next-id lock");
            return STATUS_EXEC_ERROR;
        }
        generated_id = table->next_id;
        if (generated_id == 0U || generated_id == UINT64_MAX) {
            pthread_mutex_unlock(table->next_id_lock);
            set_error(errbuf, errbuf_size, "EXEC ERROR: auto-generated id overflow");
            return STATUS_EXEC_ERROR;
        }
        table->next_id += 1U;
        pthread_mutex_unlock(table->next_id_lock);

        status = build_insert_row_with_generated_id(table, stmt, generated_id, &row, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            return status;
        }

        status = table_runtime_lock_id_window(table, generated_id, TABLE_RUNTIME_INSERT_ROW_LOCK_WINDOW,
                                              &range_lock_set, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            free_row(&row);
            return status;
        }
        range_locked = 1;

        if (pthread_rwlock_wrlock(table->data_lock) != 0) {
            table_runtime_unlock_id_window(table, &range_lock_set);
            free_row(&row);
            set_error(errbuf, errbuf_size, "EXEC ERROR: failed to acquire data write lock");
            return STATUS_EXEC_ERROR;
        }
        status = append_row_to_table_with_offset(ctx->db_dir, stmt->table_name, &row, &row_offset, errbuf, errbuf_size);
        pthread_rwlock_unlock(table->data_lock);
        if (status != STATUS_OK) {
            if (range_locked) {
                table_runtime_unlock_id_window(table, &range_lock_set);
            }
            free_row(&row);
            return translate_module_status(status, STATUS_STORAGE_ERROR, "STORAGE ERROR", errbuf, errbuf_size);
        }

        if (pthread_rwlock_wrlock(table->index_lock) != 0) {
            if (range_locked) {
                table_runtime_unlock_id_window(table, &range_lock_set);
            }
            free_row(&row);
            set_error(errbuf, errbuf_size, "EXEC ERROR: failed to acquire index write lock");
            return STATUS_EXEC_ERROR;
        }
        status = bptree_insert(&table->id_index, generated_id, row_offset, errbuf, errbuf_size);
        if (status == STATUS_OK) {
            table->id_index_ready = 1;
        }
        pthread_rwlock_unlock(table->index_lock);
        if (range_locked) {
            table_runtime_unlock_id_window(table, &range_lock_set);
        }
        free_row(&row);
        if (status != STATUS_OK) {
            return translate_module_status(status, STATUS_INDEX_ERROR, "INDEX ERROR", errbuf, errbuf_size);
        }

        out_result->type = RESULT_INSERT;
        out_result->affected_rows = 1U;
        out_result->has_generated_id = 1;
        out_result->generated_id = generated_id;
        return STATUS_OK;
    }

    status = build_insert_row_existing_behavior(&table->schema, stmt, &row, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    if (pthread_rwlock_wrlock(table->data_lock) != 0) {
        free_row(&row);
        set_error(errbuf, errbuf_size, "EXEC ERROR: failed to acquire data write lock");
        return STATUS_EXEC_ERROR;
    }
    status = append_row_to_table(ctx->db_dir, stmt->table_name, &row, errbuf, errbuf_size);
    pthread_rwlock_unlock(table->data_lock);
    free_row(&row);
    if (status != STATUS_OK) {
        return translate_module_status(status, STATUS_STORAGE_ERROR, "STORAGE ERROR", errbuf, errbuf_size);
    }

    out_result->type = RESULT_INSERT;
    out_result->affected_rows = 1U;
    return STATUS_OK;
}

/* SELECT를 실행하면서 id 인덱스 경로와 full scan 경로 중 하나를 선택해 QueryResult를 반환한다. */
static int execute_select(ExecutionContext *ctx, const SelectStatement *stmt,
                          ExecResult *out_result,
                          char *errbuf, size_t errbuf_size)
{
    TableRuntime *table = NULL;
    uint64_t id_key = 0U;
    int status;
    int use_index;

    status = get_or_load_table_runtime(ctx, stmt->table_name, &table, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        if (status == STATUS_SCHEMA_ERROR) {
            return translate_module_status(status, STATUS_SCHEMA_ERROR, "SCHEMA ERROR", errbuf, errbuf_size);
        }
        if (status == STATUS_STORAGE_ERROR) {
            return translate_module_status(status, STATUS_STORAGE_ERROR, "STORAGE ERROR", errbuf, errbuf_size);
        }
        if (status == STATUS_INDEX_ERROR) {
            return translate_module_status(status, STATUS_INDEX_ERROR, "INDEX ERROR", errbuf, errbuf_size);
        }
        return status;
    }

    status = validate_select_columns(&table->schema, stmt, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    if (pthread_rwlock_rdlock(table->index_lock) != 0) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: failed to acquire index read lock");
        return STATUS_EXEC_ERROR;
    }
    use_index = can_use_id_index(table, stmt, &id_key);
    pthread_rwlock_unlock(table->index_lock);

    if (use_index) {
        status = execute_select_with_id_index(ctx, table, stmt, id_key, out_result, errbuf, errbuf_size);
    } else {
        status = execute_select_with_full_scan(ctx, table, stmt, out_result, errbuf, errbuf_size);
    }

    if (status != STATUS_OK) {
        return status;
    }

    out_result->type = RESULT_SELECT;
    out_result->affected_rows = out_result->query_result.row_count;
    return STATUS_OK;
}
