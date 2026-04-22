기준 베이스라인은 현재 **C 기반 파일형 미니 SQL 처리기**이고, `.schema` / `.data` 파일을 직접 읽고 쓰며 `Lexer -> Parser -> Executor -> Storage` 흐름은 이미 구현되어 있지만, **B+Tree / 인덱스 / 페이지 관리**는 아직 없는 상태입니다. 이번 작업은 그 코드베이스를 유지한 채, **id 자동 부여 + 메모리 기반 B+Tree 인덱스 + 성능 비교**를 추가하는 확장 작업으로 정의하면 됩니다. 

아래 문서는 그대로 Codex 작업 명세로 넘겨도 되는 형태로 정리했습니다.

# Codex 작업 지시용 요구사항 명세서

## 1. 이번 작업의 최종 목표

기존 SQL 처리기를 유지하면서 다음 4가지를 추가한다.

1. `id` 컬럼이 있는 테이블에 `INSERT`가 들어오면 `id`를 자동 생성한다.
2. 생성된 `id`를 키로 하는 **메모리 기반 B+Tree 인덱스**를 만든다.
3. `SELECT ... WHERE id = ?` 형태의 조회는 **반드시 B+Tree 인덱스**를 사용한다.
4. 다른 컬럼 기준 조회는 기존처럼 **선형 탐색**으로 수행하고, 1,000,000건 이상 데이터에서 둘의 성능 차이를 비교한다.

## 2. 절대 유지해야 하는 기존 제약

이번 작업은 “기존 프로젝트를 갈아엎는 것”이 아니라 “기존 파이프라인 위에 인덱스를 얹는 것”이다. 따라서 아래는 유지한다.

* 저장 포맷은 그대로 유지한다.
  각 테이블은 `<table>.schema`, `<table>.data` 두 파일이다.
* `.data` 파일 escape 규칙은 그대로 유지한다.
* SQL 문법은 원칙적으로 늘리지 않는다.
* `WHERE`는 여전히 **단일 `column = literal`** 조건만 지원한다.
* `SELECT` 결과 출력 형식과 `INSERT 1` 출력 형식은 유지한다.
* 인덱스는 **디스크 영속화하지 않는다**. 프로세스 메모리 안에서만 유지한다.
* 외부 라이브러리 없이 **순수 C**로 구현한다.

## 3. 가장 중요한 설계 결정

### 3-1. 인덱스는 “row 전체”가 아니라 “row의 파일 오프셋”을 가리킨다

B+Tree leaf에 저장할 값은 `id -> row_offset` 매핑이다.

이렇게 해야 하는 이유는 두 가지다.

* B+Tree가 실제 DB 인덱스처럼 “키 -> 실제 저장 위치” 역할을 하게 된다.
* `WHERE id = ?` 조회 시 전체 파일을 다 읽지 않고, 파일의 특정 위치만 바로 읽어올 수 있다.

즉 leaf value는 `Row*`가 아니라 **`.data` 파일 안에서 그 row가 시작하는 바이트 위치**여야 한다.

### 3-2. 인덱스는 처음 테이블을 접근할 때 1회 빌드하고, 이후에는 캐시한다

한 SQL 파일 안에 여러 statement가 있을 수 있으므로, statement마다 `.data`를 다시 스캔하면 성능이 무너진다.
따라서 실행 중에는 **테이블별 runtime cache**를 둬야 한다.

### 3-3. `id` 컬럼이 있는 테이블만 인덱스를 만든다

스키마에 정확히 `id`라는 컬럼명이 있으면 indexed table로 간주한다.
`id`가 없는 테이블은 기존 동작을 그대로 유지한다.

## 4. 새로 추가하거나 변경해야 하는 핵심 자료구조

### 4-1. `ExecResult` 확장

기존 `ExecResult`에 아래 메타데이터를 추가한다.

```c
typedef struct {
    ExecResultType type;
    size_t affected_rows;
    QueryResult query_result;

    int used_index;          // SELECT에서만 의미 있음. 1이면 B+Tree 사용
    int has_generated_id;    // INSERT에서만 의미 있음
    uint64_t generated_id;   // 자동 생성된 id
} ExecResult;
```

설명:

* `used_index`는 테스트에서 “정말 인덱스를 탔는지” 검증하기 위해 필요하다.
* `generated_id`는 CLI 출력에 꼭 노출할 필요는 없지만, 테스트와 벤치마크에서 매우 유용하다.

### 4-2. `ExecutionContext`

statement 사이에 테이블 인덱스를 재사용하기 위한 실행 컨텍스트를 도입한다.

