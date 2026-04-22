#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>

#include "schema.h"

/*
 * scan_table_rows_with_offsets가 각 row를 읽을 때 호출하는 콜백 타입이다.
 * row는 복원된 값 배열, row_offset은 해당 row 시작 파일 위치, user_data는 호출자 컨텍스트며,
 * 0은 계속, 1은 정상 조기 종료, -1은 에러 중단을 뜻한다.
 */
typedef int (*RowScanCallback)(const Row *row,
                               long row_offset,
                               void *user_data,
                               char *errbuf,
                               size_t errbuf_size);

/* table_name의 `.data` 파일이 없으면 생성하고 이미 있으면 그대로 두며 상태를 반환한다. */
int ensure_table_data_file(const char *db_dir, const char *table_name,
                           char *errbuf, size_t errbuf_size);

/*
 * row를 escape 규칙대로 `.data` 파일 끝에 append하고,
 * append 시작 파일 offset을 out_row_offset에 기록한 뒤 상태를 반환한다.
 */
int append_row_to_table_with_offset(const char *db_dir, const char *table_name,
                                    const Row *row, long *out_row_offset,
                                    char *errbuf, size_t errbuf_size);

/* row를 `.data` 파일에 append하되 row offset이 필요 없는 기존 경로용 래퍼 함수다. */
int append_row_to_table(const char *db_dir, const char *table_name,
                        const Row *row,
                        char *errbuf, size_t errbuf_size);

/*
 * `.data` 파일을 처음부터 끝까지 streaming scan 하면서 row를 복원해 callback으로 넘기고,
 * callback/입출력 결과를 STATUS 코드로 반환한다.
 */
int scan_table_rows_with_offsets(const char *db_dir, const char *table_name,
                                 size_t expected_column_count,
                                 RowScanCallback callback,
                                 void *user_data,
                                 char *errbuf, size_t errbuf_size);

/*
 * `.data` 파일에서 row_offset 위치의 한 줄만 읽어 Row로 복원해 out_row에 채우고
 * 성공/실패 상태를 반환한다.
 */
int read_row_at_offset(const char *db_dir, const char *table_name,
                       long row_offset,
                       size_t expected_column_count,
                       Row *out_row,
                       char *errbuf, size_t errbuf_size);

/* `.data` 전체를 읽어 Row 배열로 materialize하고 out_rows/out_row_count에 채운다. */
int read_all_rows_from_table(const char *db_dir, const char *table_name,
                             size_t expected_column_count,
                             Row **out_rows, size_t *out_row_count,
                             char *errbuf, size_t errbuf_size);

/* Row 하나가 소유한 values 배열과 각 문자열 메모리를 해제한다. */
void free_row(Row *row);
/* Row 배열 전체를 순회하며 각 row와 배열 메모리를 해제한다. */
void free_rows(Row *rows, size_t row_count);

#endif
