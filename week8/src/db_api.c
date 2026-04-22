#include "db_api.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "errors.h"
#include "executor.h"
#include "json_writer.h"
#include "lexer.h"
#include "parser.h"
#include "result.h"
#include "utils.h"

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

static int is_effectively_empty_tokens(const TokenArray *tokens)
{
    size_t i;

    if (tokens == NULL) {
        return 1;
    }

    for (i = 0U; i < tokens->count; ++i) {
        if (tokens->items[i].type != TOKEN_SEMICOLON && tokens->items[i].type != TOKEN_EOF) {
            return 0;
        }
    }
    return 1;
}

static const Token *first_non_separator_token(const TokenArray *tokens)
{
    size_t i;

    if (tokens == NULL) {
        return NULL;
    }

    for (i = 0U; i < tokens->count; ++i) {
        if (tokens->items[i].type != TOKEN_SEMICOLON) {
            return &tokens->items[i];
        }
    }
    return NULL;
}

static int has_extra_statement(const TokenArray *tokens, size_t cursor)
{
    while (cursor < tokens->count && tokens->items[cursor].type == TOKEN_SEMICOLON) {
        ++cursor;
    }

    return cursor < tokens->count && tokens->items[cursor].type != TOKEN_EOF;
}

static const char *status_error_code(int status)
{
    switch (status) {
        case STATUS_PARSE_ERROR:
        case STATUS_LEX_ERROR:
            return "SQL_PARSE_ERROR";
        case STATUS_SCHEMA_ERROR:
            return "SCHEMA_ERROR";
        case STATUS_STORAGE_ERROR:
            return "STORAGE_ERROR";
        case STATUS_INDEX_ERROR:
            return "INDEX_ERROR";
        case STATUS_EXEC_ERROR:
            return "EXECUTION_ERROR";
        default:
            return "INTERNAL_ERROR";
    }
}

static int status_http_code(int status)
{
    switch (status) {
        case STATUS_PARSE_ERROR:
        case STATUS_LEX_ERROR:
        case STATUS_SCHEMA_ERROR:
        case STATUS_EXEC_ERROR:
            return 400;
        case STATUS_STORAGE_ERROR:
        case STATUS_INDEX_ERROR:
        default:
            return 500;
    }
}

static char *make_error_body(const char *error_code, const char *message)
{
    char *body = json_make_error(error_code, message);

    if (body != NULL) {
        return body;
    }
    return json_make_error("INTERNAL_ERROR", "failed to build JSON error response");
}

static int set_error_result(DbApiResult *result,
                            int http_status,
                            const char *error_code,
                            const char *message)
{
    if (result == NULL) {
        return 0;
    }

    result->http_status = http_status;
    result->json_body = make_error_body(error_code, message);
    return result->json_body != NULL;
}

static int append_columns(JsonBuilder *builder, const QueryResult *query_result)
{
    size_t i;

    if (!json_builder_append_char(builder, '[')) {
        return 0;
    }
    for (i = 0U; i < query_result->column_count; ++i) {
        if (i > 0U && !json_builder_append_char(builder, ',')) {
            return 0;
        }
        if (!json_builder_append_escaped_string(builder, query_result->columns[i])) {
            return 0;
        }
    }
    return json_builder_append_char(builder, ']');
}

static int append_rows(JsonBuilder *builder, const QueryResult *query_result)
{
    size_t row_index;

    if (!json_builder_append_char(builder, '[')) {
        return 0;
    }

    for (row_index = 0U; row_index < query_result->row_count; ++row_index) {
        size_t column_index;
        const Row *row = &query_result->rows[row_index];

        if (row_index > 0U && !json_builder_append_char(builder, ',')) {
            return 0;
        }
        if (!json_builder_append_char(builder, '[')) {
            return 0;
        }

        for (column_index = 0U; column_index < query_result->column_count; ++column_index) {
            const char *value = "";

            if (column_index < row->value_count && row->values[column_index] != NULL) {
                value = row->values[column_index];
            }
            if (column_index > 0U && !json_builder_append_char(builder, ',')) {
                return 0;
            }
            if (!json_builder_append_escaped_string(builder, value)) {
                return 0;
            }
        }

        if (!json_builder_append_char(builder, ']')) {
            return 0;
        }
    }

    return json_builder_append_char(builder, ']');
}

