#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "benchmark.h"
#include "errors.h"

/* benchmark 바이너리의 CLI 인자 형식을 stdout에 출력한다. */
static void print_usage(const char *program_name)
{
    printf("Usage: %s -d <db_dir> -t <table_name> -n <row_count> -p <probe_count>\n", program_name);
}

/* 10진수 문자열 text를 size_t로 파싱해 out_value에 저장하고 성공 시 1을 반환한다. */
static int parse_size_arg(const char *text, size_t *out_value)
{
    char *endptr = NULL;
    unsigned long long value;

    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return 0;
    }

    value = strtoull(text, &endptr, 10);
    if (endptr == NULL || *endptr != '\0') {
        return 0;
    }

    *out_value = (size_t)value;
    return 1;
}

/* CLI 인자를 해석한 뒤 run_benchmark를 호출하고 측정 결과를 사람이 읽기 쉬운 형식으로 출력한다. */
int main(int argc, char **argv)
{
    const char *db_dir = NULL;
    const char *table_name = "users";
    size_t row_count = 1000000U;
    size_t probe_count = 100U;
    BenchmarkReport report;
    char errbuf[256] = {0};
    int i;
    int status;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            db_dir = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            table_name = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            if (!parse_size_arg(argv[++i], &row_count)) {
                print_usage(argv[0]);
                return STATUS_INVALID_ARGS;
            }
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            if (!parse_size_arg(argv[++i], &probe_count)) {
                print_usage(argv[0]);
                return STATUS_INVALID_ARGS;
            }
        } else {
            print_usage(argv[0]);
            return STATUS_INVALID_ARGS;
        }
    }

    if (db_dir == NULL) {
        print_usage(argv[0]);
        return STATUS_INVALID_ARGS;
    }

    status = run_benchmark(db_dir, table_name, row_count, probe_count, &report, errbuf, sizeof(errbuf));
    if (status != STATUS_OK) {
        if (errbuf[0] != '\0') {
            fprintf(stderr, "%s\n", errbuf);
        }
        return status;
    }

    printf("Rows inserted: %zu\n", report.row_count);
    printf("Insert total: %.2f ms\n", report.insert_total_ms);
    printf("ID select avg: %.2f ms\n", report.id_select_avg_ms);
    printf("Name select avg: %.2f ms\n", report.non_id_select_avg_ms);
    printf("Speedup: %.2fx\n", report.speedup_ratio);
    return STATUS_OK;
}
