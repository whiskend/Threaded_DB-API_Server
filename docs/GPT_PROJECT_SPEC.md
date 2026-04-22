# GPT Project Spec: Threaded_DB-API_Server

이 문서는 GPT가 이 프로젝트를 이어서 이해하고 수정할 때 사용할 구현 기준 스펙이다.
기존 발표/학습용 문서는 현재 코드와 맥락이 다른 부분이 있으므로, 이 문서는 `src/`, `include/`,
`tests/`, `queries/`, `db/`, `Makefile`의 실제 구현만 기준으로 한다.

## 1. 현재 프로젝트의 정체

현재 구현된 프로그램은 이름과 달리 HTTP API 서버가 아니다. 실제 동작하는 핵심은 C로 작성된
파일 기반 미니 SQL 처리기이며, 실행 파일 이름은 `sql_processor`다.

주요 기능은 다음과 같다.

- SQL 파일을 CLI로 입력받는다.
- SQL 전체를 한 번에 lexer로 토큰화한다.
- 토큰 스트림에서 `INSERT` 또는 `SELECT` 문을 순서대로 파싱한다.
- `.schema` 파일로 테이블 컬럼 목록을 읽는다.
- `.data` 파일에 행을 append하거나 scan한다.
- 테이블에 정확히 `id` 컬럼이 있으면 in-memory B+Tree 인덱스를 만든다.
- `WHERE id = <canonical positive integer>` 형태의 SELECT는 B+Tree로 `row_offset`을 찾는다.
- 그 외 SELECT는 `.data` 파일을 처음부터 끝까지 streaming full scan한다.

지원하지 않는 것:

- HTTP 서버, thread pool, socket 처리
- JSON API
- 트랜잭션, rollback, 동시성 제어
- CREATE TABLE, UPDATE, DELETE
- JOIN, ORDER BY, GROUP BY, BETWEEN, LIKE
- 타입 시스템
- 영구 저장되는 인덱스 파일
- row update/delete에 따른 인덱스 유지

## 2. 빌드와 실행

### 빌드

```sh
make
```

`Makefile`은 `clang`과 다음 플래그를 사용한다.

```make
-std=c99 -Wall -Wextra -Werror -Iinclude
```

빌드 산출물:

- `sql_processor`: 메인 CLI 실행 파일
- `build/test_*`: C 단위 테스트 바이너리
- `build/benchmark_bptree`: 벤치마크 CLI

### 테스트

```sh
make test
```

`make test`는 `tests/test_*.c`로 만든 바이너리를 모두 실행하고, `tests/test_integration.sh`도 실행한다.

### CLI 실행

```sh
./sql_processor -d db -f queries/multi_statements.sql
./sql_processor --db db --file queries/select_users.sql
./sql_processor -h
```

필수 인자:

- `-d`, `--db`: DB 디렉터리
- `-f`, `--file`: SQL 파일

기본값은 없다. help가 아니면 둘 다 반드시 필요하다.

## 3. 디렉터리와 파일 역할

```text
include/
  ast.h          SQL AST 구조체
  lexer.h        SQL lexer public API
  parser.h       SQL parser public API
  schema.h       .schema 로딩과 TableSchema/Row
  storage.h      .data 파일 append/scan/read API
  runtime.h      ExecutionContext, TableRuntime, id index build
  executor.h     Statement 실행 진입점
  result.h       ExecResult와 stdout 렌더링
  bptree.h       in-memory B+Tree
  cli.h          CLI option parser
  errors.h       공통 STATUS_* 코드
  benchmark.h    benchmark report API

src/
  main.c         CLI entrypoint와 전체 실행 loop
  cli.c          argv 파싱
  utils.c        파일 전체 읽기, trim, safe strdup, xmalloc
  lexer.c        SQL 문자열 -> TokenArray
  parser.c       TokenArray -> Statement AST
  schema.c       db_dir/table.schema -> TableSchema
  storage.c      db_dir/table.data 파일 직렬화/역직렬화
  runtime.c      테이블 runtime cache와 id B+Tree rebuild
  executor.c     INSERT/SELECT 실행
  result.c       ExecResult 출력
  bptree.c       B+Tree 삽입/검색/검증
  benchmark.c    indexed select vs full scan 벤치마크

tests/
  test_lexer.c
  test_parser.c
  test_storage.c
  test_bptree.c
  test_runtime_index.c
  test_executor.c
  test_integration.sh

queries/
  insert_users.sql
  select_users.sql
  select_user_where.sql
  multi_statements.sql

db/
  users.schema
  products.schema
  users.data
```

