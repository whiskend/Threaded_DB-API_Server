#include "schema.h"
#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* 실패 메시지와 파일/줄 번호를 기록해 어떤 storage 검증이 깨졌는지 보여준다. */
static void fail_test(const char *message, const char *file, int line) {
    fprintf(stderr, "TEST FAILED at %s:%d: %s\n", file, line, message);
    tests_failed = 1;
}

/* expr가 거짓이면 실패 위치를 기록하고 현재 테스트 함수를 즉시 종료한다. */
#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fail_test(#expr, __FILE__, __LINE__); \
        return; \
    } \
} while (0)

/* 두 문자열이 다르면 기대값과 실제값을 포함한 메시지로 현재 테스트를 실패시킨다. */
#define ASSERT_STREQ(expected, actual) do { \
    if (strcmp((expected), (actual)) != 0) { \
        char _message[512]; \
        snprintf(_message, sizeof(_message), "expected '%s' but got '%s'", (expected), (actual)); \
        fail_test(_message, __FILE__, __LINE__); \
        return; \
    } \
} while (0)

/* path가 존재하면 삭제를 시도해 테스트 DB fixture를 깨끗하게 만든다. */
static void remove_if_exists(const char *path) {
    remove(path);
}

/* path 디렉터리가 없으면 생성해 storage 테스트용 디렉터리를 준비한다. */
static void ensure_directory(const char *path) {
    if (MKDIR(path) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create directory %s: %s\n", path, strerror(errno));
        exit(1);
    }
}

/* storage 테스트에서 만든 schema/data 파일과 디렉터리를 제거한다. */
static void cleanup_test_db(void) {
    remove_if_exists("tests/tmp_storage_db/users.schema");
    remove_if_exists("tests/tmp_storage_db/users.data");
    remove_if_exists("tests/tmp_storage_db/dupe.schema");
    RMDIR("tests/tmp_storage_db");
}

