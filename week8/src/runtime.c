#include "runtime.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "storage.h"
#include "utils.h"

/* 인덱스 빌드 중 현재 id 컬럼 위치, 최대 id, 빌드 대상 트리를 전달하는 상태 구조체다. */
typedef struct {
    int id_column_index; /* 스캔 중인 row에서 id 컬럼이 몇 번째인지 나타낸다. */
    BPTree *tree;        /* 현재 row를 삽입할 대상 B+Tree다. */
    uint64_t max_id;     /* 지금까지 본 가장 큰 stored id다. */
    int saw_row;         /* 최소 한 행 이상 읽었는지 나타낸다. */
} IdIndexBuildState;

/* runtime 모듈 내부에서 서식 문자열 기반 에러 메시지를 errbuf에 기록한다. */
static void set_error(char *errbuf, size_t errbuf_size, const char *fmt, ...)
{
    va_list args;

    if (errbuf == NULL || errbuf_size == 0U) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(errbuf, errbuf_size, fmt, args);
    va_end(args);
}

/* text를 heap 문자열로 복제해 반환하고 NULL이면 빈 문자열로 간주한다. */
static char *dup_string(const char *text)
{
    size_t length;
    char *copy;

    if (text == NULL) {
        text = "";
    }

    length = strlen(text);
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length + 1U);
    return copy;
}

/* TableRuntime이 소유한 pthread lock들을 초기화한다. */
static int init_table_runtime_locks(TableRuntime *table, char *errbuf, size_t errbuf_size)
{
    size_t initialized_range_count = 0U;
    int next_id_initialized = 0;
    int data_initialized = 0;
    int index_initialized = 0;

    if (table == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid table lock initialization arguments");
        return STATUS_EXEC_ERROR;
    }

    table->next_id_lock = (pthread_mutex_t *)calloc(1U, sizeof(*table->next_id_lock));
    table->data_lock = (pthread_rwlock_t *)calloc(1U, sizeof(*table->data_lock));
    table->index_lock = (pthread_rwlock_t *)calloc(1U, sizeof(*table->index_lock));
    table->range_locks = (pthread_mutex_t *)calloc(TABLE_RUNTIME_RANGE_LOCK_COUNT,
                                                   sizeof(*table->range_locks));
    if (table->next_id_lock == NULL || table->data_lock == NULL ||
        table->index_lock == NULL || table->range_locks == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        goto fail;
    }

    if (pthread_mutex_init(table->next_id_lock, NULL) != 0) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: failed to initialize next-id lock");
        goto fail;
    }
    next_id_initialized = 1;

    if (pthread_rwlock_init(table->data_lock, NULL) != 0) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: failed to initialize data lock");
        goto fail;
    }
    data_initialized = 1;

    if (pthread_rwlock_init(table->index_lock, NULL) != 0) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: failed to initialize index lock");
        goto fail;
    }
    index_initialized = 1;

    while (initialized_range_count < TABLE_RUNTIME_RANGE_LOCK_COUNT) {
        if (pthread_mutex_init(&table->range_locks[initialized_range_count], NULL) != 0) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: failed to initialize range lock");
            goto fail;
        }
        initialized_range_count += 1U;
    }

    table->locks_initialized = 1;
    return STATUS_OK;

fail:
    while (initialized_range_count > 0U) {
        initialized_range_count -= 1U;
        pthread_mutex_destroy(&table->range_locks[initialized_range_count]);
    }
    if (index_initialized) {
        pthread_rwlock_destroy(table->index_lock);
    }
    if (data_initialized) {
        pthread_rwlock_destroy(table->data_lock);
    }
    if (next_id_initialized) {
        pthread_mutex_destroy(table->next_id_lock);
    }
    free(table->range_locks);
    free(table->index_lock);
    free(table->data_lock);
    free(table->next_id_lock);
    table->range_locks = NULL;
    table->index_lock = NULL;
    table->data_lock = NULL;
    table->next_id_lock = NULL;
    table->locks_initialized = 0;
    return STATUS_EXEC_ERROR;
}

