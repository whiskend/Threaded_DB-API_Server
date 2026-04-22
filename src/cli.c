#include "cli.h"

#include <stdio.h>
#include <string.h>

/* cli.c 내부에서만 쓰는 간단한 상태 코드 별칭이다. */
enum {
    STATUS_OK = 0,          /* 인자 파싱이 정상 종료됐음을 뜻한다. */
    STATUS_INVALID_ARGS = 1 /* 인자 조합이 올바르지 않음을 뜻한다. */
};

/* options를 빈 기본값 상태로 초기화해 이후 인자 파싱의 시작점으로 만든다. */
static void init_options(CliOptions *options) {
    options->db_dir = NULL;
    options->sql_file = NULL;
    options->help_requested = 0;
}

/* program_name을 넣어 현재 CLI가 지원하는 호출 형태를 stdout에 안내한다. */
void print_usage(const char *program_name) {
    printf("Usage: %s -d <db_dir> -f <sql_file>\n", program_name);
    printf("       %s --db <db_dir> --file <sql_file>\n", program_name);
    printf("       %s -h | --help\n", program_name);
}

/* argc/argv를 순회하며 DB 경로, SQL 파일, help 요청을 해석해 out_options에 채운다. */
int parse_cli_args(int argc, char **argv, CliOptions *out_options) {
    int i;

    if (out_options == NULL) {
        return STATUS_INVALID_ARGS;
    }

    init_options(out_options);

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            out_options->help_requested = 1;
            continue;
        }

        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--db") == 0) {
            if (i + 1 >= argc) {
                return STATUS_INVALID_ARGS;
            }
            out_options->db_dir = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) {
            if (i + 1 >= argc) {
                return STATUS_INVALID_ARGS;
            }
            out_options->sql_file = argv[++i];
            continue;
        }

        return STATUS_INVALID_ARGS;
    }

    if (out_options->help_requested) {
        return STATUS_OK;
    }

    if (out_options->db_dir == NULL || out_options->sql_file == NULL) {
        return STATUS_INVALID_ARGS;
    }

    return STATUS_OK;
}