```c
typedef struct {
    char *table_name;
    TableSchema schema;
    int has_id_column;
    int id_column_index;

    int id_index_ready;   // 1이면 B+Tree 빌드 완료
    uint64_t next_id;     // 다음 INSERT에 부여할 id

    BPTree id_index;
} TableRuntime;

typedef struct {
    char *db_dir;
    TableRuntime *tables;
    size_t table_count;
    size_t table_capacity;
} ExecutionContext;
```

설명:

* `TableRuntime`는 “한 테이블의 실행 시점 메타데이터”다.
* `next_id`는 해당 테이블에서 다음에 생성할 id다.
* `ExecutionContext`는 한 번의 프로그램 실행 동안 살아 있는 캐시다.

### 4-3. `BPTree`

```c
#define BPTREE_ORDER 64
#define BPTREE_MAX_KEYS (BPTREE_ORDER - 1)
#define BPTREE_MAX_CHILDREN (BPTREE_ORDER)

typedef struct BPTreeNode {
    int is_leaf;
    size_t key_count;
    uint64_t keys[BPTREE_MAX_KEYS];

    struct BPTreeNode *parent;
    struct BPTreeNode *next;  // leaf chain

    union {
        struct BPTreeNode *children[BPTREE_MAX_CHILDREN];
        long row_offsets[BPTREE_MAX_KEYS];
    } ptrs;
} BPTreeNode;

typedef struct {
    BPTreeNode *root;
    size_t key_count;
} BPTree;
```

설명:

* key는 `uint64_t id`
* leaf value는 `long row_offset`
* `next` 포인터는 leaf chain 유지용이다. 이번 과제는 equality lookup만 쓰더라도 B+Tree답게 구현한다.

## 5. 단계별 구현 명세

---

## 단계 1. 실행 컨텍스트 도입

### 목적

statement마다 인덱스를 다시 만들지 않도록, 실행 중 테이블 상태를 캐싱한다.

### 수정 파일

* `src/main.c`
* `src/executor.c`
* 신규: `src/runtime.c`, `include/runtime.h` 또는 이에 준하는 파일

### 공개 함수

```c
int init_execution_context(
    const char *db_dir,
    ExecutionContext *out_ctx,
    char *errbuf,
    size_t errbuf_size
);

int get_or_load_table_runtime(
    ExecutionContext *ctx,
    const char *table_name,
    TableRuntime **out_table,
    char *errbuf,
    size_t errbuf_size
);

void free_execution_context(ExecutionContext *ctx);
```

### 함수별 상세

#### `init_execution_context`

입력:

* `db_dir`: DB 디렉터리 경로
* `out_ctx`: 초기화할 컨텍스트 구조체
* `errbuf`, `errbuf_size`: 에러 메시지 버퍼

연산:

* `db_dir` 복사
* 테이블 캐시 배열 초기화
* `out_ctx`를 사용 가능한 빈 상태로 만든다

반환:

* 성공 시 `STATUS_OK`
* 메모리 할당 실패나 내부 초기화 실패 시 적절한 에러 코드

#### `get_or_load_table_runtime`

입력:

* `ctx`
* `table_name`
* `out_table`
* `errbuf`, `errbuf_size`

연산:

1. 캐시에서 같은 테이블이 이미 로드되어 있으면 그대로 반환
2. 없으면 schema 로드
3. `.data` 파일이 없으면 생성
4. schema에 `id` 컬럼이 있으면 인덱스를 빌드
5. `next_id` 계산
6. 캐시에 저장 후 포인터 반환

반환:

* 성공 시 `STATUS_OK`
* schema 문제면 `STATUS_SCHEMA_ERROR`
* 파일 문제면 `STATUS_STORAGE_ERROR` 또는 `STATUS_FILE_ERROR`
* 인덱스 빌드 문제면 `STATUS_INDEX_ERROR` 권장

소유권:

* `*out_table`은 `ctx`가 소유한다. caller가 free하면 안 된다.

#### `free_execution_context`

연산:

* 모든 `TableRuntime`의 schema, tree, table_name 메모리 해제
* `ctx->tables`, `ctx->db_dir` 해제

### 기존 함수 시그니처 변경

`executor.c`의 기존 함수는 아래처럼 바꾼다.

```c
int execute_statement(
    ExecutionContext *ctx,
    const Statement *stmt,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);
```

### `main.c` 변경 요구

실행 흐름을 아래처럼 바꾼다.