## 4. 전체 실행 흐름

`src/main.c`가 전체 오케스트레이션을 한다.

```text
argv
  -> parse_cli_args()
  -> read_text_file(sql_file)
  -> trim_whitespace(sql_text)
  -> tokenize_sql(trimmed_sql)
  -> init_execution_context(db_dir)
  -> while token cursor != EOF:
       parse_next_statement(tokens, &cursor)
       execute_statement(ctx, stmt)
       print_exec_result(result)
       free_exec_result()
       free_statement()
  -> free_token_array()
  -> free_execution_context()
```

중요한 의미:

- SQL 파일 전체가 먼저 tokenization된다. 따라서 lex error가 있으면 어떤 statement도 실행되지 않는다.
- parse와 execute는 statement 단위로 반복된다.
- 앞 statement가 이미 실행된 뒤 뒤 statement에서 실패하면 rollback하지 않는다.
- `ExecutionContext`는 한 번 만든 뒤 파일 안의 모든 statement가 공유한다.
- 같은 테이블은 최초 접근 시 schema/index를 로드하고 이후 context 안에서 캐시된다.

## 5. 상태 코드

`include/errors.h`의 `StatusCode`가 모듈 간 공통 상태다.

```c
STATUS_OK = 0
STATUS_INVALID_ARGS = 1
STATUS_FILE_ERROR = 2
STATUS_LEX_ERROR = 3
STATUS_PARSE_ERROR = 4
STATUS_SCHEMA_ERROR = 5
STATUS_STORAGE_ERROR = 6
STATUS_EXEC_ERROR = 7
STATUS_INDEX_ERROR = 8
```

주의:

- 일부 모듈은 내부에서 raw `0`/`1`을 쓰고 executor/runtime에서 공통 코드로 변환한다.
- 에러 메시지는 caller가 넘긴 `errbuf`에 사람이 읽을 수 있는 문자열로 기록된다.
- CLI는 최종 status code를 process exit code로 반환한다.

## 6. SQL 문법

### Lexer

`tokenize_sql()`은 입력 SQL 문자열을 `TokenArray`로 바꾼다.

지원 토큰:

- keyword: `INSERT`, `INTO`, `VALUES`, `SELECT`, `FROM`, `WHERE`
- identifier: `[A-Za-z_][A-Za-z0-9_]*`
- string literal: single quote 문자열
- number literal: optional `-`, digits, optional one decimal point
- punctuation: `,`, `(`, `)`, `;`, `*`, `=`
- EOF sentinel

키워드는 대소문자를 무시한다. 식별자 text는 원문 대소문자를 유지하고, 이후 schema lookup은
대소문자를 구분한다.

문자열 literal:

```sql
'Alice'
'O''Reilly'
```

lexer는 quote를 제거하고 `''`를 `'`로 바꾼 text를 저장한다.

숫자 literal:

```sql
1
-7
3.14
```

숫자는 파싱 단계까지는 문자열 text로 보관된다. executor/storage도 대부분 값을 문자열로 다룬다.

### Parser

최상위 AST는 `Statement`다.

```c
typedef enum {
    STMT_INSERT,
    STMT_SELECT
} StatementType;
```

지원 INSERT:

```sql
INSERT INTO users VALUES ('Alice', 20);
INSERT INTO users (name, age) VALUES ('Alice', 20);
```

지원 SELECT:

```sql
SELECT * FROM users;
SELECT id, name FROM users;
SELECT * FROM users WHERE id = 2;
SELECT name FROM users WHERE age = 20;
```

WHERE는 단일 equality 조건만 지원한다.

