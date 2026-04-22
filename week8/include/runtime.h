#ifndef RUNTIME_H
#define RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "ast.h"
#include "bptree.h"
#include "schema.h"

#define TABLE_RUNTIME_RANGE_LOCK_COUNT 64U
#define TABLE_RUNTIME_INSERT_RANGE_WINDOW 5U

typedef struct {
    size_t indices[TABLE_RUNTIME_RANGE_LOCK_COUNT];
    size_t count;
} RuntimeRangeLockSet;

/* 실행 중 한 테이블의 schema, next_id, 인덱스 캐시를 보관하는 runtime 엔트리다. */
typedef struct {
    char *table_name;      /* 캐시가 가리키는 테이블 이름이다. */
    TableSchema schema;    /* 한 번 읽어 온 schema 캐시다. */
    int has_id_column;     /* schema에 정확히 'id' 컬럼이 있는지 여부다. */
    int id_column_index;   /* 'id' 컬럼의 schema index다. */
    int id_index_ready;    /* B+Tree 인덱스 빌드가 완료됐는지 나타낸다. */
    uint64_t next_id;      /* 다음 INSERT에 부여할 auto-generated id다. */
    BPTree id_index;       /* id -> row_offset 매핑을 담는 B+Tree다. */
    int locks_initialized;
    pthread_mutex_t *next_id_lock;
    pthread_rwlock_t *data_lock;
    pthread_rwlock_t *index_lock;
    pthread_mutex_t *range_locks;
} TableRuntime;

/* 한 실행 동안 재사용되는 DB 경로와 테이블 캐시 배열을 담는 컨텍스트다. */
typedef struct {
    char *db_dir;            /* 현재 실행이 바라보는 DB 디렉터리 경로다. */
    TableRuntime *tables;    /* 로드된 테이블 runtime 배열이다. */
    size_t table_count;      /* 실제로 로드된 테이블 수다. */
    size_t table_capacity;   /* tables 배열 할당 용량이다. */
} ExecutionContext;

/* db_dir를 기준으로 out_ctx를 빈 runtime cache 상태로 초기화하고 상태를 반환한다. */
int init_execution_context(const char *db_dir,
                           ExecutionContext *out_ctx,
                           char *errbuf, size_t errbuf_size);

/*
 * ctx에서 table_name runtime을 찾거나 새로 로드해 out_table에 넘기고,
 * schema 로드/인덱스 빌드 결과를 STATUS 코드로 반환한다.
 */
int get_or_load_table_runtime(ExecutionContext *ctx,
                              const char *table_name,
                              TableRuntime **out_table,
                              char *errbuf, size_t errbuf_size);

/*
 * SELECT 실행 전 runtime cache를 명시적으로 준비하는 public wrapper다.
 * 호출자는 반드시 write lock을 잡은 상태에서 이 함수를 호출해야 한다.
 */
int runtime_preload_table(ExecutionContext *ctx,
                          const char *table_name,
                          char *errbuf,
                          size_t errbuf_size);

int table_runtime_lock_id_window(TableRuntime *table,
                                 uint64_t id,
                                 uint64_t window,
                                 RuntimeRangeLockSet *lock_set,
                                 char *errbuf,
                                 size_t errbuf_size);
void table_runtime_unlock_id_window(TableRuntime *table,
                                    const RuntimeRangeLockSet *lock_set);

/*
 * schema와 id_column_index를 기준으로 data 파일을 스캔해 out_tree를 채우고,
 * 다음 auto-generated id를 out_next_id에 계산해 넣은 뒤 상태 코드를 반환한다.
 */
int build_id_index_for_table(const char *db_dir,
                             const TableSchema *schema,
                             int id_column_index,
                             BPTree *out_tree,
                             uint64_t *out_next_id,
                             char *errbuf, size_t errbuf_size);

/* text가 저장된 canonical positive integer id인지 검증하고 out_id에 숫자로 변환한다. */
int parse_stored_id_value(const char *text,
                          uint64_t *out_id,
                          char *errbuf, size_t errbuf_size);

/* literal이 인덱스 조회에 안전한 canonical integer면 out_id를 채우고 1을 반환한다. */
int try_parse_indexable_id_literal(const LiteralValue *literal,
                                   uint64_t *out_id);

/* ExecutionContext가 소유한 모든 테이블 캐시와 인덱스를 해제한다. */
void free_execution_context(ExecutionContext *ctx);

#endif