```c
parse_cli_args(...)
read_text_file(...)
tokenize_sql(...)

init_execution_context(db_dir, &ctx, ...)

while (parse_next_statement(...)) {
    execute_statement(&ctx, &stmt, &result, ...)
    print_exec_result(&result)
    free_statement(&stmt)
    free_exec_result(&result)
}

free_execution_context(&ctx)
```

### 완료 기준

* 한 SQL 파일 안에서 같은 테이블을 여러 번 조회/삽입해도 인덱스를 재빌드하지 않는다.
* `main.c`에서 global state 없이 context를 통해서만 런타임 상태를 관리한다.

---

## 단계 2. B+Tree 모듈 구현

### 목적

`id -> row_offset`를 저장하는 순수 메모리 기반 B+Tree를 구현한다.

### 수정 파일

* 신규: `src/bptree.c`, `include/bptree.h`

### 공개 함수

```c
int bptree_init(
    BPTree *out_tree,
    char *errbuf,
    size_t errbuf_size
);

int bptree_search(
    const BPTree *tree,
    uint64_t key,
    long *out_offset,
    int *out_found,
    char *errbuf,
    size_t errbuf_size
);

int bptree_insert(
    BPTree *tree,
    uint64_t key,
    long row_offset,
    char *errbuf,
    size_t errbuf_size
);

int bptree_validate(
    const BPTree *tree,
    char *errbuf,
    size_t errbuf_size
);

void bptree_destroy(BPTree *tree);
```

### 함수별 상세

#### `bptree_init`

입력:

* `out_tree`

연산:

* 빈 tree 초기화
* `root = NULL`
* `key_count = 0`

반환:

* 성공 시 `STATUS_OK`

#### `bptree_search`

입력:

* `tree`
* `key`
* `out_offset`
* `out_found`

연산:

* root부터 내려가 해당 key가 있어야 할 leaf를 찾는다
* key가 있으면 해당 leaf의 `row_offset` 반환

반환:

* 성공 시 `STATUS_OK`
* `*out_found = 1`이면 검색 성공
* `*out_found = 0`이면 해당 key 없음
* tree 구조 이상이면 `STATUS_INDEX_ERROR`

#### `bptree_insert`

입력:

* `tree`
* `key`
* `row_offset`

연산:

* 적절한 leaf에 key를 정렬 상태로 삽입
* leaf overflow 시 split
* separator key를 parent에 전파
* internal overflow 시 split 반복
* root split까지 처리

반환:

* 성공 시 `STATUS_OK`
* duplicate key면 `STATUS_INDEX_ERROR`
* 메모리 할당 실패 등도 `STATUS_INDEX_ERROR`

#### `bptree_validate`

연산:

* 테스트용 검증 함수
* 모든 leaf 깊이가 동일한지
* leaf chain이 정렬 상태인지
* internal separator가 자식 범위를 올바르게 나누는지
* key_count가 음수/overflow 없는지 검사

반환:

* 정상 구조면 `STATUS_OK`
* 이상이 있으면 `STATUS_INDEX_ERROR`

#### `bptree_destroy`

연산:

* 전체 node 재귀 해제

### 내부 static helper 권장 함수

```c
static BPTreeNode *create_node(int is_leaf);
static BPTreeNode *find_leaf_node(BPTreeNode *root, uint64_t key);
static int insert_into_leaf(BPTreeNode *leaf, uint64_t key, long row_offset);
static int split_leaf_and_insert(BPTree *tree, BPTreeNode *leaf, uint64_t key, long row_offset, char *errbuf, size_t errbuf_size);
static int insert_into_parent(BPTree *tree, BPTreeNode *left, uint64_t separator_key, BPTreeNode *right, char *errbuf, size_t errbuf_size);
static int split_internal_and_insert(BPTree *tree, BPTreeNode *node, uint64_t separator_key, BPTreeNode *right_child, char *errbuf, size_t errbuf_size);
```

### 완료 기준

* 순차 insert 1,000,000건에서도 search가 정상 동작해야 한다.
* duplicate key를 허용하지 않는다.
* root split, internal split, leaf chain이 모두 테스트로 검증된다.

---

## 단계 3. storage 계층에 “offset 기반 접근” 추가

### 목적

인덱스가 가리키는 row를 파일에서 바로 읽을 수 있게 만든다.
또한 인덱스 빌드 시 전체 row를 메모리에 다 올리지 않고 스트리밍으로 스캔할 수 있게 만든다.

### 수정 파일

* `src/storage.c`
* `include/storage.h`

### 신규/변경 함수