```text
WHERE <identifier> = <literal>
```

파서는 SQL 의미를 검사하지 않는다. 예를 들어 컬럼 존재 여부, value 개수, id 정책은 executor에서 검사한다.

`parse_next_statement()`는 토큰 스트림의 현재 cursor에서 statement 하나를 읽고 cursor를 이동한다.
세미콜론은 statement terminator로 optional이며, 빈 statement separator는 건너뛴다.

## 7. AST와 값 표현

`include/ast.h` 기준 구조:

```c
typedef enum {
    VALUE_STRING,
    VALUE_NUMBER
} ValueType;

typedef struct {
    ValueType type;
    char *text;
} LiteralValue;
```

`LiteralValue.type`은 lexer/parser가 string과 number literal을 구분하기 위해 들고 있지만, 현재 storage와
일반 비교 로직은 값을 모두 문자열로 취급한다.

INSERT AST:

```c
typedef struct {
    char *table_name;
    char **columns;
    size_t column_count;
    LiteralValue *values;
    size_t value_count;
} InsertStatement;
```

SELECT AST:

```c
typedef struct {
    char *table_name;
    int select_all;
    char **columns;
    size_t column_count;
    WhereClause where_clause;
} SelectStatement;
```

AST 내부 문자열/배열은 heap 소유권을 가진다. 실행 후 반드시 `free_statement()`로 해제한다.

## 8. DB 파일 포맷

DB 디렉터리는 테이블별로 다음 두 파일을 가진다.

```text
<table>.schema
<table>.data
```

예:

```text
db/users.schema
db/users.data
```

### `.schema`

컬럼 이름을 줄 단위로 저장한다.

```text
id
name
age
```

규칙:

- 빈 줄은 무시한다.
- 각 줄은 앞뒤 공백을 trim한다.
- 중복 컬럼명은 schema error다.
- 타입 정보는 없다.
- primary key 선언도 없다.
- 컬럼명이 정확히 `id`이면 runtime이 auto-id와 B+Tree 인덱스 대상으로 간주한다.

### `.data`

한 row가 한 줄이다. 필드는 `|`로 구분한다.

```text
1|Alice|20
2|Bob|25
```

escape 규칙:

| 원본 문자 | 저장 형태 |
| --- | --- |
| `\` | `\\` |
| `|` | `\|` |
| newline | `\n` |

예:

```text
1|Alice\|Admin|line1\nline2\\done
```

역직렬화 시 허용되는 escape는 `\\`, `\|`, `\n`뿐이다. 그 외 escape나 dangling backslash는 malformed row로
처리된다.

schema 컬럼 수와 저장된 field 수가 다르면 `storage.c`가 현재 schema 길이에 맞춘다.

- 저장된 field가 부족하면 빈 문자열로 padding한다.
- 저장된 field가 많으면 뒤쪽 field를 truncate한다.

`.data` 파일이 없으면 빈 테이블로 간주한다. 단, table runtime을 로드할 때 `ensure_table_data_file()`이
호출되어 없는 `.data` 파일은 만들어질 수 있다.

## 9. Runtime Cache와 인덱스 빌드

`ExecutionContext`는 한 번의 CLI 실행 동안 공유되는 runtime 상태다.

```c
typedef struct {
    char *db_dir;
    TableRuntime *tables;
    size_t table_count;
    size_t table_capacity;
} ExecutionContext;
```

테이블 하나는 `TableRuntime`으로 캐시된다.

```c
typedef struct {
    char *table_name;
    TableSchema schema;
    int has_id_column;
    int id_column_index;
    int id_index_ready;
    uint64_t next_id;
    BPTree id_index;
} TableRuntime;
```

`get_or_load_table_runtime()` 흐름:

```text
ctx.tables에서 table_name 검색
  -> 있으면 재사용
  -> 없으면:
       table_name 복사
       load_table_schema(db_dir, table_name)
       ensure_table_data_file(db_dir, table_name)
       schema_find_column_index(schema, "id")
       bptree_init(id_index)
       if id column exists:
           build_id_index_for_table()
           id_index_ready = 1
       ctx.tables 동적 배열에 append
