#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "executor.h"
#include "parser.h"
#include "runtime.h"

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

/* 실패 메시지와 파일/줄 번호를 출력해 어떤 assertion이 깨졌는지 기록한다. */
static void fail_test(const char *message, const char *file, int line)
{
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

/* path가 존재하면 삭제를 시도해 테스트 fixture를 깨끗한 상태로 만든다. */
static void remove_if_exists(const char *path)
{
    remove(path);
}

/* text의 복사본을 heap에 만들고 반환하며, 실패 시 테스트를 즉시 종료한다. */
static char *dup_string(const char *text)
{
    size_t length = strlen(text) + 1U;
    char *copy = (char *)malloc(length);

    if (copy == NULL) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    memcpy(copy, text, length);
    return copy;
}

/* path 디렉터리가 없으면 생성해 테스트 DB 경로를 준비한다. */
static void ensure_directory(const char *path)
{
    if (MKDIR(path) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create directory %s: %s\n", path, strerror(errno));
        exit(1);
    }
}

/* executor 테스트가 사용하는 schema/data 파일과 디렉터리를 정리한다. */
static void cleanup_test_db(void)
{
    remove_if_exists("build/test_executor_db/users.schema");
    remove_if_exists("build/test_executor_db/users.data");
    remove_if_exists("build/test_executor_db/products.schema");
    remove_if_exists("build/test_executor_db/products.data");
    RMDIR("build/test_executor_db");
}

/* auto-id가 붙는 users 테이블 schema를 fixture로 생성한다. */
static void prepare_users_schema(void)
{
    FILE *schema_file;

    cleanup_test_db();
    ensure_directory("build");
    ensure_directory("build/test_executor_db");

    schema_file = fopen("build/test_executor_db/users.schema", "w");
    if (schema_file == NULL) {
        fprintf(stderr, "Failed to open users schema file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("id\nname\nage\n", schema_file);
    fclose(schema_file);
}

/* id 컬럼이 없는 products 테이블 schema를 fixture로 생성한다. */
static void prepare_products_schema(void)
{
    FILE *schema_file;

    cleanup_test_db();
    ensure_directory("build");
    ensure_directory("build/test_executor_db");

    schema_file = fopen("build/test_executor_db/products.schema", "w");
    if (schema_file == NULL) {
        fprintf(stderr, "Failed to open products schema file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("name\nprice\n", schema_file);
    fclose(schema_file);
}

/* path 파일 전체를 읽어 NUL 종료 문자열로 반환해 저장 결과 검증에 사용한다. */
static char *read_entire_file(const char *path)
{
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
    fclose(file);

    if (read_size != (size_t)length) {
        free(buffer);
        return NULL;
    }

    buffer[length] = '\0';
    return buffer;
}

/* 테스트용 INSERT AST를 직접 구성해 parser를 거치지 않고 executor만 검증할 수 있게 한다. */
static Statement make_insert_stmt(const char *table_name,
                                  const char **columns, size_t column_count,
                                  const char **values, size_t value_count)
{
    Statement stmt = {0};
    size_t i;

    stmt.type = STMT_INSERT;
    stmt.insert_stmt.table_name = dup_string(table_name);
    stmt.insert_stmt.column_count = column_count;
    stmt.insert_stmt.value_count = value_count;

    if (column_count > 0U) {
        stmt.insert_stmt.columns = (char **)calloc(column_count, sizeof(char *));
        if (stmt.insert_stmt.columns == NULL) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        for (i = 0U; i < column_count; ++i) {
            stmt.insert_stmt.columns[i] = dup_string(columns[i]);
        }
    }

    if (value_count > 0U) {
        stmt.insert_stmt.values = (LiteralValue *)calloc(value_count, sizeof(LiteralValue));
        if (stmt.insert_stmt.values == NULL) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        for (i = 0U; i < value_count; ++i) {
            stmt.insert_stmt.values[i].type = VALUE_STRING;
            stmt.insert_stmt.values[i].text = dup_string(values[i]);
        }
    }

    return stmt;
}

/* 테스트용 SELECT AST를 직접 구성해 WHERE/projection/index 경로를 세밀하게 제어한다. */
static Statement make_select_stmt(const char *table_name,
                                  int select_all,
                                  const char **columns, size_t column_count,
                                  const char *where_column,
                                  const char *where_value,
                                  ValueType where_type)
{
    Statement stmt = {0};
    size_t i;

    stmt.type = STMT_SELECT;
    stmt.select_stmt.table_name = dup_string(table_name);
    stmt.select_stmt.select_all = select_all;
    stmt.select_stmt.column_count = column_count;

    if (column_count > 0U) {
        stmt.select_stmt.columns = (char **)calloc(column_count, sizeof(char *));
        if (stmt.select_stmt.columns == NULL) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }

        for (i = 0U; i < column_count; ++i) {
            stmt.select_stmt.columns[i] = dup_string(columns[i]);
        }
    }

    if (where_column != NULL) {
        stmt.select_stmt.where_clause.has_condition = 1;
        stmt.select_stmt.where_clause.column_name = dup_string(where_column);
        stmt.select_stmt.where_clause.value.type = where_type;
        stmt.select_stmt.where_clause.value.text = dup_string(where_value);
    }

    return stmt;
}

/* build/test_executor_db를 가리키는 ExecutionContext를 초기화해 반환한다. */
static ExecutionContext create_context(void)
{
    ExecutionContext ctx = {0};
    char errbuf[256] = {0};

    if (init_execution_context("build/test_executor_db", &ctx, errbuf, sizeof(errbuf)) != STATUS_OK) {
        fprintf(stderr, "Failed to init execution context: %s\n", errbuf);
        exit(1);
    }

    return ctx;
}

/* id 컬럼이 있는 테이블에서 VALUES INSERT 시 id가 자동 생성되고 파일에 기록되는지 검증한다. */
static void test_auto_id_insert_success(void)
{
    const char *values[] = {"Alice", "20"};
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};
    char *content;

    prepare_users_schema();
    ctx = create_context();
    stmt = make_insert_stmt("users", NULL, 0U, values, 2U);

    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.has_generated_id == 1);
    ASSERT_TRUE(result.generated_id == 1U);
    ASSERT_TRUE(result.affected_rows == 1U);

    content = read_entire_file("build/test_executor_db/users.data");
    ASSERT_TRUE(content != NULL);
    ASSERT_STREQ("1|Alice|20\n", content);

    free(content);
    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

/* column list INSERT에서도 generated id와 컬럼 재배치가 올바른지 검증한다. */
static void test_column_list_auto_id_insert_success(void)
{
    const char *columns[] = {"age", "name"};
    const char *values[] = {"21", "Bob"};
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};
    char *content;

    prepare_users_schema();
    ctx = create_context();
    stmt = make_insert_stmt("users", columns, 2U, values, 2U);

    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.generated_id == 1U);

    content = read_entire_file("build/test_executor_db/users.data");
    ASSERT_TRUE(content != NULL);
    ASSERT_STREQ("1|Bob|21\n", content);

    free(content);
    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

/* auto-id 테이블에서 사용자가 id 컬럼을 직접 지정하면 EXEC ERROR가 나는지 확인한다. */
static void test_explicit_id_insert_fails(void)
{
    const char *columns[] = {"id", "name"};
    const char *values[] = {"7", "Alice"};
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};

    prepare_users_schema();
    ctx = create_context();
    stmt = make_insert_stmt("users", columns, 2U, values, 2U);

    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_EXEC_ERROR);
    ASSERT_TRUE(strstr(errbuf, "explicit 'id'") != NULL);

    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

/* users 테이블에 두 건을 미리 넣어 이후 SELECT 테스트용 fixture를 만든다. */
static void seed_users_with_auto_ids(ExecutionContext *ctx)
{
    const char *values1[] = {"Alice", "20"};
    const char *values2[] = {"Bob", "21"};
    Statement stmt1 = make_insert_stmt("users", NULL, 0U, values1, 2U);
    Statement stmt2 = make_insert_stmt("users", NULL, 0U, values2, 2U);
    ExecResult result = {0};
    char errbuf[256] = {0};

    if (execute_statement(ctx, &stmt1, &result, errbuf, sizeof(errbuf)) != STATUS_OK) {
        fprintf(stderr, "seed insert 1 failed: %s\n", errbuf);
        exit(1);
    }
    free_exec_result(&result);

    if (execute_statement(ctx, &stmt2, &result, errbuf, sizeof(errbuf)) != STATUS_OK) {
        fprintf(stderr, "seed insert 2 failed: %s\n", errbuf);
        exit(1);
    }
    free_exec_result(&result);

    free_statement(&stmt1);
    free_statement(&stmt2);
}

/* WHERE id = ? 조회가 B+Tree 인덱스를 타고 used_index를 1로 남기는지 검증한다. */
static void test_select_where_id_uses_index(void)
{
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};

    prepare_users_schema();
    ctx = create_context();
    seed_users_with_auto_ids(&ctx);

    stmt = make_select_stmt("users", 1, NULL, 0U, "id", "2", VALUE_NUMBER);
    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.used_index == 1);
    ASSERT_TRUE(result.query_result.row_count == 1U);
    ASSERT_STREQ("2", result.query_result.rows[0].values[0]);
    ASSERT_STREQ("Bob", result.query_result.rows[0].values[1]);

    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

