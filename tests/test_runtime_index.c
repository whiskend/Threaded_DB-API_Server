#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "runtime.h"
#include "schema.h"
#include "storage.h"

#ifdef _WIN32
#include <direct.h>
/* 테스트용 디렉터리를 생성하기 위한 Windows mkdir 래퍼 매크로다. */
#define MKDIR(path) _mkdir(path)
/* 테스트용 디렉터리를 제거하기 위한 Windows rmdir 래퍼 매크로다. */
#define RMDIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
/* 테스트용 디렉터리를 생성하기 위한 POSIX mkdir 래퍼 매크로다. */
#define MKDIR(path) mkdir((path), 0777)
/* 테스트용 디렉터리를 제거하기 위한 POSIX rmdir 래퍼 매크로다. */
#define RMDIR(path) rmdir(path)
#endif

/* 하나라도 실패한 테스트가 있었는지 기록하는 전역 플래그다. */
static int tests_failed = 0;

/* 실패 메시지와 파일/줄 번호를 출력해 어떤 검증이 깨졌는지 기록한다. */
static void fail_test(const char *message, const char *file, int line)
{
    fprintf(stderr, "TEST FAILED at %s:%d: %s\n", file, line, message);
    tests_failed = 1;
}

/* expr가 거짓이면 실패를 기록하고 현재 테스트를 종료한다. */
#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fail_test(#expr, __FILE__, __LINE__); \
        return; \
    } \
} while (0)

/* 두 문자열이 다르면 기대값과 실제값을 기록하고 현재 테스트를 종료한다. */
#define ASSERT_STREQ(expected, actual) do { \
    if (strcmp((expected), (actual)) != 0) { \
        char _message[512]; \
        snprintf(_message, sizeof(_message), "expected '%s' but got '%s'", (expected), (actual)); \
        fail_test(_message, __FILE__, __LINE__); \
        return; \
    } \
} while (0)

/* offset 추적 테스트에서 몇 번째 row를 봤는지와 캡처한 row offset을 보관한다. */
typedef struct {
    /* scan callback이 지금까지 방문한 row 개수다. */
    size_t seen_rows;
    /* 목표 row가 시작한 파일 오프셋이다. */
    long target_offset;
} OffsetCapture;

/* path가 존재하면 삭제를 시도해 테스트 fixture를 초기화한다. */
static void remove_if_exists(const char *path)
{
    remove(path);
}

/* path 디렉터리가 없으면 생성해 runtime 테스트용 DB 디렉터리를 준비한다. */
static void ensure_directory(const char *path)
{
    if (MKDIR(path) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create directory %s: %s\n", path, strerror(errno));
        exit(1);
    }
}

/* runtime index 테스트가 사용하는 schema/data 파일과 디렉터리를 제거한다. */
static void cleanup_test_db(void)
{
    remove_if_exists("build/test_runtime_db/users.schema");
    remove_if_exists("build/test_runtime_db/users.data");
    RMDIR("build/test_runtime_db");
}

