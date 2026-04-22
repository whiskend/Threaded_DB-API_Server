#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

/* mini_db_server가 지원하는 CLI usage를 stdout에 출력한다. */
static void print_usage(const char *program_name)
{
    printf("Usage: %s -d <db_dir> [-p <port>] [-t <threads>] [-q <queue_size>]\n", program_name);
    printf("       %s --db <db_dir> [--port <port>] [--threads <threads>] [--queue-size <queue_size>]\n",
           program_name);
    printf("       %s -h | --help\n", program_name);
}

/* 10진수 문자열을 int로 파싱하고 범위를 검증한다. */
static int parse_int_arg(const char *text, int min_value, int max_value, int *out_value)
{
    char *endptr = NULL;
    long parsed;

    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return -1;
    }

    parsed = strtol(text, &endptr, 10);
    if (endptr == NULL || *endptr != '\0' || parsed < min_value || parsed > max_value) {
        return -1;
    }

    *out_value = (int)parsed;
    return 0;
}

/* 10진수 문자열을 size_t로 파싱하고 1 이상인지 검증한다. */
static int parse_size_arg(const char *text, size_t *out_value)
{
    char *endptr = NULL;
    unsigned long long parsed;

    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return -1;
    }

    parsed = strtoull(text, &endptr, 10);
    if (endptr == NULL || *endptr != '\0' || parsed == 0U) {
        return -1;
    }

    *out_value = (size_t)parsed;
    return 0;
}

/* 서버 CLI 인자를 읽어 명세된 기본값과 필수 옵션 규칙을 적용한다. */
static int parse_server_args(int argc, char **argv, ServerConfig *out_config, int *out_help)
{
    int i;

    if (out_config == NULL || out_help == NULL) {
        return -1;
    }

    memset(out_config, 0, sizeof(*out_config));
    out_config->port = 8080;
    out_config->thread_count = 4U;
    out_config->queue_capacity = 64U;
    *out_help = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            *out_help = 1;
            return 0;
        }

        if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--db") == 0) && i + 1 < argc) {
            out_config->db_dir = argv[++i];
            continue;
        }

        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            if (parse_int_arg(argv[++i], 1, 65535, &out_config->port) != 0) {
                return -1;
            }
            continue;
        }

        if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) && i + 1 < argc) {
            if (parse_size_arg(argv[++i], &out_config->thread_count) != 0) {
                return -1;
            }
            continue;
        }

        if ((strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--queue-size") == 0) && i + 1 < argc) {
            if (parse_size_arg(argv[++i], &out_config->queue_capacity) != 0) {
                return -1;
            }
            continue;
        }

        return -1;
    }

    return out_config->db_dir == NULL ? -1 : 0;
}

/* mini_db_server 진입점으로 CLI 파싱 후 server_init/run/destroy를 순서대로 호출한다. */
int main(int argc, char **argv)
{
    ServerConfig config;
    Server server;
    char errbuf[256] = {0};
    int help = 0;

    if (parse_server_args(argc, argv, &config, &help) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (help) {
        print_usage(argv[0]);
        return 0;
    }

    if (server_init(&server, &config, errbuf, sizeof(errbuf)) != 0) {
        if (errbuf[0] != '\0') {
            fprintf(stderr, "%s\n", errbuf);
        }
        return 1;
    }

    if (server_run(&server, errbuf, sizeof(errbuf)) != 0) {
        if (errbuf[0] != '\0') {
            fprintf(stderr, "%s\n", errbuf);
        }
        server_destroy(&server);
        return 1;
    }

    server_destroy(&server);
    return 0;
}