```

캐시는 외부 파일 변경을 자동 감지하지 않는다. 같은 `ExecutionContext` 안에서 한 번 로드된 테이블은
schema와 index를 계속 재사용한다.

### id 인덱스 rebuild

`build_id_index_for_table()`은 `.data` 전체를 streaming scan하면서 다음을 수행한다.

```text
for each row with row_offset:
  id_text = row.values[id_column_index]
  parse_stored_id_value(id_text)
  bptree_insert(id, row_offset)
  max_id 갱신

next_id = max_id + 1
빈 테이블이면 next_id = 1
```

stored id 검증 규칙:

- 양의 10진 정수여야 한다.
- `0`은 불가하다.
- leading zero는 불가하다. 예: `001` 불가.
- `-1`, `1.0`, 빈 문자열, overflow는 불가하다.
- 중복 id는 B+Tree duplicate key error로 runtime load 실패가 된다.

인덱스는 파일에 저장되지 않는다. 프로세스 시작 후 테이블 최초 접근 때마다 `.data`에서 재구성된다.

## 10. INSERT 실행 규칙

`execute_statement()`가 `STMT_INSERT`면 `execute_insert()`로 위임한다.

### id 컬럼이 있는 테이블

컬럼명이 정확히 `id`인 테이블은 auto-id 테이블이다.

규칙:

- 사용자는 `id` 값을 직접 넣을 수 없다.
- column list가 없으면 `schema.column_count - 1`개의 value가 필요하다.
- 이때 value는 schema 순서에서 `id` 컬럼을 건너뛰며 배치된다.
- column list가 있으면 column 수와 value 수가 같아야 한다.
- column list 안에 `id`가 있으면 error다.
- unknown column은 error다.
- duplicate column은 error다.
- column list에서 빠진 non-id 컬럼은 빈 문자열로 저장된다.
- generated id는 `table->next_id`를 사용한다.

실행 흐름:

```text
table = get_or_load_table_runtime()
validate_insert_columns_for_auto_id()
generated_id = table->next_id
build_insert_row_with_generated_id()
append_row_to_table_with_offset()
bptree_insert(table->id_index, generated_id, row_offset)
table->next_id++
result.type = RESULT_INSERT
result.affected_rows = 1
result.has_generated_id = 1
result.generated_id = generated_id
```

예:

schema:

```text
id
name
age
```

SQL:

```sql
INSERT INTO users VALUES ('Alice', 20);
```

저장:

```text
1|Alice|20
```

SQL:

```sql
INSERT INTO users (age, name) VALUES (21, 'Bob');
```

저장:

```text
1|Bob|21
```

### id 컬럼이 없는 테이블

기존 INSERT 동작을 유지한다.

규칙:

- column list가 없으면 value 수가 schema 컬럼 수와 같아야 한다.
- column list가 있으면 column 수와 value 수가 같아야 한다.
- unknown column은 error다.
- duplicate column은 error다.
- column list에서 빠진 컬럼은 빈 문자열로 저장된다.
- auto-id와 B+Tree 인덱스는 사용하지 않는다.

## 11. SELECT 실행 규칙

`execute_statement()`가 `STMT_SELECT`면 `execute_select()`로 위임한다.

공통 흐름:

```text
table = get_or_load_table_runtime()
validate_select_columns()
if can_use_id_index():
    execute_select_with_id_index()
else:
    execute_select_with_full_scan()