/* TableRuntime이 소유한 pthread lock들을 해제한다. */
static void destroy_table_runtime_locks(TableRuntime *table)
{
    size_t i;

    if (table == NULL) {
        return;
    }

    if (table->locks_initialized) {
        if (table->range_locks != NULL) {
            for (i = 0U; i < TABLE_RUNTIME_RANGE_LOCK_COUNT; ++i) {
                pthread_mutex_destroy(&table->range_locks[i]);
            }
        }
        if (table->index_lock != NULL) {
            pthread_rwlock_destroy(table->index_lock);
        }
        if (table->data_lock != NULL) {
            pthread_rwlock_destroy(table->data_lock);
        }
        if (table->next_id_lock != NULL) {
            pthread_mutex_destroy(table->next_id_lock);
        }
    }

    free(table->range_locks);
    free(table->index_lock);
    free(table->data_lock);
    free(table->next_id_lock);
    table->range_locks = NULL;
    table->index_lock = NULL;
    table->data_lock = NULL;
    table->next_id_lock = NULL;
    table->locks_initialized = 0;
}

/* TableRuntime 하나가 소유한 heap 메모리와 lock 자원을 모두 해제한다. */
static void free_table_runtime_contents(TableRuntime *table)
{
    if (table == NULL) {
        return;
    }

    destroy_table_runtime_locks(table);
    free(table->table_name);
    free_table_schema(&table->schema);
    bptree_destroy(&table->id_index);
    memset(table, 0, sizeof(*table));
}

/* RuntimeRangeLockSet에 stripe index를 중복 없이 오름차순으로 추가한다. */
static int add_range_lock_index(RuntimeRangeLockSet *lock_set, size_t index)
{
    size_t position = 0U;
    size_t i;

    if (lock_set == NULL || index >= TABLE_RUNTIME_RANGE_LOCK_COUNT) {
        return 0;
    }

    while (position < lock_set->count && lock_set->indices[position] < index) {
        position += 1U;
    }
    if (position < lock_set->count && lock_set->indices[position] == index) {
        return 1;
    }
    if (lock_set->count >= TABLE_RUNTIME_RANGE_LOCK_COUNT) {
        return 0;
    }

    for (i = lock_set->count; i > position; --i) {
        lock_set->indices[i] = lock_set->indices[i - 1U];
    }
    lock_set->indices[position] = index;
    lock_set->count += 1U;
    return 1;
}

/* text가 leading zero 없는 양의 uint64 정수인지 검사하고 out_value에 파싱 결과를 넣는다. */
static int parse_canonical_positive_uint64(const char *text, uint64_t *out_value)
{
    size_t i;
    uint64_t value = 0U;

    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return 0;
    }

    if (text[0] == '0') {
        return 0;
    }

    for (i = 0U; text[i] != '\0'; ++i) {
        unsigned int digit;

        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }

        digit = (unsigned int)(text[i] - '0');
        if (value > (UINT64_MAX - (uint64_t)digit) / 10U) {
            return 0;
        }

        value = (value * 10U) + (uint64_t)digit;
    }

    if (value == 0U) {
        return 0;
    }

    *out_value = value;
    return 1;
}

/* 저장된 id 문자열 text를 검증해 out_id에 숫자로 변환하고 상태 코드를 반환한다. */
int parse_stored_id_value(const char *text,
                          uint64_t *out_id,
                          char *errbuf, size_t errbuf_size)
{
    if (!parse_canonical_positive_uint64(text, out_id)) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: invalid stored id '%s'", text == NULL ? "" : text);
        return STATUS_INDEX_ERROR;
    }

    return STATUS_OK;
}

/* WHERE literal이 인덱스에 사용할 수 있는 canonical positive integer면 out_id를 채운다. */
int try_parse_indexable_id_literal(const LiteralValue *literal,
                                   uint64_t *out_id)
{
    uint64_t value = 0U;

    if (out_id != NULL) {
        *out_id = 0U;
    }

    if (literal == NULL || out_id == NULL || literal->text == NULL) {
        return 0;
    }

    if (!parse_canonical_positive_uint64(literal->text, &value)) {
        return 0;
    }

    *out_id = value;
    return 1;
}

/* scan_table_rows_with_offsets 콜백으로 호출돼 row의 id를 tree에 삽입하며 최대 id를 추적한다. */
static int build_id_index_callback(const Row *row,
                                   long row_offset,
                                   void *user_data,
                                   char *errbuf,
                                   size_t errbuf_size)
{
    IdIndexBuildState *state = (IdIndexBuildState *)user_data;
    uint64_t current_id = 0U;
    int status;

    if (row == NULL || state == NULL || state->tree == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: invalid index build callback arguments");
        return -1;
    }

    if (state->id_column_index < 0 ||
        (size_t)state->id_column_index >= row->value_count ||
        row->values[state->id_column_index] == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: missing stored id value");
        return -1;
    }

    status = parse_stored_id_value(row->values[state->id_column_index], &current_id,
                                   errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return -1;
    }

    status = bptree_insert(state->tree, current_id, row_offset, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        if (errbuf != NULL && errbuf_size > 0U && errbuf[0] == '\0') {
            set_error(errbuf, errbuf_size, "INDEX ERROR: duplicate stored id %" PRIu64, current_id);
        }
        return -1;
    }

    if (!state->saw_row || current_id > state->max_id) {
        state->max_id = current_id;
    }
    state->saw_row = 1;
    return 0;
}