```c
typedef int (*RowScanCallback)(
    const Row *row,
    long row_offset,
    void *user_data,
    char *errbuf,
    size_t errbuf_size
);
/*
callback 반환 규칙:
0  -> 계속 스캔
1  -> 조기 종료(정상)
-1 -> 에러
*/

int append_row_to_table_with_offset(
    const char *db_dir,
    const char *table_name,
    const Row *row,
    long *out_row_offset,
    char *errbuf,
    size_t errbuf_size
);

int scan_table_rows_with_offsets(
    const char *db_dir,
    const char *table_name,
    size_t expected_column_count,
    RowScanCallback callback,
    void *user_data,
    char *errbuf,
    size_t errbuf_size
);

int read_row_at_offset(
    const char *db_dir,
    const char *table_name,
    long row_offset,
    size_t expected_column_count,
    Row *out_row,
    char *errbuf,
    size_t errbuf_size
);

void free_row(Row *row);
```

### 함수별 상세

#### `append_row_to_table_with_offset`

입력:

* `db_dir`, `table_name`
* `row`
* `out_row_offset`

연산:

* `.data` 파일을 append 모드로 연다
* write 직전 `ftell()` 위치를 저장한다
* 기존 escape 규칙으로 row를 직렬화해 한 줄 append한다
* append 시작 위치를 `*out_row_offset`에 저장한다

반환:

* 성공 시 `STATUS_OK`
* 파일 열기/쓰기 실패 시 `STATUS_STORAGE_ERROR`

#### `scan_table_rows_with_offsets`

입력:

* `db_dir`, `table_name`
* `expected_column_count`
* `callback`
* `user_data`

연산:

* `.data` 파일을 처음부터 한 줄씩 읽는다
* 각 line 시작 offset을 기억한다
* 기존 unescape/split/normalize 로직으로 `Row`를 복원한다
* `callback(row, offset, user_data, ...)`를 호출한다
* callback이 0이면 계속, 1이면 정상 중단, -1이면 에러

반환:

* 스캔 성공 또는 callback 정상 중단이면 `STATUS_OK`
* 파일/파싱 문제면 `STATUS_STORAGE_ERROR`

메모리 규칙:

* callback에 전달한 `Row`는 callback 종료 후 storage가 해제해도 된다.
* callback이 row를 계속 들고 있어야 한다면 deep-copy해야 한다.

#### `read_row_at_offset`

입력:

* `db_dir`, `table_name`
* `row_offset`
* `expected_column_count`
* `out_row`

연산:

* 파일을 열고 `fseek`로 `row_offset`으로 이동
* 그 위치의 한 line만 읽어 `Row`로 복원
* normalize 수행

반환:

* 성공 시 `STATUS_OK`
* seek/read 실패 시 `STATUS_STORAGE_ERROR`

#### `free_row`

연산:

* `Row.values[i]`와 `Row.values` 해제
* 단일 row 해제용 helper

### 추가 요구

기존 `read_all_rows_from_table()`은 남겨도 되지만, 내부를 `scan_table_rows_with_offsets()` 기반으로 재구성해도 된다.

### 완료 기준

* 인덱스 빌드 시 1,000,000 rows를 한 번에 메모리에 다 올리지 않는다.
* `WHERE id = ?` 조회 시 한 row만 offset으로 읽어오는 경로가 존재한다.

---

## 단계 4. 테이블 인덱스 빌드 로직 구현

### 목적

기존 `.data` 파일에 이미 들어 있는 row들로부터 `id` 인덱스를 1회 빌드하고, 다음 자동 id 값을 계산한다.

### 수정 파일

* `src/runtime.c` 또는 신규 `src/index_runtime.c`

### 권장 함수

```c
int build_id_index_for_table(
    const char *db_dir,
    const TableSchema *schema,
    int id_column_index,
    BPTree *out_tree,
    uint64_t *out_next_id,
    char *errbuf,
    size_t errbuf_size
);

int parse_stored_id_value(
    const char *text,
    uint64_t *out_id,
    char *errbuf,
    size_t errbuf_size
);

int try_parse_indexable_id_literal(
    const LiteralValue *literal,
    uint64_t *out_id
);
```

### 함수별 상세

#### `build_id_index_for_table`

입력:

* `db_dir`
* `schema`
* `id_column_index`
* `out_tree`
* `out_next_id`

연산:

1. 빈 B+Tree 초기화
2. `.data` 전체를 streaming scan
3. 각 row의 `id` 컬럼 값을 읽음
4. `parse_stored_id_value()`로 검증
5. `bptree_insert(id, row_offset)` 수행
6. 최대 id 추적
7. 스캔이 끝나면 `next_id = max_id + 1`
8. row가 하나도 없으면 `next_id = 1`