result.type = RESULT_SELECT
result.affected_rows = result.query_result.row_count
```

projection:

- `SELECT *`이면 schema 컬럼 전체를 schema 순서대로 출력한다.
- `SELECT id, name`이면 지정한 컬럼만 지정한 순서대로 출력한다.
- projection column 존재 여부는 executor에서 검사한다.
- SELECT projection의 duplicate column은 현재 명시적으로 막지 않는다.

WHERE:

- WHERE가 없으면 모든 row가 매칭된다.
- WHERE가 있으면 row의 해당 field와 literal text를 `strcmp()`로 완전 비교한다.
- 타입 변환은 없다.
- `WHERE age = 20`과 `WHERE age = '20'`은 둘 다 literal text가 `20`이므로 같은 결과가 된다.

### B+Tree 인덱스 경로

다음 조건을 모두 만족하면 인덱스를 사용한다.

- 테이블에 exact name `id` 컬럼이 있다.
- runtime에서 id index build가 완료됐다.
- WHERE가 있다.
- WHERE 컬럼명이 정확히 `id`다.
- WHERE literal text가 canonical positive uint64다.

인덱스 가능 예:

```sql
SELECT * FROM users WHERE id = 2;
SELECT name FROM users WHERE id = '2';
```

`try_parse_indexable_id_literal()`은 `LiteralValue.type`을 보지 않고 text만 검사한다. 따라서 문자열 literal
`'2'`도 인덱스를 탈 수 있다.

인덱스 불가 예:

```sql
SELECT * FROM users WHERE id = 001;
SELECT * FROM users WHERE id = '001';
SELECT * FROM users WHERE id = 0;
SELECT * FROM users WHERE id = -1;
SELECT * FROM users WHERE name = 'Bob';
```

인덱스 경로 흐름:

```text
initialize_query_result_columns()
bptree_search(id_index, id_key) -> row_offset
out_result.used_index = 1
if not found:
    return zero-row result
read_row_at_offset(db_dir, table_name, row_offset)
row_matches_where_clause()로 문자열 equality 재확인
project_single_row()
append_result_row()
```

row를 다시 읽은 뒤 WHERE를 재확인하는 이유는 string-level 의미 보존을 위한 방어 로직이다.

### Full scan 경로

인덱스를 탈 수 없으면 전체 `.data`를 streaming scan한다.

```text
initialize_query_result_columns()
state.where_index = schema_find_column_index(where column)
scan_table_rows_with_offsets(..., select_full_scan_callback)
  for each row:
    row_matches_where_clause()
    project_single_row()
    append_result_row()
out_result.used_index = 0
```

Full scan은 row 전체를 한 번에 materialize하지 않고 callback 방식으로 한 줄씩 처리한다. 단, 결과 row는
`QueryResult.rows`에 deep copy로 누적된다.

## 12. ExecResult와 출력

`include/result.h`:

```c
typedef struct {
    ExecResultType type;
    size_t affected_rows;
    QueryResult query_result;
    int used_index;
    int has_generated_id;
    uint64_t generated_id;
} ExecResult;
```

INSERT 결과:

- `type = RESULT_INSERT`
- `affected_rows = 1`
- auto-id 테이블이면 `has_generated_id = 1`, `generated_id = ...`

현재 `print_exec_result()`는 generated id를 출력하지 않고 다음 형태만 출력한다.

```text
INSERT 1
```

SELECT 결과는 ASCII table로 출력한다.

```text
id | name
---+------
2  | Bob

1 rows selected
```

`used_index`는 출력하지 않는다. 테스트와 벤치마크가 내부적으로 확인한다.

## 13. B+Tree 구현 스펙

위치:

- `include/bptree.h`
- `src/bptree.c`

용도:

- `uint64_t id`를 `.data` 파일의 `long row_offset`에 매핑한다.
- 현재는 `id` equality lookup만 사용한다.
- range scan은 구현되어 있지 않다.

상수:

```c
#define BPTREE_ORDER 64
#define BPTREE_MAX_KEYS 63
#define BPTREE_MAX_CHILDREN 64
```

노드 구조:

```c
typedef struct BPTreeNode {
    int is_leaf;
    size_t key_count;
    uint64_t keys[BPTREE_MAX_KEYS];
    struct BPTreeNode *parent;
    struct BPTreeNode *next;
    union {
        struct BPTreeNode *children[BPTREE_MAX_CHILDREN];
        long row_offsets[BPTREE_MAX_KEYS];
    } ptrs;
} BPTreeNode;
```

의미:

- leaf node: `keys[i] -> row_offsets[i]`
- internal node: separator key와 child pointer를 가진다.
- `next`는 leaf chain에서 오른쪽 leaf를 가리킨다.
- `parent`는 split 전파와 validation에 사용한다.

검색:

```text
node = root
while node is internal:
  child_index = first index where key < keys[index]
  node = children[child_index]