/* 기존 data 파일을 스캔해 id 인덱스를 재구성하고 다음 auto id를 계산한다. */
int build_id_index_for_table(const char *db_dir,
                             const TableSchema *schema,
                             int id_column_index,
                             BPTree *out_tree,
                             uint64_t *out_next_id,
                             char *errbuf, size_t errbuf_size)
{
    IdIndexBuildState state;
    int status;

    if (db_dir == NULL || schema == NULL || out_tree == NULL || out_next_id == NULL || id_column_index < 0) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: invalid table index build arguments");
        return STATUS_INDEX_ERROR;
    }

    status = bptree_init(out_tree, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    state.id_column_index = id_column_index;
    state.tree = out_tree;
    state.max_id = 0U;
    state.saw_row = 0;

    status = scan_table_rows_with_offsets(db_dir, schema->table_name, schema->column_count,
                                          build_id_index_callback, &state,
                                          errbuf, errbuf_size);
    if (status != STATUS_OK) {
        bptree_destroy(out_tree);
        return STATUS_INDEX_ERROR;
    }

    if (!state.saw_row) {
        *out_next_id = 1U;
        return STATUS_OK;
    }

    if (state.max_id == UINT64_MAX) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: next id would overflow");
        bptree_destroy(out_tree);
        return STATUS_INDEX_ERROR;
    }

    *out_next_id = state.max_id + 1U;
    return STATUS_OK;
}

/* db_dir 기준으로 out_ctx를 빈 runtime cache 상태로 초기화한다. */
int init_execution_context(const char *db_dir,
                           ExecutionContext *out_ctx,
                           char *errbuf, size_t errbuf_size)
{
    if (out_ctx == NULL || db_dir == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid execution context arguments");
        return STATUS_EXEC_ERROR;
    }

    memset(out_ctx, 0, sizeof(*out_ctx));
    out_ctx->db_dir = dup_string(db_dir);
    if (out_ctx->db_dir == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }

    if (errbuf != NULL && errbuf_size > 0U) {
        errbuf[0] = '\0';
    }
    return STATUS_OK;
}

/* ctx에서 table_name runtime을 찾아 재사용하거나 schema/index를 새로 로드해 out_table에 넘긴다. */
int get_or_load_table_runtime(ExecutionContext *ctx,
                              const char *table_name,
                              TableRuntime **out_table,
                              char *errbuf, size_t errbuf_size)
{
    TableRuntime table = {0};
    TableRuntime *grown;
    size_t i;
    int status;

    if (ctx == NULL || table_name == NULL || out_table == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid table runtime arguments");
        return STATUS_EXEC_ERROR;
    }

    for (i = 0U; i < ctx->table_count; ++i) {
        if (strcmp(ctx->tables[i].table_name, table_name) == 0) {
            *out_table = &ctx->tables[i];
            return STATUS_OK;
        }
    }

    table.table_name = dup_string(table_name);
    if (table.table_name == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }

    status = load_table_schema(ctx->db_dir, table_name, &table.schema, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free(table.table_name);
        return STATUS_SCHEMA_ERROR;
    }

    status = ensure_table_data_file(ctx->db_dir, table_name, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_table_schema(&table.schema);
        free(table.table_name);
        return STATUS_STORAGE_ERROR;
    }

    table.id_column_index = schema_find_column_index(&table.schema, "id");
    table.has_id_column = table.id_column_index >= 0 ? 1 : 0;

    status = bptree_init(&table.id_index, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_table_schema(&table.schema);
        free(table.table_name);
        return STATUS_INDEX_ERROR;
    }

    if (table.has_id_column) {
        status = build_id_index_for_table(ctx->db_dir, &table.schema, table.id_column_index,
                                          &table.id_index, &table.next_id,
                                          errbuf, errbuf_size);
        if (status != STATUS_OK) {
            bptree_destroy(&table.id_index);
            free_table_schema(&table.schema);
            free(table.table_name);
            return STATUS_INDEX_ERROR;
        }
        table.id_index_ready = 1;
    }

    if (ctx->table_count == ctx->table_capacity) {
        size_t new_capacity = ctx->table_capacity == 0U ? 4U : ctx->table_capacity * 2U;

        grown = (TableRuntime *)realloc(ctx->tables, new_capacity * sizeof(TableRuntime));
        if (grown == NULL) {
            bptree_destroy(&table.id_index);
            free_table_schema(&table.schema);
            free(table.table_name);
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            return STATUS_EXEC_ERROR;
        }

        ctx->tables = grown;
        ctx->table_capacity = new_capacity;
    }

    ctx->tables[ctx->table_count] = table;
    status = init_table_runtime_locks(&ctx->tables[ctx->table_count], errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_table_runtime_contents(&ctx->tables[ctx->table_count]);
        return status;
    }

    *out_table = &ctx->tables[ctx->table_count];
    ctx->table_count += 1U;
    return STATUS_OK;
}