/* non-id 조건 조회는 기존 full scan 경로를 사용해 used_index가 0인지 확인한다. */
static void test_select_where_non_id_uses_full_scan(void)
{
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};

    prepare_users_schema();
    ctx = create_context();
    seed_users_with_auto_ids(&ctx);

    stmt = make_select_stmt("users", 1, NULL, 0U, "name", "Bob", VALUE_STRING);
    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.used_index == 0);
    ASSERT_TRUE(result.query_result.row_count == 1U);
    ASSERT_STREQ("2", result.query_result.rows[0].values[0]);

    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

/* canonical integer가 아닌 id literal은 의미 보존을 위해 인덱스를 타지 않는지 확인한다. */
static void test_non_canonical_id_literal_does_not_use_index(void)
{
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};

    prepare_users_schema();
    ctx = create_context();
    seed_users_with_auto_ids(&ctx);

    stmt = make_select_stmt("users", 1, NULL, 0U, "id", "001", VALUE_STRING);
    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.used_index == 0);
    ASSERT_TRUE(result.query_result.row_count == 0U);

    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

/* id 컬럼이 없는 테이블은 auto-id 없이 기존 INSERT/SELECT 동작을 유지하는지 본다. */
static void test_non_indexed_table_keeps_existing_behavior(void)
{
    const char *values[] = {"apple", "1000"};
    Statement insert_stmt;
    Statement select_stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};

    prepare_products_schema();
    ctx = create_context();

    insert_stmt = make_insert_stmt("products", NULL, 0U, values, 2U);
    ASSERT_TRUE(execute_statement(&ctx, &insert_stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.has_generated_id == 0);
    free_exec_result(&result);
    free_statement(&insert_stmt);

    select_stmt = make_select_stmt("products", 1, NULL, 0U, "name", "apple", VALUE_STRING);
    ASSERT_TRUE(execute_statement(&ctx, &select_stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.used_index == 0);
    ASSERT_TRUE(result.query_result.row_count == 1U);
    ASSERT_STREQ("apple", result.query_result.rows[0].values[0]);
    ASSERT_STREQ("1000", result.query_result.rows[0].values[1]);

    free_exec_result(&result);
    free_statement(&select_stmt);
    free_execution_context(&ctx);
}

/* executor 관련 테스트를 전부 실행하고 실패 플래그에 따라 종료 코드를 반환한다. */
int main(void)
{
    test_auto_id_insert_success();
    test_column_list_auto_id_insert_success();
    test_explicit_id_insert_fails();
    test_select_where_id_uses_index();
    test_select_where_non_id_uses_full_scan();
    test_non_canonical_id_literal_does_not_use_index();
    test_non_indexed_table_keeps_existing_behavior();
    cleanup_test_db();

    if (tests_failed != 0) {
        return 1;
    }

    puts("test_executor: OK");
    return 0;
}
