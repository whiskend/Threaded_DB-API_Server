#include "db_api.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "executor.h"
#include "json_writer.h"
#include "lexer.h"
#include "parser.h"
#include "result.h"
#include "utils.h"

/* 문자열을 heap에 복제해 caller가 소유할 수 있게 만든다. */
static char *dup_string(const char *text)
{
    size_t length;
    char *copy;

    if (text == NULL) {
        text = "";
    }

    length = strlen(text);
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length + 1U);
    return copy;
}

/* 숫자 문자열을 JSON 버퍼에 append하는 공통 헬퍼다. */
static int append_size_value(JsonBuffer *buf, size_t value)
{
    char number[32];

    snprintf(number, sizeof(number), "%zu", value);
    return json_append(buf, number);
}

/* uint64 값 하나를 JSON 숫자로 append한다. */
static int append_u64_value(JsonBuffer *buf, uint64_t value)
{
    char number[32];

    snprintf(number, sizeof(number), "%llu", (unsigned long long)value);
    return json_append(buf, number);
}

/* boolean 값을 JSON 리터럴 true/false로 append한다. */
static int append_bool_value(JsonBuffer *buf, int value)
{
    return json_append(buf, value ? "true" : "false");
}

/* 단일 SELECT 결과의 columns 배열을 JSON으로 직렬화한다. */
static int append_columns_json(JsonBuffer *buf, const QueryResult *query_result)
{
    size_t i;

    if (json_append_char(buf, '[') != 0) {
        return -1;
    }

    for (i = 0U; i < query_result->column_count; ++i) {
        if (i > 0U && json_append_char(buf, ',') != 0) {
            return -1;
        }
        if (json_append_escaped_string(buf, query_result->columns[i]) != 0) {
            return -1;
        }
    }

    return json_append_char(buf, ']');
}

/* 단일 SELECT 결과의 rows 배열을 JSON 2차원 문자열 배열로 직렬화한다. */
static int append_rows_json(JsonBuffer *buf, const QueryResult *query_result)
{
    size_t row_index;
    size_t column_index;

    if (json_append_char(buf, '[') != 0) {
        return -1;
    }

    for (row_index = 0U; row_index < query_result->row_count; ++row_index) {
        if (row_index > 0U && json_append_char(buf, ',') != 0) {
            return -1;
        }
        if (json_append_char(buf, '[') != 0) {
            return -1;
        }

        for (column_index = 0U; column_index < query_result->column_count; ++column_index) {
            if (column_index > 0U && json_append_char(buf, ',') != 0) {
                return -1;
            }
            if (json_append_escaped_string(buf, query_result->rows[row_index].values[column_index]) != 0) {
                return -1;
            }
        }

        if (json_append_char(buf, ']') != 0) {
            return -1;
        }
    }

    return json_append_char(buf, ']');
}

/* ExecResult를 명세된 flat success JSON 형식으로 변환한다. */
static int make_success_json(const ExecResult *result, char **out_json)
{
    JsonBuffer buf;
    char *json;

    if (result == NULL || out_json == NULL) {
        return -1;
    }

    *out_json = NULL;
    if (json_buffer_init(&buf) != 0) {
        return -1;
    }

    if (result->type == RESULT_SELECT) {
        if (json_append(&buf, "{\"success\":true,\"type\":\"select\",\"used_index\":") != 0 ||
            append_bool_value(&buf, result->used_index) != 0 ||
            json_append(&buf, ",\"row_count\":") != 0 ||
            append_size_value(&buf, result->query_result.row_count) != 0 ||
            json_append(&buf, ",\"columns\":") != 0 ||
            append_columns_json(&buf, &result->query_result) != 0 ||
            json_append(&buf, ",\"rows\":") != 0 ||
            append_rows_json(&buf, &result->query_result) != 0 ||
            json_append_char(&buf, '}') != 0) {
            json_buffer_free(&buf);
            return -1;
        }
    } else {
        if (json_append(&buf, "{\"success\":true,\"type\":\"insert\",\"affected_rows\":") != 0 ||
            append_size_value(&buf, result->affected_rows) != 0) {
            json_buffer_free(&buf);
            return -1;
        }

        if (result->has_generated_id) {
            if (json_append(&buf, ",\"generated_id\":") != 0 ||
                append_u64_value(&buf, result->generated_id) != 0) {
                json_buffer_free(&buf);
                return -1;
            }
        }

        if (json_append_char(&buf, '}') != 0) {
            json_buffer_free(&buf);
            return -1;
        }
    }

    json = json_buffer_take(&buf);
    json_buffer_free(&buf);
    if (json == NULL) {
        return -1;
    }

    *out_json = json;
    return 0;
}