static char *exec_result_to_json(const ExecResult *result)
{
    JsonBuilder builder;
    unsigned long long generated_id;

    if (!json_builder_init(&builder)) {
        return NULL;
    }

    if (result->type == RESULT_INSERT) {
        generated_id = result->has_generated_id ? (unsigned long long)result->generated_id : 0ULL;
        if (!json_builder_append(&builder, "{\"success\":true,\"type\":\"insert\",\"affected_rows\":") ||
            !json_builder_append_size(&builder, result->affected_rows) ||
            !json_builder_append(&builder, ",\"generated_id\":") ||
            !json_builder_append_uint64(&builder, generated_id) ||
            !json_builder_append_char(&builder, '}')) {
            json_builder_free(&builder);
            return NULL;
        }
        return json_builder_take(&builder);
    }

    if (!json_builder_append(&builder, "{\"success\":true,\"type\":\"select\",\"used_index\":") ||
        !json_builder_append_bool(&builder, result->used_index) ||
        !json_builder_append(&builder, ",\"row_count\":") ||
        !json_builder_append_size(&builder, result->query_result.row_count) ||
        !json_builder_append(&builder, ",\"columns\":") ||
        !append_columns(&builder, &result->query_result) ||
        !json_builder_append(&builder, ",\"rows\":") ||
        !append_rows(&builder, &result->query_result) ||
        !json_builder_append_char(&builder, '}')) {
        json_builder_free(&builder);
        return NULL;
    }

    return json_builder_take(&builder);
}

static int run_select(DbApi *api, const Statement *stmt, ExecResult *result, char *errbuf, size_t errbuf_size)
{
    int status;
    const char *table_name = stmt->select_stmt.table_name;

    if (pthread_rwlock_wrlock(&api->rwlock) != 0) {
        snprintf(errbuf, errbuf_size, "failed to acquire preload lock");
        return STATUS_EXEC_ERROR;
    }
    status = runtime_preload_table(&api->ctx, table_name, errbuf, errbuf_size);
    pthread_rwlock_unlock(&api->rwlock);
    if (status != STATUS_OK) {
        return status;
    }

    if (pthread_rwlock_rdlock(&api->rwlock) != 0) {
        snprintf(errbuf, errbuf_size, "failed to acquire read lock");
        return STATUS_EXEC_ERROR;
    }
    status = execute_statement(&api->ctx, stmt, result, errbuf, errbuf_size);
    pthread_rwlock_unlock(&api->rwlock);
    return status;
}

static int run_insert(DbApi *api, const Statement *stmt, ExecResult *result, char *errbuf, size_t errbuf_size)
{
    int status;
    const char *table_name = stmt->insert_stmt.table_name;

    if (pthread_rwlock_wrlock(&api->rwlock) != 0) {
        snprintf(errbuf, errbuf_size, "failed to acquire preload lock");
        return STATUS_EXEC_ERROR;
    }
    status = runtime_preload_table(&api->ctx, table_name, errbuf, errbuf_size);
    pthread_rwlock_unlock(&api->rwlock);
    if (status != STATUS_OK) {
        return status;
    }

    if (pthread_rwlock_rdlock(&api->rwlock) != 0) {
        snprintf(errbuf, errbuf_size, "failed to acquire read lock");
        return STATUS_EXEC_ERROR;
    }
    status = execute_statement(&api->ctx, stmt, result, errbuf, errbuf_size);
    pthread_rwlock_unlock(&api->rwlock);
    return status;
}

