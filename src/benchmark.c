#include "benchmark.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ast.h"
#include "errors.h"
#include "executor.h"
#include "runtime.h"

#ifdef _WIN32
#include <direct.h>
/* Windows 환경에서 디렉터리 생성 함수를 공통 이름 MKDIR로 맞춘다. */
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
/* POSIX 환경에서 디렉터리 생성 함수를 공통 이름 MKDIR로 맞춘다. */
#define MKDIR(path) mkdir((path), 0777)
#endif

/* 단일 메시지를 errbuf에 복사해 벤치마크 모듈 오류 문자열로 기록한다. */
static void set_error(char *errbuf, size_t errbuf_size, const char *message)
{
    if (errbuf != NULL && errbuf_size > 0U) {
        snprintf(errbuf, errbuf_size, "%s", message);
    }
}

/* CLOCK_MONOTONIC 기준 현재 시각을 밀리초 단위 double 값으로 반환한다. */
static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
}

/* path 디렉터리가 없으면 생성하고, 생성/존재 여부를 STATUS 코드로 반환한다. */
static int ensure_directory(const char *path, char *errbuf, size_t errbuf_size)
{
    if (MKDIR(path) != 0 && errno != EEXIST) {
        char message[256];

        snprintf(message, sizeof(message), "BENCHMARK ERROR: failed to create directory '%s'", path);
        set_error(errbuf, errbuf_size, message);
        return STATUS_FILE_ERROR;
    }

    return STATUS_OK;
}