leaf에서 key linear search
```

internal child 선택 조건은 코드상 다음과 같다.

```c
while (child_index < node->key_count && key >= node->keys[child_index]) {
    child_index++;
}
```

즉 separator key 이상이면 오른쪽 child로 내려간다.

삽입:

```text
if root == NULL:
  leaf root 생성 후 첫 key 저장
else:
  leaf = find_leaf_node(root, key)
  if leaf has room:
    insert_into_leaf()
  else:
    split_leaf_and_insert()
  tree->key_count++
```

중복 key는 `STATUS_INDEX_ERROR`다.

leaf split:

```text
기존 63개 + 새 key = temp 64개
left_count = 64 / 2 = 32
right_count = 32
기존 leaf는 왼쪽 32개
new_leaf는 오른쪽 32개
new_leaf->next = old leaf->next
old leaf->next = new_leaf
separator = new_leaf->keys[0]
insert_into_parent()
```

internal split:

```text
기존 63 separator + 새 separator = temp 64개
temp children = 65개
split_index = 64 / 2 = 32
promoted_key = temp_keys[32]
left node: keys[0..31], children[0..32]
right node: keys[33..63], children[33..64]
promoted_key를 parent로 전파
```

root split:

- parent가 없으면 새 internal root를 만든다.
- 새 root는 `separator_key`, `left`, `right`를 가진다.

검증:

`bptree_validate()`가 확인하는 것:

- root/key_count consistency
- parent pointer consistency
- node key_count overflow
- node 내부 key strict increasing
- internal child pointer non-null
- child key range
- 모든 leaf depth 동일
- leaf chain이 오름차순
- leaf chain에서 센 key 수가 `tree->key_count`와 같음

삭제, update, merge, redistribution은 없다.

## 14. Storage API 세부 흐름

### Append

`append_row_to_table_with_offset()`:

```text
ensure_table_data_file()
open <table>.data as "ab+"
fseek end
out_row_offset = ftell(file)
for each value:
  escape_field()
  write "|" separator if needed
  write escaped value
write "\n"
fclose
```

`row_offset`은 append 시작 위치다. id index는 이 offset을 leaf value로 저장한다.

### Full scan

`scan_table_rows_with_offsets()`:

```text
open <table>.data as "rb"
if ENOENT:
  return STATUS_OK
while read_line():
  row_offset = ftell(file) before reading line
  parse_line_into_row(line, expected_column_count)
  callback(&row, row_offset, user_data)
  free_row(&row)
callback return:
  0 -> continue
  1 -> normal early stop
 -1 -> storage error