반환:

* 성공 시 `STATUS_OK`
* duplicate / malformed id / empty id / negative / overflow면 `STATUS_INDEX_ERROR`

#### `parse_stored_id_value`

입력:

* `.data` 파일에 저장된 id 텍스트

연산:

* 빈 문자열 금지
* 10진수 정수인지 확인
* 음수 금지
* leading zero 금지 (`"0"` 자체도 허용하지 않는 편 권장, auto id는 1부터 시작)
* `uint64_t` overflow 검사

반환:

* 정상 파싱 시 `STATUS_OK`
* 아니면 `STATUS_INDEX_ERROR`

#### `try_parse_indexable_id_literal`

입력:

* `WHERE id = literal`의 literal

연산:

* 이 literal이 “인덱스로 안전하게 조회 가능한 canonical integer”인지 검사
* 예: `1`, `'1'`은 허용
* `001`, `'001'`, `1.0`, `-1`은 인덱스 사용 대상 아님

반환:

* 사용 가능하면 `1` 반환하고 `*out_id` 채움
* 사용 불가면 `0`
* 이 함수는 에러를 내지 않고 “인덱스 사용 여부 판단”만 한다

### 중요한 규칙

* 기존 SQL 의미를 바꾸면 안 되므로, canonical integer가 아닌 `WHERE id = '001'`은 인덱스를 쓰지 말고 full scan으로 내려가야 한다.
* 그래야 기존 string equality semantics와 어긋나지 않는다.

### 완료 기준

* 기존 데이터 파일만 있어도 프로그램 시작 후 id index를 복원할 수 있어야 한다.
* `next_id`는 항상 “현재 최대 id + 1”이어야 한다.

---

## 단계 5. INSERT 경로를 자동 id 방식으로 변경

### 목적

기존 `id 중복 검사` 방식에서 벗어나, `id 자동 생성 + 즉시 B+Tree 삽입` 방식으로 바꾼다.

### 수정 파일

* `src/executor.c`

### 권장 함수

```c
static int execute_insert(
    ExecutionContext *ctx,
    const InsertStatement *stmt,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);

static int validate_insert_columns_for_auto_id(
    const TableRuntime *table,
    const InsertStatement *stmt,
    char *errbuf,
    size_t errbuf_size
);

static int build_insert_row_with_generated_id(
    const TableRuntime *table,
    const InsertStatement *stmt,
    uint64_t generated_id,
    Row *out_row,
    char *errbuf,
    size_t errbuf_size
);
```

### 함수별 상세

#### `validate_insert_columns_for_auto_id`

입력:

* `table`
* `stmt`

연산:

* table에 `id` 컬럼이 없으면 기존 규칙 유지
* table에 `id` 컬럼이 있으면 아래 규칙 검사

규칙:

1. `INSERT INTO table VALUES (...)`

   * 입력 value 개수는 `schema.column_count - 1` 이어야 한다
   * schema 순서대로 non-id 컬럼에만 값을 채운다
2. `INSERT INTO table (col1, col2, ...) VALUES (...)`

   * `id`는 column list에 들어오면 안 된다
   * 중복 column은 에러
   * 없는 컬럼은 에러
   * 주어진 컬럼 외 나머지 non-id 컬럼은 빈 문자열 `""`
3. `id`를 사용자가 직접 넣는 시나리오는 이번 과제 범위에서 금지한다

반환:

* 성공 시 `STATUS_OK`
* 위반 시 `STATUS_EXEC_ERROR`

#### `build_insert_row_with_generated_id`

입력:

* `table`
* `stmt`
* `generated_id`
* `out_row`

연산:

* 최종 row를 schema 길이에 맞게 만든다
* `id` 위치에는 `generated_id`를 문자열로 넣는다
* 나머지는 기존 규칙대로 채운다

반환:

* 성공 시 `STATUS_OK`

메모리 소유권:

* `out_row` 내부는 heap 할당
* caller가 `free_row()`로 해제

#### `execute_insert`

연산 순서:

1. `get_or_load_table_runtime()`로 table runtime 확보
2. validation 수행
3. `generated_id = table->next_id`
4. `build_insert_row_with_generated_id()`
5. `append_row_to_table_with_offset()` 호출
6. `bptree_insert(generated_id, row_offset)` 호출
7. 성공 시 `table->next_id++`
8. `ExecResult`에 `affected_rows=1`, `has_generated_id=1`, `generated_id=...`

반환:

* 성공 시 `STATUS_OK`
* 실패 시 적절한 error code

### 예시 동작

schema:

```text
id
name
age
```

입력:

```sql
INSERT INTO users VALUES ('Alice', '20');
INSERT INTO users (age, name) VALUES ('21', 'Bob');
```

파일에 실제로 들어가는 row:

```text
1|Alice|20
2|Bob|21
```

### 완료 기준

* `id` 있는 테이블에 INSERT 하면 항상 자동 id가 부여된다.
* insert 직후 B+Tree에도 동일 id가 등록된다.
* 같은 실행 안에서는 `next_id`가 계속 증가한다.
* 재실행 후에도 기존 `.data`를 스캔해 다음 id가 이어져야 한다.

---

## 단계 6. SELECT 경로에서 id 조회는 인덱스를 사용하게 변경

### 목적

`WHERE id = ?`는 B+Tree를 타고, 나머지는 기존처럼 선형 탐색하게 만든다.

### 수정 파일

* `src/executor.c`

### 권장 함수

```c
static int execute_select(
    ExecutionContext *ctx,
    const SelectStatement *stmt,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);

static int can_use_id_index(
    const TableRuntime *table,
    const SelectStatement *stmt,
    uint64_t *out_id_key
);

static int execute_select_with_id_index(
    ExecutionContext *ctx,
    TableRuntime *table,
    const SelectStatement *stmt,
    uint64_t id_key,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);

static int execute_select_with_full_scan(
    ExecutionContext *ctx,
    TableRuntime *table,
    const SelectStatement *stmt,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);

static int project_single_row(
    const TableSchema *schema,
    const SelectStatement *stmt,
    const Row *source_row,
    Row *out_projected,
    char *errbuf,
    size_t errbuf_size
);

static int append_result_row(
    QueryResult *result,
    const Row *projected_row,
    char *errbuf,
    size_t errbuf_size
);
```

### 함수별 상세

#### `can_use_id_index`

입력:

* `table`
* `stmt`
* `out_id_key`

연산:

* `stmt->where_clause.has_condition == 1` 인지 확인
* where column이 정확히 `id`인지 확인
* `table->has_id_column == 1` 인지 확인
* literal이 canonical integer인지 `try_parse_indexable_id_literal()`로 확인

반환:

* 인덱스 사용 가능하면 `1`
* 아니면 `0`

#### `execute_select_with_id_index`

연산:

1. `bptree_search(id_key)` 호출
2. 못 찾으면 빈 결과 반환
3. 찾으면 `read_row_at_offset()` 호출
4. 혹시라도 방어적으로 `row_matches_where_clause()` 다시 호출
5. projection 적용
6. 결과 1 row 생성
7. `out_result->used_index = 1`

반환:

* 성공 시 `STATUS_OK`

#### `execute_select_with_full_scan`

연산:

* `scan_table_rows_with_offsets()`로 전체 파일을 스트리밍 스캔
* 각 row에 대해 `row_matches_where_clause()` 적용
* 일치하면 `project_single_row()` 후 `append_result_row()`
* `out_result->used_index = 0`

반환:

* 성공 시 `STATUS_OK`

### 매우 중요한 규칙

* `WHERE id = 123` → 인덱스 사용
* `WHERE id = '123'` → 인덱스 사용 가능
* `WHERE id = '00123'` → 인덱스 사용 금지, full scan
* `WHERE name = 'Alice'` → full scan
* `WHERE id = -1` → full scan
* `WHERE id = 1.0` → full scan

### 완료 기준

* 결과 자체는 기존 SELECT와 동일해야 한다.
* 단, `WHERE id = ?`일 때만 `ExecResult.used_index = 1`이 되어야 한다.
* 다른 조건은 전부 `used_index = 0`이다.

---

## 단계 7. 벤치마크 도구 추가

### 목적

1,000,000건 이상 insert 후, `id` 조회와 비-인덱스 조회의 속도 차이를 측정한다.

### 신규 파일 권장

* `src/benchmark.c`
* `src/benchmark_main.c` 또는 `tools/benchmark_bptree.c`

### 자료구조

```c
typedef struct {
    size_t row_count;
    size_t probe_count;

    double insert_total_ms;

    double id_select_total_ms;
    double id_select_avg_ms;

    double non_id_select_total_ms;
    double non_id_select_avg_ms;

    double speedup_ratio;
} BenchmarkReport;
```

### 공개 함수

```c
int run_benchmark(
    const char *db_dir,
    const char *table_name,
    size_t row_count,
    size_t probe_count,
    BenchmarkReport *out_report,
    char *errbuf,
    size_t errbuf_size
);
```