int db_api_init(DbApi *api, const char *db_dir, char *errbuf, size_t errbuf_size)
{
    int status;

    if (api == NULL || db_dir == NULL) {
        if (errbuf != NULL && errbuf_size > 0U) {
            snprintf(errbuf, errbuf_size, "invalid DB API arguments");
        }
        return STATUS_INVALID_ARGS;
    }

    memset(api, 0, sizeof(*api));
    status = init_execution_context(db_dir, &api->ctx, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    if (pthread_rwlock_init(&api->rwlock, NULL) != 0) {
        free_execution_context(&api->ctx);
        if (errbuf != NULL && errbuf_size > 0U) {
            snprintf(errbuf, errbuf_size, "failed to initialize DB rwlock");
        }
        return STATUS_EXEC_ERROR;
    }

    api->initialized = 1;
    return STATUS_OK;
}

void db_api_destroy(DbApi *api)
{
    if (api == NULL || !api->initialized) {
        return;
    }

    pthread_rwlock_destroy(&api->rwlock);
    free_execution_context(&api->ctx);
    api->initialized = 0;
}

int db_api_execute_sql(DbApi *api, const char *sql, DbApiResult *out_result)
{
    char errbuf[512] = {0};
    char *sql_copy = NULL;
    char *trimmed_sql;
    TokenArray tokens = {0};
    Statement stmt = {0};
    ExecResult exec_result = {0};
    size_t cursor = 0U;
    int status = STATUS_OK;
    char *json_body = NULL;
    const Token *first_token;

    if (out_result == NULL) {
        return 0;
    }
    out_result->http_status = 500;
    out_result->json_body = NULL;

    if (api == NULL || !api->initialized || sql == NULL) {
        return set_error_result(out_result, 500, "INTERNAL_ERROR", "invalid DB API state");
    }

    sql_copy = dup_string(sql);
    if (sql_copy == NULL) {
        return set_error_result(out_result, 500, "INTERNAL_ERROR", "out of memory");
    }
    trimmed_sql = trim_whitespace(sql_copy);
    if (trimmed_sql == NULL || trimmed_sql[0] == '\0') {
        free(sql_copy);
        return set_error_result(out_result, 400, "EMPTY_QUERY", "sql must not be empty");
    }

    status = tokenize_sql(trimmed_sql, &tokens, errbuf, sizeof(errbuf));
    if (status != STATUS_OK) {
        free(sql_copy);
        free_token_array(&tokens);
        return set_error_result(out_result, 400, "SQL_PARSE_ERROR", errbuf);
    }

    if (is_effectively_empty_tokens(&tokens)) {
        free(sql_copy);
        free_token_array(&tokens);
        return set_error_result(out_result, 400, "EMPTY_QUERY", "sql must contain one statement");
    }

    first_token = first_non_separator_token(&tokens);
    if (first_token == NULL ||
        (first_token->type != TOKEN_SELECT && first_token->type != TOKEN_INSERT && first_token->type != TOKEN_EOF)) {
        free(sql_copy);
        free_token_array(&tokens);
        return set_error_result(out_result, 400, "UNSUPPORTED_QUERY", "only SELECT and INSERT are supported");
    }

    status = parse_next_statement(&tokens, &cursor, &stmt, errbuf, sizeof(errbuf));
    if (status != STATUS_OK) {
        free(sql_copy);
        free_token_array(&tokens);
        free_statement(&stmt);
        return set_error_result(out_result, 400, "SQL_PARSE_ERROR", errbuf);
    }

    if (has_extra_statement(&tokens, cursor)) {
        free(sql_copy);
        free_token_array(&tokens);
        free_statement(&stmt);
        return set_error_result(out_result, 400, "MULTI_STATEMENT_NOT_ALLOWED",
                                "only one SQL statement is allowed per request");
    }

    if (stmt.type == STMT_SELECT) {
        status = run_select(api, &stmt, &exec_result, errbuf, sizeof(errbuf));
    } else if (stmt.type == STMT_INSERT) {
        status = run_insert(api, &stmt, &exec_result, errbuf, sizeof(errbuf));
    } else {
        status = STATUS_EXEC_ERROR;
        snprintf(errbuf, sizeof(errbuf), "unsupported statement type");
    }

    if (status != STATUS_OK) {
        int http_status = status_http_code(status);
        const char *error_code = status_error_code(status);
        const char *message = errbuf[0] == '\0' ? "SQL execution failed" : errbuf;

        free_exec_result(&exec_result);
        free_statement(&stmt);
        free_token_array(&tokens);
        free(sql_copy);
        return set_error_result(out_result, http_status, error_code, message);
    }

    json_body = exec_result_to_json(&exec_result);
    free_exec_result(&exec_result);
    free_statement(&stmt);
    free_token_array(&tokens);
    free(sql_copy);

    if (json_body == NULL) {
        return set_error_result(out_result, 500, "INTERNAL_ERROR", "failed to serialize result");
    }

    out_result->http_status = 200;
    out_result->json_body = json_body;
    return 1;
}

void db_api_result_free(DbApiResult *result)
{
    if (result == NULL) {
        return;
    }

    free(result->json_body);
    result->json_body = NULL;
    result->http_status = 0;
}
