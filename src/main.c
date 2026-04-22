#include <stdio.h>
#include <stdlib.h>

#include "cli.h"
#include "errors.h"
#include "executor.h"
#include "lexer.h"
#include "parser.h"
#include "runtime.h"
#include "utils.h"

/* message가 비어 있지 않을 때만 stderr에 한 줄로 출력하는 공용 에러 출력 헬퍼다. */
static void print_error_message(const char *message)
{
    if (message != NULL && message[0] != '\0') {
        fprintf(stderr, "%s\n", message);
    }
}

/*
 * 프로그램 진입점이다.
 * CLI 인자를 받아 SQL 파일을 읽고, lexer/parser/executor/runtime을 순서대로 호출해
 * 각 SQL 문을 실행한 뒤 종료 코드를 반환한다.
 */
int main(int argc, char **argv)
{
    CliOptions options = {0};
    char *sql_text = NULL;
    char *trimmed_sql = NULL;
    TokenArray tokens = {0};
    Statement stmt = {0};
    ExecResult result = {0};
    ExecutionContext ctx = {0};
    char errbuf[256] = {0};
    size_t cursor = 0;
    size_t executed_statement_count = 0;
    int status;

    status = parse_cli_args(argc, argv, &options);
    if (status != STATUS_OK) {
        print_usage(argv[0]);
        return status;
    }

    if (options.help_requested) {
        print_usage(argv[0]);
        return STATUS_OK;
    }

    sql_text = read_text_file(options.sql_file);
    if (sql_text == NULL) {
        fprintf(stderr, "FILE ERROR: failed to read SQL file '%s'\n", options.sql_file);
        return STATUS_FILE_ERROR;
    }

    trimmed_sql = trim_whitespace(sql_text);
    if (trimmed_sql == NULL || trimmed_sql[0] == '\0') {
        fprintf(stderr, "FILE ERROR: SQL file '%s' is empty\n", options.sql_file);
        free(sql_text);
        return STATUS_FILE_ERROR;
    }

    status = tokenize_sql(trimmed_sql, &tokens, errbuf, sizeof(errbuf));
    if (status != STATUS_OK) {
        print_error_message(errbuf);
        free(sql_text);
        free_token_array(&tokens);
        return status;
    }

    status = init_execution_context(options.db_dir, &ctx, errbuf, sizeof(errbuf));
    if (status != STATUS_OK) {
        print_error_message(errbuf);
        free(sql_text);
        free_token_array(&tokens);
        return status;
    }

    while (cursor < tokens.count && tokens.items[cursor].type == TOKEN_SEMICOLON) {
        ++cursor;
    }

    while (cursor < tokens.count && tokens.items[cursor].type != TOKEN_EOF) {
        status = parse_next_statement(&tokens, &cursor, &stmt, errbuf, sizeof(errbuf));
        if (status != STATUS_OK) {
            print_error_message(errbuf);
            free_execution_context(&ctx);
            free(sql_text);
            free_token_array(&tokens);
            free_statement(&stmt);
            return status;
        }

        status = execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf));
        if (status != STATUS_OK) {
            print_error_message(errbuf);
            free_execution_context(&ctx);
            free(sql_text);
            free_token_array(&tokens);
            free_statement(&stmt);
            free_exec_result(&result);
            return status;
        }

        print_exec_result(&result);
        executed_statement_count += 1;

        free_exec_result(&result);
        free_statement(&stmt);

        while (cursor < tokens.count && tokens.items[cursor].type == TOKEN_SEMICOLON) {
            ++cursor;
        }
    }

    free_token_array(&tokens);
    free_execution_context(&ctx);
    free(sql_text);

    if (executed_statement_count == 0U) {
        fprintf(stderr, "PARSE ERROR: no executable SQL statement found\n");
        return STATUS_PARSE_ERROR;
    }

    return STATUS_OK;
}