### 권장 내부 함수

```c
static int ensure_benchmark_schema(
    const char *db_dir,
    const char *table_name,
    char *errbuf,
    size_t errbuf_size
);

static int benchmark_bulk_insert(
    ExecutionContext *ctx,
    const char *table_name,
    size_t row_count,
    char *errbuf,
    size_t errbuf_size
);

static int benchmark_id_selects(
    ExecutionContext *ctx,
    const char *table_name,
    size_t probe_count,
    double *out_total_ms,
    char *errbuf,
    size_t errbuf_size
);

static int benchmark_non_id_selects(
    ExecutionContext *ctx,
    const char *table_name,
    size_t probe_count,
    double *out_total_ms,
    char *errbuf,
    size_t errbuf_size
);
```

### 벤치마크 규칙

1. benchmark 전용 schema는 직접 파일로 만든다.
   현재 엔진은 `CREATE TABLE`을 지원하지 않으므로 fixture 방식으로 준비한다.
2. row 수는 최소 `1,000,000`
3. 비교 쿼리는 반드시 결과 cardinality가 비슷해야 한다.
   예:

   * indexed: `SELECT * FROM users WHERE id = 900000`
   * full scan: `SELECT * FROM users WHERE name = 'user_900000'`
4. parsing overhead가 벤치마크를 오염시키지 않도록, benchmark 도구는 AST를 직접 만들어 `execute_statement()`를 호출해도 된다.
5. 시간 측정은 `clock_gettime(CLOCK_MONOTONIC, ...)` 사용 권장

### 출력 예시

```text
Rows inserted: 1000000
Insert total: 8421.53 ms
ID select avg: 0.08 ms
Name select avg: 57.31 ms
Speedup: 716.37x
```

### 완료 기준

* 1,000,000건 이상 insert가 실제로 수행된다.
* id 조회와 non-id 조회의 평균 시간을 둘 다 숫자로 보여준다.
* README에 실행 명령과 결과 예시가 들어간다.

---

## 단계 8. 테스트 추가 및 수정

### 목적

핵심 로직을 단위 테스트와 기능 테스트로 검증한다.

### 신규 테스트 파일 권장

* `tests/test_bptree.c`
* `tests/test_runtime_index.c`
* 기존 `tests/test_executor.c` 확장
* 기존 `tests/test_integration.sh` 확장

### 필수 테스트 케이스

#### `tests/test_bptree.c`

반드시 포함할 테스트:

* sequential insert 후 search 성공
* random insert 후 search 성공
* root split 발생 케이스
* internal split 발생 케이스
* duplicate key insert 실패
* leaf chain 정렬 검증
* `bptree_validate()` 성공 검증

#### `tests/test_runtime_index.c`

반드시 포함할 테스트:

* 기존 `.data` 파일에서 index build 성공
* `next_id == max(existing_id) + 1`
* duplicate id가 있는 파일에서 build 실패
* empty id가 있는 파일에서 build 실패
* malformed id에서 build 실패
* `read_row_at_offset()`가 정확한 row를 복원하는지 검증

#### `tests/test_executor.c` 추가 항목

반드시 포함할 테스트:

* `id` 자동 생성 insert
* column list insert에서도 자동 생성
* explicit `id` insert 실패
* `WHERE id = ?`에서 `used_index == 1`
* `WHERE non_id = ?`에서 `used_index == 0`
* non-canonical id literal은 인덱스를 쓰지 않는지 확인
* `id` 없는 테이블은 기존 behavior 유지

#### `tests/test_integration.sh`

반드시 포함할 시나리오:

1. schema 파일 준비
2. SQL 파일로 여러 INSERT 수행
3. SQL 파일로 `WHERE id = ?` 조회
4. 결과 row가 기대값과 같은지 검증
5. `.data` 파일에 auto-generated id가 실제로 기록되었는지 검증

### 완료 기준

* 새 테스트가 모두 통과한다.
* 기존 lexer/parser/storage 관련 테스트를 깨뜨리지 않는다.

---

## 단계 9. README 업데이트

### 목적

목요일 발표 때 README만 보고도 설명 가능한 상태로 만든다.

### README 필수 섹션

1. 프로젝트 개요
   기존 SQL 처리기 위에 B+Tree 인덱스를 얹었다는 설명
2. 아키텍처
   `Lexer -> Parser -> Executor -> Storage + Runtime Cache + B+Tree`
3. 저장 구조
   `.schema`, `.data`, row_offset 기반 인덱스 설명