/* 벤치마크 전용 schema/data 파일을 초기화해 id,name,age 구조의 테이블을 준비한다. */
static int ensure_benchmark_schema(const char *db_dir,
                                   const char *table_name,
                                   char *errbuf,
                                   size_t errbuf_size)
{
    char schema_path[1024];
    char data_path[1024];
    FILE *schema_file;
    FILE *data_file;
    int status;

    status = ensure_directory(db_dir, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/%s.schema", db_dir, table_name);
    snprintf(data_path, sizeof(data_path), "%s/%s.data", db_dir, table_name);

    schema_file = fopen(schema_path, "w");
    if (schema_file == NULL) {
        set_error(errbuf, errbuf_size, "BENCHMARK ERROR: failed to open schema file");
        return STATUS_FILE_ERROR;
    }

    fputs("id\nname\nage\n", schema_file);
    fclose(schema_file);

    data_file = fopen(data_path, "w");
    if (data_file == NULL) {
        set_error(errbuf, errbuf_size, "BENCHMARK ERROR: failed to reset data file");
        return STATUS_FILE_ERROR;
    }
    fclose(data_file);

    return STATUS_OK;
}

/* ctx/table_name에 대해 row_count건 INSERT AST를 반복 실행해 대량 데이터를 적재한다. */
static int benchmark_bulk_insert(ExecutionContext *ctx,
                                 const char *table_name,
                                 size_t row_count,
                                 char *errbuf,
                                 size_t errbuf_size)
{
    Statement stmt = {0};
    LiteralValue values[2];
    char name_buffer[64];
    char age_buffer[32];
    size_t i;

    stmt.type = STMT_INSERT;
    stmt.insert_stmt.table_name = (char *)table_name;
    stmt.insert_stmt.columns = NULL;
    stmt.insert_stmt.column_count = 0U;
    stmt.insert_stmt.values = values;
    stmt.insert_stmt.value_count = 2U;

    values[0].type = VALUE_STRING;
    values[0].text = name_buffer;
    values[1].type = VALUE_STRING;
    values[1].text = age_buffer;

    for (i = 0U; i < row_count; ++i) {
        ExecResult result = {0};
        int status;

        snprintf(name_buffer, sizeof(name_buffer), "user_%zu", i + 1U);
        snprintf(age_buffer, sizeof(age_buffer), "%zu", 20U + (i % 50U));

        status = execute_statement(ctx, &stmt, &result, errbuf, errbuf_size);
        free_exec_result(&result);
        if (status != STATUS_OK) {
            return status;
        }
    }

    return STATUS_OK;
}

/* id 조건 SELECT를 probe_count번 실행해 총 소요 시간을 out_total_ms에 기록한다. */
static int benchmark_id_selects(ExecutionContext *ctx,
                                const char *table_name,
                                size_t row_count,
                                size_t probe_count,
                                double *out_total_ms,
                                char *errbuf,
                                size_t errbuf_size)
{
    Statement stmt = {0};
    char id_buffer[32];
    size_t i;
    double start_ms;

    stmt.type = STMT_SELECT;
    stmt.select_stmt.table_name = (char *)table_name;
    stmt.select_stmt.select_all = 1;
    stmt.select_stmt.columns = NULL;
    stmt.select_stmt.column_count = 0U;
    stmt.select_stmt.where_clause.has_condition = 1;
    stmt.select_stmt.where_clause.column_name = "id";
    stmt.select_stmt.where_clause.value.type = VALUE_NUMBER;
    stmt.select_stmt.where_clause.value.text = id_buffer;

    start_ms = now_ms();
    for (i = 0U; i < probe_count; ++i) {
        ExecResult result = {0};
        int status;
        size_t probe_id = (row_count - (i % row_count));

        snprintf(id_buffer, sizeof(id_buffer), "%zu", probe_id);
        status = execute_statement(ctx, &stmt, &result, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            free_exec_result(&result);
            return status;
        }
        if (result.used_index != 1 || result.query_result.row_count != 1U) {
            free_exec_result(&result);
            set_error(errbuf, errbuf_size, "BENCHMARK ERROR: id select did not use index");
            return STATUS_EXEC_ERROR;
        }
        free_exec_result(&result);
    }
    *out_total_ms = now_ms() - start_ms;

    return STATUS_OK;
}

/* name 조건 full-scan SELECT를 probe_count번 실행해 총 소요 시간을 out_total_ms에 기록한다. */
static int benchmark_non_id_selects(ExecutionContext *ctx,
                                    const char *table_name,
                                    size_t row_count,
                                    size_t probe_count,
                                    double *out_total_ms,
                                    char *errbuf,
                                    size_t errbuf_size)
{
    Statement stmt = {0};
    char name_buffer[64];
    size_t i;
    double start_ms;

    stmt.type = STMT_SELECT;
    stmt.select_stmt.table_name = (char *)table_name;
    stmt.select_stmt.select_all = 1;
    stmt.select_stmt.columns = NULL;
    stmt.select_stmt.column_count = 0U;
    stmt.select_stmt.where_clause.has_condition = 1;
    stmt.select_stmt.where_clause.column_name = "name";
    stmt.select_stmt.where_clause.value.type = VALUE_STRING;
    stmt.select_stmt.where_clause.value.text = name_buffer;

    start_ms = now_ms();
    for (i = 0U; i < probe_count; ++i) {
        ExecResult result = {0};
        int status;
        size_t probe_id = (row_count - (i % row_count));

        snprintf(name_buffer, sizeof(name_buffer), "user_%zu", probe_id);
        status = execute_statement(ctx, &stmt, &result, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            free_exec_result(&result);
            return status;
        }
        if (result.used_index != 0 || result.query_result.row_count != 1U) {
            free_exec_result(&result);
            set_error(errbuf, errbuf_size, "BENCHMARK ERROR: non-id select unexpectedly used index");
            return STATUS_EXEC_ERROR;
        }
        free_exec_result(&result);
    }
    *out_total_ms = now_ms() - start_ms;

    return STATUS_OK;
}

/*
 * 전체 벤치마크 오케스트레이션 함수다.
 * DB 디렉터리와 테이블 이름, insert/조회 횟수를 받아 데이터를 만들고 성능을 측정한 뒤
 * out_report에 총합/평균/배수를 채워 반환한다.
 */
int run_benchmark(const char *db_dir,
                  const char *table_name,
                  size_t row_count,
                  size_t probe_count,
                  BenchmarkReport *out_report,
                  char *errbuf, size_t errbuf_size)
{
    ExecutionContext ctx = {0};
    double insert_start_ms;
    int status;

    if (db_dir == NULL || table_name == NULL || out_report == NULL ||
        row_count == 0U || probe_count == 0U) {
        set_error(errbuf, errbuf_size, "BENCHMARK ERROR: invalid benchmark arguments");
        return STATUS_EXEC_ERROR;
    }

    memset(out_report, 0, sizeof(*out_report));

    status = ensure_benchmark_schema(db_dir, table_name, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    status = init_execution_context(db_dir, &ctx, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    insert_start_ms = now_ms();
    status = benchmark_bulk_insert(&ctx, table_name, row_count, errbuf, errbuf_size);
    out_report->insert_total_ms = now_ms() - insert_start_ms;
    if (status != STATUS_OK) {
        free_execution_context(&ctx);
        return status;
    }

    status = benchmark_id_selects(&ctx, table_name, row_count, probe_count,
                                  &out_report->id_select_total_ms,
                                  errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_execution_context(&ctx);
        return status;
    }

    status = benchmark_non_id_selects(&ctx, table_name, row_count, probe_count,
                                      &out_report->non_id_select_total_ms,
                                      errbuf, errbuf_size);
    free_execution_context(&ctx);
    if (status != STATUS_OK) {
        return status;
    }

    out_report->row_count = row_count;
    out_report->probe_count = probe_count;
    out_report->id_select_avg_ms = out_report->id_select_total_ms / (double)probe_count;
    out_report->non_id_select_avg_ms = out_report->non_id_select_total_ms / (double)probe_count;
    if (out_report->id_select_avg_ms > 0.0) {
        out_report->speedup_ratio = out_report->non_id_select_avg_ms / out_report->id_select_avg_ms;
    }

    return STATUS_OK;
}
