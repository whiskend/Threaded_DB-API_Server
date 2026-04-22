#include "result.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* left와 right 중 더 큰 width 값을 반환해 표 열 폭 계산에 사용한다. */
static size_t max_size(size_t left, size_t right)
{
    return left > right ? left : right;
}

/* widths와 column_count를 받아 ASCII 표의 구분선 한 줄을 stdout에 출력한다. */
static void print_separator(const size_t *widths, size_t column_count)
{
    size_t column_index;
    size_t dash_index;

    for (column_index = 0; column_index < column_count; ++column_index) {
        for (dash_index = 0; dash_index < widths[column_index]; ++dash_index) {
            putchar('-');
        }
        if (column_index + 1 < column_count) {
            fputs("-+-", stdout);
        }
    }
    putchar('\n');
}

/* result를 INSERT 요약 또는 SELECT 표 형태로 렌더링해 stdout에 출력한다. */
void print_exec_result(const ExecResult *result)
{
    size_t *widths;
    size_t column_index;
    size_t row_index;

    if (result == NULL) {
        return;
    }

    if (result->type == RESULT_INSERT) {
        printf("INSERT %zu\n", result->affected_rows);
        return;
    }

    widths = NULL;
    if (result->query_result.column_count > 0) {
        widths = (size_t *)calloc(result->query_result.column_count, sizeof(size_t));
    }

    if (result->query_result.column_count > 0 && widths == NULL) {
        fputs("SELECT result available but width calculation failed\n", stderr);
        return;
    }

    for (column_index = 0; column_index < result->query_result.column_count; ++column_index) {
        widths[column_index] = strlen(result->query_result.columns[column_index]);
    }

    for (row_index = 0; row_index < result->query_result.row_count; ++row_index) {
        for (column_index = 0; column_index < result->query_result.column_count; ++column_index) {
            const char *value = result->query_result.rows[row_index].values[column_index];
            widths[column_index] = max_size(widths[column_index], strlen(value));
        }
    }

    for (column_index = 0; column_index < result->query_result.column_count; ++column_index) {
        printf("%-*s", (int)widths[column_index], result->query_result.columns[column_index]);
        if (column_index + 1 < result->query_result.column_count) {
            fputs(" | ", stdout);
        }
    }
    putchar('\n');

    print_separator(widths, result->query_result.column_count);

    for (row_index = 0; row_index < result->query_result.row_count; ++row_index) {
        for (column_index = 0; column_index < result->query_result.column_count; ++column_index) {
            printf("%-*s",
                   (int)widths[column_index],
                   result->query_result.rows[row_index].values[column_index]);
            if (column_index + 1 < result->query_result.column_count) {
                fputs(" | ", stdout);
            }
        }
        putchar('\n');
    }

    putchar('\n');
    printf("%zu rows selected\n", result->query_result.row_count);
    free(widths);
}
