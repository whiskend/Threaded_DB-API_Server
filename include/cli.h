#ifndef CLI_H
#define CLI_H

/* CLI로 받은 DB 경로, SQL 파일 경로, help 요청 여부를 담는 옵션 구조체다. */
typedef struct {
    char *db_dir;         /* `-d` 또는 `--db`로 받은 DB 디렉터리 경로다. */
    char *sql_file;       /* `-f` 또는 `--file`로 받은 SQL 파일 경로다. */
    int help_requested;   /* `-h` 또는 `--help`가 요청됐는지 나타낸다. */
} CliOptions;

/* argc/argv를 해석해 out_options를 채우고 인자 오류 여부를 STATUS 코드로 반환한다. */
int parse_cli_args(int argc, char **argv, CliOptions *out_options);
/* program_name을 사용해 지원하는 CLI 사용법을 stdout에 출력한다. */
void print_usage(const char *program_name);

#endif