4. id 자동 생성 규칙
5. 인덱스 사용 조건
   정확히 언제 `WHERE id = ?`가 B+Tree를 타는지
6. 복잡도 비교

   * indexed select: 대략 `O(log N)`
   * non-index select: `O(N)`
7. 테스트 방법
8. 벤치마크 실행 방법 및 결과
9. 한계점
   인덱스는 메모리 기반이며 디스크 영속화되지 않는다는 점

---

## 6. 파일별 변경 요약

### `src/main.c`

* `ExecutionContext` init/free 추가
* `execute_statement()` 호출 시 context 전달

### `src/cli.c`

* 변경 없음 권장
  benchmark는 별도 바이너리로 만드는 편이 깔끔하다

### `src/lexer.c`

* 변경 없음

### `src/parser.c`

* 변경 없음
  이번 과제는 SQL 문법 확장이 아니라 executor/runtime 확장이다

### `src/schema.c`

* 기존 함수 유지
* 필요하면 `schema_find_column_index(schema, "id")`를 재사용

### `src/storage.c`

* offset append, streaming scan, single-row read 추가
* `free_row()` 추가

### `src/executor.c`

* 가장 큰 변경 지점
* insert/select를 context 기반으로 재구성
* auto id / index lookup / full scan 분기 추가

### `src/result.c`

* 기존 출력 유지
* `used_index`, `generated_id`는 출력하지 않아도 된다

### 신규 파일

* `src/bptree.c`
* `src/runtime.c`
* `src/benchmark.c`
* `tests/test_bptree.c`
* `tests/test_runtime_index.c`

---

## 7. Codex가 반드시 지켜야 할 코드 작성 원칙

1. giant function 금지
   `execute_select()` 하나에 다 몰아넣지 말고, index path와 full scan path를 분리한다.
2. memory ownership 명확화
   어떤 함수가 할당하고 누가 해제하는지 주석 또는 함수 구조로 드러나야 한다.
3. 기존 스타일 유지
   `errbuf`, `STATUS_*`, `snake_case` 스타일 유지
4. global state 금지
   런타임 캐시는 `ExecutionContext`에만 둔다
5. 테스트 우선
   `bptree_validate()` 같은 테스트 보조 API를 넣어도 된다

---

## 8. 이 명세에서 애매하지 않게 확정해야 하는 정책

Codex가 임의로 바꾸지 말고 아래 정책을 그대로 따른다.

### 정책 A. indexed table의 정의

* schema에 정확히 `id` 컬럼이 있으면 indexed table

### 정책 B. auto id 시작값

* 빈 테이블이면 `1`부터 시작

### 정책 C. 기존 row의 id 형식

* canonical positive integer만 허용
* malformed/duplicate면 index build 실패

### 정책 D. INSERT에서 id 직접 지정

* 이번 과제 범위에서는 금지
* `INSERT INTO users (id, name) VALUES (...)` 는 에러

### 정책 E. index 사용 조건

* `WHERE id = ?` 이고, literal이 canonical integer일 때만 사용

### 정책 F. 인덱스 persistence

* 없음
* 프로세스 시작 후 첫 접근 시 `.data`를 스캔해서 rebuild

---

## 9. 최종 완료 체크리스트

아래 항목이 모두 만족되면 구현 완료로 본다.

* `id` 컬럼이 있는 테이블에 INSERT 하면 자동 id가 부여된다.
* 생성된 id가 `.data` 파일에 실제로 기록된다.
* 같은 id가 B+Tree에도 즉시 반영된다.
* `SELECT ... WHERE id = ?` 는 `used_index == 1`이다.
* 다른 컬럼 조건 조회는 `used_index == 0`이다.
* 1,000,000건 이상 insert benchmark가 돌아간다.
* id 조회와 non-id 조회의 평균 속도 차이를 숫자로 제시한다.
* unit test / integration test가 모두 통과한다.
* README만으로 발표가 가능하다.

---

## 10. 선택 구현으로 넣으면 차별화되는 항목

필수는 아니지만 여유가 있으면 아래 중 하나를 추가해도 좋다.

* B+Tree 구조를 DOT 파일로 덤프해서 시각화
* benchmark 결과에 median / p95 추가
* leaf chain을 이용한 range scan prototype
* debug mode에서 “이번 SELECT가 index path였는지”를 로그로 출력

이 명세의 핵심은 **“기존 SQL 처리기 구조를 유지한 상태에서, executor/storage/runtime에만 필요한 확장을 넣어 id 인덱스를 작동시키는 것”**입니다.