/* users 테이블 schema fixture를 생성해 runtime 로딩 테스트의 기반을 만든다. */
static void write_users_schema(void)
{
    FILE *schema_file;

    cleanup_test_db();
    ensure_directory("build");
    ensure_directory("build/test_runtime_db");

    schema_file = fopen("build/test_runtime_db/users.schema", "w");
    if (schema_file == NULL) {
        fprintf(stderr, "Failed to open runtime schema file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("id\nname\nage\n", schema_file);
    fclose(schema_file);
}

/* content 그대로 users.data에 기록해 기존 데이터에서 index를 rebuild하는 시나리오를 만든다. */
static void write_users_data(const char *content)
{
    FILE *data_file = fopen("build/test_runtime_db/users.data", "w");

    if (data_file == NULL) {
        fprintf(stderr, "Failed to open runtime data file: %s\n", strerror(errno));
        exit(1);
    }

    fputs(content, data_file);
    fclose(data_file);
}

/* scan callback으로 두 번째 row를 만났을 때 그 row의 파일 오프셋을 OffsetCapture에 저장한다. */
static int capture_second_row_offset(const Row *row,
                                     long row_offset,
                                     void *user_data,
                                     char *errbuf,
                                     size_t errbuf_size)
{
    OffsetCapture *capture = (OffsetCapture *)user_data;

    (void)row;
    (void)errbuf;
    (void)errbuf_size;

    capture->seen_rows += 1U;
    if (capture->seen_rows == 2U) {
        capture->target_offset = row_offset;
        return 1;
    }

    return 0;
}

/* 기존 data 파일에서 id 인덱스를 빌드하고 next_id가 max id + 1이 되는지 검증한다. */
static void test_build_index_and_next_id_success(void)
{
    ExecutionContext ctx = {0};
    TableRuntime *table = NULL;
    char errbuf[256] = {0};
    long offset = 0L;
    int found = 0;
    Row row = {0};

    write_users_schema();
    write_users_data("1|Alice|20\n2|Bob|25\n5|Chris|30\n");

    ASSERT_TRUE(init_execution_context("build/test_runtime_db", &ctx, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(get_or_load_table_runtime(&ctx, "users", &table, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(table->has_id_column == 1);
    ASSERT_TRUE(table->id_index_ready == 1);
    ASSERT_TRUE(table->next_id == 6U);
    ASSERT_TRUE(bptree_search(&table->id_index, 2U, &offset, &found, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(found == 1);
    ASSERT_TRUE(read_row_at_offset("build/test_runtime_db", "users", offset, 3U, &row, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_STREQ("2", row.values[0]);
    ASSERT_STREQ("Bob", row.values[1]);
    ASSERT_STREQ("25", row.values[2]);

    free_row(&row);
    free_execution_context(&ctx);
}

/* 기존 data 파일에 duplicate id가 있으면 runtime 로딩이 INDEX ERROR로 실패하는지 확인한다. */
static void test_duplicate_id_build_fails(void)
{
    ExecutionContext ctx = {0};
    TableRuntime *table = NULL;
    char errbuf[256] = {0};

    write_users_schema();
    write_users_data("1|Alice|20\n1|Bob|25\n");

    ASSERT_TRUE(init_execution_context("build/test_runtime_db", &ctx, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(get_or_load_table_runtime(&ctx, "users", &table, errbuf, sizeof(errbuf)) == STATUS_INDEX_ERROR);
    free_execution_context(&ctx);
}

/* 비어 있는 id 값이 저장된 data 파일은 인덱스 빌드에 실패해야 함을 검증한다. */
static void test_empty_id_build_fails(void)
{
    ExecutionContext ctx = {0};
    TableRuntime *table = NULL;
    char errbuf[256] = {0};

    write_users_schema();
    write_users_data("|Alice|20\n");

    ASSERT_TRUE(init_execution_context("build/test_runtime_db", &ctx, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(get_or_load_table_runtime(&ctx, "users", &table, errbuf, sizeof(errbuf)) == STATUS_INDEX_ERROR);
    free_execution_context(&ctx);
}

/* canonical positive integer가 아닌 id가 저장돼 있으면 인덱스 빌드가 실패하는지 검증한다. */
static void test_malformed_id_build_fails(void)
{
    ExecutionContext ctx = {0};
    TableRuntime *table = NULL;
    char errbuf[256] = {0};

    write_users_schema();
    write_users_data("001|Alice|20\n");

    ASSERT_TRUE(init_execution_context("build/test_runtime_db", &ctx, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(get_or_load_table_runtime(&ctx, "users", &table, errbuf, sizeof(errbuf)) == STATUS_INDEX_ERROR);
    free_execution_context(&ctx);
}

/* 저장된 row의 offset을 다시 읽어 정확한 escape 복원과 단일 row 복구가 되는지 확인한다. */
static void test_read_row_at_offset_round_trip(void)
{
    Row read_back = {0};
    char *values1[] = {"1", "Alice", "20"};
    char *values2[] = {"2", "Bob|Builder", "line1\nline2"};
    Row row1 = {values1, 3U};
    Row row2 = {values2, 3U};
    OffsetCapture capture = {0};
    char errbuf[256] = {0};

    write_users_schema();
    ASSERT_TRUE(append_row_to_table("build/test_runtime_db", "users", &row1, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(append_row_to_table("build/test_runtime_db", "users", &row2, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(scan_table_rows_with_offsets("build/test_runtime_db", "users", 3U,
                                             capture_second_row_offset, &capture,
                                             errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(capture.target_offset >= 0L);
    ASSERT_TRUE(read_row_at_offset("build/test_runtime_db", "users", capture.target_offset, 3U,
                                   &read_back, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_STREQ("2", read_back.values[0]);
    ASSERT_STREQ("Bob|Builder", read_back.values[1]);
    ASSERT_STREQ("line1\nline2", read_back.values[2]);

    free_row(&read_back);
}

/* runtime/index 관련 테스트를 전부 실행하고 결과에 맞는 종료 코드를 반환한다. */
int main(void)
{
    test_build_index_and_next_id_success();
    test_duplicate_id_build_fails();
    test_empty_id_build_fails();
    test_malformed_id_build_fails();
    test_read_row_at_offset_round_trip();
    cleanup_test_db();

    if (tests_failed != 0) {
        return 1;
    }

    puts("test_runtime_index: OK");
    return 0;
}