/* 명세된 flat error JSON 형식을 만들어 out_json에 heap 문자열로 넘긴다. */
static int make_error_json(const char *error_code, const char *message, char **out_json)
{
    JsonBuffer buf;
    char *json;

    if (out_json == NULL) {
        return -1;
    }

    *out_json = NULL;
    if (json_buffer_init(&buf) != 0) {
        return -1;
    }

    if (json_append(&buf, "{\"success\":false,\"error_code\":") != 0 ||
        json_append_escaped_string(&buf, error_code == NULL ? "INTERNAL_ERROR" : error_code) != 0 ||
        json_append(&buf, ",\"message\":") != 0 ||
        json_append_escaped_string(&buf, message == NULL ? "" : message) != 0 ||
        json_append_char(&buf, '}') != 0) {
        json_buffer_free(&buf);
        return -1;
    }

    json = json_buffer_take(&buf);
    json_buffer_free(&buf);
    if (json == NULL) {
        return -1;
    }

    *out_json = json;
    return 0;
}

/* engine StatusCode를 명세상 HTTP status와 error_code로 변환한다. */
static void map_engine_status(int status, int *out_http_status, const char **out_error_code)
{
    if (out_http_status == NULL || out_error_code == NULL) {
        return;
    }

    switch (status) {
        case STATUS_LEX_ERROR:
        case STATUS_PARSE_ERROR:
            *out_http_status = 400;
            *out_error_code = "SQL_PARSE_ERROR";
            break;
        case STATUS_SCHEMA_ERROR:
            *out_http_status = 400;
            *out_error_code = "SCHEMA_ERROR";
            break;
        case STATUS_EXEC_ERROR:
            *out_http_status = 400;
            *out_error_code = "EXECUTION_ERROR";
            break;
        case STATUS_STORAGE_ERROR:
            *out_http_status = 500;
            *out_error_code = "STORAGE_ERROR";
            break;
        case STATUS_INDEX_ERROR:
            *out_http_status = 500;
            *out_error_code = "INDEX_ERROR";
            break;
        default:
            *out_http_status = 500;
            *out_error_code = "INTERNAL_ERROR";
            break;
    }
}

/* 첫 의미 있는 토큰을 찾기 위해 leading semicolon을 건너뛴다. */
static size_t skip_leading_semicolons(const TokenArray *tokens)
{
    size_t cursor = 0U;

    while (cursor < tokens->count && tokens->items[cursor].type == TOKEN_SEMICOLON) {
        ++cursor;
    }

    return cursor;
}

/* cursor 이후에도 실제 statement가 남았는지 확인하기 위해 trailing semicolon을 건너뛴다. */
static size_t skip_trailing_semicolons(const TokenArray *tokens, size_t cursor)
{
    while (cursor < tokens->count && tokens->items[cursor].type == TOKEN_SEMICOLON) {
        ++cursor;
    }

    return cursor;
}

/* lock 연산 실패 시 INTERNAL_ERROR JSON을 만들어 반환한다. */
static int make_lock_error(char **out_json, int *out_http_status)
{
    if (out_http_status != NULL) {
        *out_http_status = 500;
    }

    if (make_error_json("INTERNAL_ERROR", "failed to acquire database lock", out_json) != 0) {
        return STATUS_EXEC_ERROR;
    }

    return STATUS_EXEC_ERROR;
}

/* DbApi가 사용할 ExecutionContext와 rwlock을 초기화한다. */
int db_api_init(DbApi *api, const char *db_dir)
{
    char errbuf[256] = {0};

    if (api == NULL || db_dir == NULL) {
        return -1;
    }

    memset(api, 0, sizeof(*api));
    if (init_execution_context(db_dir, &api->ctx, errbuf, sizeof(errbuf)) != STATUS_OK) {
        memset(api, 0, sizeof(*api));
        return -1;
    }

    if (pthread_rwlock_init(&api->db_rwlock, NULL) != 0) {
        free_execution_context(&api->ctx);
        memset(api, 0, sizeof(*api));
        return -1;
    }

    return 0;
}

/* 공유 runtime cache와 rwlock을 정리한다. */
void db_api_destroy(DbApi *api)
{
    if (api == NULL || api->ctx.db_dir == NULL) {
        return;
    }

    free_execution_context(&api->ctx);
    pthread_rwlock_destroy(&api->db_rwlock);
    memset(api, 0, sizeof(*api));
}