/* SELECT read lock 실행 전에 write lock 상태에서 table runtime을 미리 로드한다. */
int runtime_preload_table(ExecutionContext *ctx,
                          const char *table_name,
                          char *errbuf,
                          size_t errbuf_size)
{
    TableRuntime *table = NULL;

    return get_or_load_table_runtime(ctx, table_name, &table, errbuf, errbuf_size);
}

/* id 기준 앞뒤 window 범위에 대응하는 stripe lock들을 deadlock 없이 잡는다. */
int table_runtime_lock_id_window(TableRuntime *table,
                                 uint64_t id,
                                 uint64_t window,
                                 RuntimeRangeLockSet *lock_set,
                                 char *errbuf,
                                 size_t errbuf_size)
{
    uint64_t start_id;
    uint64_t end_id;
    uint64_t current_id;
    size_t i;

    if (table == NULL || lock_set == NULL || !table->locks_initialized || table->range_locks == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid range lock arguments");
        return STATUS_EXEC_ERROR;
    }

    memset(lock_set, 0, sizeof(*lock_set));
    if (id == 0U) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid range lock id");
        return STATUS_EXEC_ERROR;
    }

    start_id = id > window ? id - window : 1U;
    end_id = UINT64_MAX - id < window ? UINT64_MAX : id + window;

    if (window >= TABLE_RUNTIME_RANGE_LOCK_COUNT) {
        for (i = 0U; i < TABLE_RUNTIME_RANGE_LOCK_COUNT; ++i) {
            if (!add_range_lock_index(lock_set, i)) {
                set_error(errbuf, errbuf_size, "EXEC ERROR: failed to build range lock set");
                return STATUS_EXEC_ERROR;
            }
        }
    } else {
        current_id = start_id;
        while (1) {
            size_t index = (size_t)(current_id % TABLE_RUNTIME_RANGE_LOCK_COUNT);

            if (!add_range_lock_index(lock_set, index)) {
                set_error(errbuf, errbuf_size, "EXEC ERROR: failed to build range lock set");
                return STATUS_EXEC_ERROR;
            }
            if (current_id == end_id) {
                break;
            }
            current_id += 1U;
        }
    }

    for (i = 0U; i < lock_set->count; ++i) {
        if (pthread_mutex_lock(&table->range_locks[lock_set->indices[i]]) != 0) {
            size_t unlock_index;

            for (unlock_index = i; unlock_index > 0U; --unlock_index) {
                pthread_mutex_unlock(&table->range_locks[lock_set->indices[unlock_index - 1U]]);
            }
            memset(lock_set, 0, sizeof(*lock_set));
            set_error(errbuf, errbuf_size, "EXEC ERROR: failed to acquire range lock");
            return STATUS_EXEC_ERROR;
        }
    }

    return STATUS_OK;
}

/* table_runtime_lock_id_window()로 잡은 stripe lock들을 역순으로 해제한다. */
void table_runtime_unlock_id_window(TableRuntime *table,
                                    const RuntimeRangeLockSet *lock_set)
{
    size_t i;

    if (table == NULL || lock_set == NULL || table->range_locks == NULL) {
        return;
    }

    for (i = lock_set->count; i > 0U; --i) {
        pthread_mutex_unlock(&table->range_locks[lock_set->indices[i - 1U]]);
    }
}

/* ctx가 보유한 모든 table runtime과 그 안의 schema/B+Tree 메모리를 해제한다. */
void free_execution_context(ExecutionContext *ctx)
{
    size_t i;

    if (ctx == NULL) {
        return;
    }

    for (i = 0U; i < ctx->table_count; ++i) {
        free_table_runtime_contents(&ctx->tables[i]);
    }

    free(ctx->tables);
    free(ctx->db_dir);
    ctx->tables = NULL;
    ctx->table_count = 0U;
    ctx->table_capacity = 0U;
    ctx->db_dir = NULL;
}