```

### Offset read

`read_row_at_offset()`:

```text
open <table>.data as "rb"
fseek(row_offset)
read one line
parse_line_into_row()
```

이 함수가 B+Tree lookup 후 실제 row를 복원하는 핵심이다.

## 15. 메모리 소유권 규칙

중요한 free 함수:

- `free_token_array()`
- `free_statement()`
- `free_table_schema()`
- `free_row()`
- `free_rows()`
- `free_exec_result()`
- `free_execution_context()`
- `bptree_destroy()`

일반 규칙:

- lexer/parser/schema/storage/executor는 대부분 heap에 문자열 배열을 만든다.
- `ExecResult.query_result`는 SELECT 결과의 column 이름과 row 값을 deep copy로 소유한다.
- storage scan callback이 받는 `Row *row`는 callback 호출이 끝나면 storage가 해제한다. callback 밖에 보관하려면 deep copy해야 한다.
- B+Tree node는 모두 heap allocation이며 `bptree_destroy()`가 재귀 해제한다.
- `ExecutionContext`를 해제하면 캐시된 schema와 B+Tree도 함께 해제된다.

## 16. 테스트가 보장하는 주요 동작

`test_lexer.c`:

- keyword case-insensitive
- string quote escape
- negative/decimal number token
- invalid character error
- unterminated string error

`test_parser.c`:

- INSERT without column list
- SELECT projection with WHERE
- multiple statements from one token stream
- missing projection/FROM/INTO/equals/paren error

`test_storage.c`:

- schema load and duplicate column error
- append escape format
- escape round trip
- missing/empty data file is empty table
- short row padding
- long row truncation
- malformed escape error

`test_bptree.c`:

- sequential and random insert/search
- root/internal split
- duplicate key failure
- sorted leaf chain

`test_runtime_index.c`:

- existing `.data` rebuilds id index and next_id
- duplicate id build failure
- empty/malformed id build failure
- offset read round trip with escaped values

`test_executor.c`:

- auto-id INSERT success
- column-list auto-id INSERT reorder
- explicit id INSERT failure
- `WHERE id = ?` uses index
- non-id WHERE uses full scan
- non-canonical id literal does not use index
- table without id keeps non-indexed behavior

`test_integration.sh`:

- real `sql_processor` binary with SQL files
- auto-generated ids persisted to `.data`
- multi-statement file executes in order
- SELECT output contains expected rows and row counts

## 17. 벤치마크

`run_benchmark()`는 다음을 수행한다.

```text
ensure benchmark db_dir
write schema id/name/age
reset data file
init_execution_context()
bulk insert row_count rows
run probe_count indexed id SELECTs
run probe_count non-id full scan SELECTs
report total/average/speedup
```

`tools/benchmark_bptree.c` CLI:

```sh
./build/benchmark_bptree -d /tmp/benchdb -t users -n 1000000 -p 100
```

기본값:

- table name: `users`
- row count: `1000000`
- probe count: `100`

벤치마크는 `result.used_index`가 기대와 다르면 error를 낸다.

## 18. 설계상 중요한 제약과 함정

- 프로젝트 이름만 보고 API 서버/스레드 서버라고 가정하면 안 된다.
- `id` 컬럼 존재 자체가 auto-id와 B+Tree 인덱스 정책을 켠다.
- schema에는 타입이 없으므로 모든 저장 값은 문자열이다.
- `VALUE_NUMBER`도 저장될 때는 text만 저장된다.
- WHERE 비교는 문자열 equality다.
- id index lookup 여부만 canonical positive integer parsing을 사용한다.
- B+Tree는 in-memory다. 프로세스 재시작 시 `.data`에서 rebuild한다.
- `ExecutionContext`는 thread-safe하지 않다.
- transaction이 없으므로 batch 중간 실패 시 앞선 INSERT는 이미 `.data`에 남는다.
- table cache는 같은 실행 중 외부 파일 변경을 반영하지 않는다.
- `.data` append 후 B+Tree insert가 실패하면 파일에 쓴 row는 rollback되지 않는다.
- `.data`에 잘못된 id가 저장되어 있으면 해당 테이블 runtime load 자체가 실패한다.
- explicit id insert는 auto-id 테이블에서 금지된다. 기존 data import는 `.data` 파일을 직접 만들거나 별도 migration이 필요하다.
- SELECT duplicate projection column은 현재 허용된다.
- parser는 의미 검사를 하지 않는다. 의미 검사는 executor가 담당한다.

## 19. 향후 작업 시 GPT가 지켜야 할 기준

- 이 문서보다 코드와 테스트가 더 최신 source of truth다.
- 기능을 바꾸면 해당 모듈 테스트를 먼저 읽고 기대 동작을 업데이트해야 한다.
- SQL 문법 확장은 lexer, parser, AST, executor, tests를 함께 변경해야 한다.
- storage 포맷 변경은 backward compatibility를 명시적으로 결정해야 한다.
- id 정책 변경은 runtime rebuild, INSERT, SELECT index path, benchmark, integration test에 모두 영향을 준다.
- concurrency/API server를 추가하려면 `ExecutionContext`, `.data` append, B+Tree mutation에 lock 전략이 필요하다.
- UPDATE/DELETE를 추가하려면 file format과 B+Tree maintenance 전략이 먼저 필요하다.
- persistent index를 추가하려면 `.data` row_offset 안정성과 crash recovery 정책을 같이 설계해야 한다.