/* SQL 1문장을 명세된 동시성 정책과 JSON 응답 형식으로 실행한다. */
int db_api_execute_sql(DbApi *api, const char *sql, char **out_json, int *out_http_status)
{
    char *sql_copy = NULL;
    char *trimmed_sql;
    TokenArray tokens = {0};
    Statement stmt = {0};
    ExecResult result = {0};
    char errbuf[256] = {0};
    size_t cursor;
    int status = STATUS_OK;

    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (out_http_status != NULL) {
        *out_http_status = 500;
    }

    if (api == NULL || sql == NULL || out_json == NULL || out_http_status == NULL) {
        return STATUS_INVALID_ARGS;
    }

    sql_copy = dup_string(sql);
    if (sql_copy == NULL) {
        make_error_json("INTERNAL_ERROR", "out of memory", out_json);
        *out_http_status = 500;
        return STATUS_EXEC_ERROR;
    }

    trimmed_sql = trim_whitespace(sql_copy);
    if (trimmed_sql == NULL || trimmed_sql[0] == '\0') {
        make_error_json("EMPTY_QUERY", "sql query is empty", out_json);
        *out_http_status = 400;
        free(sql_copy);
        return STATUS_INVALID_ARGS;
    }

    status = tokenize_sql(trimmed_sql, &tokens, errbuf, sizeof(errbuf));
    if (status != STATUS_OK) {
        make_error_json("SQL_PARSE_ERROR", errbuf[0] != '\0' ? errbuf : "failed to tokenize sql", out_json);
        *out_http_status = 400;
        free(sql_copy);
        return status;
    }

    cursor = skip_leading_semicolons(&tokens);
    if (cursor >= tokens.count || tokens.items[cursor].type == TOKEN_EOF) {
        make_error_json("EMPTY_QUERY", "sql query is empty", out_json);
        *out_http_status = 400;
        status = STATUS_INVALID_ARGS;
        goto done;
    }

    if (tokens.items[cursor].type != TOKEN_SELECT && tokens.items[cursor].type != TOKEN_INSERT) {
        make_error_json("UNSUPPORTED_QUERY", "only SELECT and INSERT are supported", out_json);
        *out_http_status = 400;
        status = STATUS_INVALID_ARGS;
        goto done;
    }

    status = parse_next_statement(&tokens, &cursor, &stmt, errbuf, sizeof(errbuf));
    if (status != STATUS_OK) {
        make_error_json("SQL_PARSE_ERROR", errbuf[0] != '\0' ? errbuf : "failed to parse sql statement", out_json);
        *out_http_status = 400;
        goto done;
    }

    cursor = skip_trailing_semicolons(&tokens, cursor);
    if (cursor >= tokens.count || tokens.items[cursor].type != TOKEN_EOF) {
        make_error_json("MULTI_STATEMENT_NOT_ALLOWED", "only one SQL statement is allowed", out_json);
        *out_http_status = 400;
        status = STATUS_INVALID_ARGS;
        goto done;
    }

    if (stmt.type == STMT_SELECT) {
        if (pthread_rwlock_wrlock(&api->db_rwlock) != 0) {
            status = make_lock_error(out_json, out_http_status);
            goto done;
        }

        status = runtime_preload_table(&api->ctx, stmt.select_stmt.table_name, errbuf, sizeof(errbuf));
        pthread_rwlock_unlock(&api->db_rwlock);
        if (status != STATUS_OK) {
            const char *error_code = NULL;
            map_engine_status(status, out_http_status, &error_code);
            make_error_json(error_code, errbuf[0] != '\0' ? errbuf : "failed to preload table runtime", out_json);
            goto done;
        }

        if (pthread_rwlock_rdlock(&api->db_rwlock) != 0) {
            status = make_lock_error(out_json, out_http_status);
            goto done;
        }

        status = execute_statement(&api->ctx, &stmt, &result, errbuf, sizeof(errbuf));
        pthread_rwlock_unlock(&api->db_rwlock);
    } else {
        if (pthread_rwlock_wrlock(&api->db_rwlock) != 0) {
            status = make_lock_error(out_json, out_http_status);
            goto done;
        }

        status = execute_statement(&api->ctx, &stmt, &result, errbuf, sizeof(errbuf));
        pthread_rwlock_unlock(&api->db_rwlock);
    }

    if (status != STATUS_OK) {
        const char *error_code = NULL;

        map_engine_status(status, out_http_status, &error_code);
        make_error_json(error_code, errbuf[0] != '\0' ? errbuf : "sql execution failed", out_json);
        goto done;
    }

    if (make_success_json(&result, out_json) != 0) {
        make_error_json("INTERNAL_ERROR", "failed to serialize sql result", out_json);
        *out_http_status = 500;
        status = STATUS_EXEC_ERROR;
        goto done;
    }

    *out_http_status = 200;

done:
    free_exec_result(&result);
    free_statement(&stmt);
    free_token_array(&tokens);
    free(sql_copy);
    return status;
}