/* users.schema fixture를 생성해 storage 테스트가 동일한 스키마를 공유하게 한다. */
static void prepare_test_db(void) {
    FILE *schema_file;

    cleanup_test_db();
    ensure_directory("tests");
    ensure_directory("tests/tmp_storage_db");

    schema_file = fopen("tests/tmp_storage_db/users.schema", "w");
    if (schema_file == NULL) {
        fprintf(stderr, "Failed to open schema file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("id\nname\nage\n", schema_file);
    fclose(schema_file);
}

/* path 파일 전체를 읽어 문자열로 반환해 raw data 파일 직렬화 결과를 검증한다. */
static char *read_entire_file(const char *path) {
    FILE *file;
    long length;
    char *buffer;
    size_t read_size;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    length = ftell(file);
    if (length < 0L) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = (char *)malloc((size_t)length + 1U);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    read_size = fread(buffer, 1U, (size_t)length, file);
    buffer[read_size] = '\0';
    fclose(file);
    return buffer;
}

/* schema 파일 로딩과 column index 조회가 정상 동작하는지 검증한다. */
static void test_load_table_schema_success(void) {
    TableSchema schema = {0};
    char errbuf[256] = {0};

    prepare_test_db();

    ASSERT_TRUE(load_table_schema("tests/tmp_storage_db", "users", &schema, errbuf, sizeof(errbuf)) == 0);
    ASSERT_TRUE(schema.column_count == 3U);
    ASSERT_STREQ("users", schema.table_name);
    ASSERT_STREQ("id", schema.columns[0]);
    ASSERT_STREQ("name", schema.columns[1]);
    ASSERT_STREQ("age", schema.columns[2]);
    ASSERT_TRUE(schema_find_column_index(&schema, "name") == 1);
    ASSERT_TRUE(schema_find_column_index(&schema, "missing") == -1);

    free_table_schema(&schema);
}

/* schema에 중복 컬럼명이 있으면 오류가 발생하는지 검증한다. */
static void test_duplicate_schema_column_error(void) {
    FILE *schema_file;
    TableSchema schema = {0};
    char errbuf[256] = {0};

    prepare_test_db();

    schema_file = fopen("tests/tmp_storage_db/dupe.schema", "w");
    if (schema_file == NULL) {
        fprintf(stderr, "Failed to open duplicate schema file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("id\nname\nid\n", schema_file);
    fclose(schema_file);

    ASSERT_TRUE(load_table_schema("tests/tmp_storage_db", "dupe", &schema, errbuf, sizeof(errbuf)) != 0);
    ASSERT_TRUE(strstr(errbuf, "duplicate column") != NULL);
    free_table_schema(&schema);
}

/* append 시 파이프, 개행, 백슬래시가 escape 규칙대로 직렬화되는지 확인한다. */
static void test_append_row_escapes_fields(void) {
    Row row = {0};
    char *values[3];
    char errbuf[256] = {0};
    char *content;

    prepare_test_db();

    values[0] = "1";
    values[1] = "Alice|Admin";
    values[2] = "line1\nline2\\done";
    row.values = values;
    row.value_count = 3U;

    ASSERT_TRUE(append_row_to_table("tests/tmp_storage_db", "users", &row, errbuf, sizeof(errbuf)) == 0);

    content = read_entire_file("tests/tmp_storage_db/users.data");
    ASSERT_TRUE(content != NULL);
    ASSERT_STREQ("1|Alice\\|Admin|line1\\nline2\\\\done\n", content);
    free(content);
}

/* 저장된 escape row가 다시 읽힐 때 원래 문자열로 정확히 복원되는지 검증한다. */
static void test_read_rows_round_trip_escape_sequences(void) {
    Row row = {0};
    Row *rows = NULL;
    size_t row_count = 0U;
    char *values[3];
    char errbuf[256] = {0};

    prepare_test_db();

    values[0] = "7";
    values[1] = "Bob|Builder";
    values[2] = "hello\npath\\value";
    row.values = values;
    row.value_count = 3U;

    ASSERT_TRUE(append_row_to_table("tests/tmp_storage_db", "users", &row, errbuf, sizeof(errbuf)) == 0);
    ASSERT_TRUE(read_all_rows_from_table("tests/tmp_storage_db", "users", 3U, &rows, &row_count, errbuf, sizeof(errbuf)) == 0);
    ASSERT_TRUE(row_count == 1U);
    ASSERT_TRUE(rows[0].value_count == 3U);
    ASSERT_STREQ("7", rows[0].values[0]);
    ASSERT_STREQ("Bob|Builder", rows[0].values[1]);
    ASSERT_STREQ("hello\npath\\value", rows[0].values[2]);

    free_rows(rows, row_count);
}

/* data 파일이 없어도 빈 테이블로 간주해 성공적으로 읽히는지 검증한다. */
static void test_read_missing_data_file_as_empty_table(void) {
    Row *rows = NULL;
    size_t row_count = 0U;
    char errbuf[256] = {0};

    prepare_test_db();
    remove_if_exists("tests/tmp_storage_db/users.data");

    ASSERT_TRUE(read_all_rows_from_table("tests/tmp_storage_db", "users", 3U, &rows, &row_count, errbuf, sizeof(errbuf)) == 0);
    ASSERT_TRUE(rows == NULL);
    ASSERT_TRUE(row_count == 0U);
}

/* 비어 있는 data 파일이 row_count 0인 정상 상태로 처리되는지 확인한다. */
static void test_read_empty_data_file(void) {
    FILE *data_file;
    Row *rows = NULL;
    size_t row_count = 0U;
    char errbuf[256] = {0};

    prepare_test_db();

    data_file = fopen("tests/tmp_storage_db/users.data", "w");
    if (data_file == NULL) {
        fprintf(stderr, "Failed to create empty data file: %s\n", strerror(errno));
        exit(1);
    }
    fclose(data_file);

    ASSERT_TRUE(read_all_rows_from_table("tests/tmp_storage_db", "users", 3U, &rows, &row_count, errbuf, sizeof(errbuf)) == 0);
    ASSERT_TRUE(rows == NULL);
    ASSERT_TRUE(row_count == 0U);
}

/* schema보다 짧은 row는 빈 문자열로 padding되어 읽히는지 검증한다. */
static void test_short_row_is_padded_for_new_schema_column(void) {
    FILE *data_file;
    Row *rows = NULL;
    size_t row_count = 0U;
    char errbuf[256] = {0};

    prepare_test_db();

    data_file = fopen("tests/tmp_storage_db/users.data", "w");
    if (data_file == NULL) {
        fprintf(stderr, "Failed to create data file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("1|Alice\n", data_file);
    fclose(data_file);

    ASSERT_TRUE(read_all_rows_from_table("tests/tmp_storage_db", "users", 3U, &rows, &row_count, errbuf, sizeof(errbuf)) == 0);
    ASSERT_TRUE(row_count == 1U);
    ASSERT_TRUE(rows[0].value_count == 3U);
    ASSERT_STREQ("1", rows[0].values[0]);
    ASSERT_STREQ("Alice", rows[0].values[1]);
    ASSERT_STREQ("", rows[0].values[2]);

    free_rows(rows, row_count);
}

/* schema보다 긴 row는 뒤쪽 필드가 잘려 현재 schema 길이에 맞춰지는지 검증한다. */
static void test_long_row_is_truncated_for_removed_schema_column(void) {
    FILE *data_file;
    Row *rows = NULL;
    size_t row_count = 0U;
    char errbuf[256] = {0};

    prepare_test_db();

    data_file = fopen("tests/tmp_storage_db/users.data", "w");
    if (data_file == NULL) {
        fprintf(stderr, "Failed to create data file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("1|Alice|20|Seoul\n", data_file);
    fclose(data_file);

    ASSERT_TRUE(read_all_rows_from_table("tests/tmp_storage_db", "users", 3U, &rows, &row_count, errbuf, sizeof(errbuf)) == 0);
    ASSERT_TRUE(row_count == 1U);
    ASSERT_TRUE(rows[0].value_count == 3U);
    ASSERT_STREQ("1", rows[0].values[0]);
    ASSERT_STREQ("Alice", rows[0].values[1]);
    ASSERT_STREQ("20", rows[0].values[2]);

    free_rows(rows, row_count);
}

/* 잘못된 escape 시퀀스가 포함된 row는 storage 오류로 감지되는지 검증한다. */
static void test_malformed_escape_row_still_errors(void) {
    FILE *data_file;
    Row *rows = NULL;
    size_t row_count = 0U;
    char errbuf[256] = {0};

    prepare_test_db();

    data_file = fopen("tests/tmp_storage_db/users.data", "w");
    if (data_file == NULL) {
        fprintf(stderr, "Failed to create malformed data file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("1|Ali\\qce|20\n", data_file);
    fclose(data_file);

    ASSERT_TRUE(read_all_rows_from_table("tests/tmp_storage_db", "users", 3U, &rows, &row_count, errbuf, sizeof(errbuf)) != 0);
    ASSERT_TRUE(strstr(errbuf, "malformed row") != NULL);
    ASSERT_TRUE(rows == NULL);
    ASSERT_TRUE(row_count == 0U);
}

/* storage 관련 테스트를 전부 실행하고 실패 플래그에 맞는 종료 코드를 반환한다. */
int main(void) {
    test_load_table_schema_success();
    test_duplicate_schema_column_error();
    test_append_row_escapes_fields();
    test_read_rows_round_trip_escape_sequences();
    test_read_missing_data_file_as_empty_table();
    test_read_empty_data_file();
    test_short_row_is_padded_for_new_schema_column();
    test_long_row_is_truncated_for_removed_schema_column();
    test_malformed_escape_row_still_errors();

    cleanup_test_db();

    if (tests_failed != 0) {
        return 1;
    }

    printf("test_storage: ok\n");
    return 0;
}
