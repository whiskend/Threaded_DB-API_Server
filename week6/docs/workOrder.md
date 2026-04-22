# SQL 처리기 통합 작업 지시서 및 참조 가이드

이 문서는 `sql-parser-engine` 프로젝트의 단일 기준 문서다. 세 명의 작업자가 각자의 Codex와 병렬로 작업할 때, 이 문서를 먼저 읽고 자기 파트 문서로 이동한다.

## 1. 문서 사용 규칙

### 최우선 규칙

* 이 문서와 각 파트 문서(`workOrder_A.md`, `workOrder_B.md`, `workOrder_C.md`)만 구현 기준으로 사용한다.
* `docs/study.md`는 학습용 설명서이며 구현 지시서가 아니다. Codex는 작업 중 `study.md`를 참고하지 않는다.
* 공통 계약은 이 문서가 고정한다. 각 파트 작업자는 자기 소유 파일만 수정한다.

### Codex 라우팅 문구

작업자가 Codex에게 아래처럼 말하면, Codex는 다음 순서로 문서를 읽고 작업해야 한다.

* `나는 A 작업을 맡은 작업자야`
  * 먼저 `docs/workOrder.md`를 읽고 공통 계약을 확인한다.
  * 그다음 `docs/workOrder_A.md`만 읽고 A 범위만 구현한다.
  * `docs/study.md`는 읽지 않는다.
* `나는 B 작업을 맡은 작업자야`
  * 먼저 `docs/workOrder.md`를 읽고 공통 계약을 확인한다.
  * 그다음 `docs/workOrder_B.md`만 읽고 B 범위만 구현한다.
  * `docs/study.md`는 읽지 않는다.
* `나는 C 작업을 맡은 작업자야`
  * 먼저 `docs/workOrder.md`를 읽고 공통 계약을 확인한다.
  * 그다음 `docs/workOrder_C.md`만 읽고 C 범위만 구현한다.
  * `docs/study.md`는 읽지 않는다.

## 2. 프로젝트 목표와 범위

이 프로젝트는 C로 구현하는 파일 기반 SQL 처리기다. 실행 흐름은 다음으로 고정한다.

```text
SQL 파일 입력 -> SQL 파싱 -> 실행 -> 파일 저장/조회 -> 결과 출력
```

### 이번 버전의 필수 기능

* CLI로 DB 디렉터리와 SQL 파일 경로를 입력받는다.
* SQL 파일 하나에는 SQL 1문장만 들어 있다.
* `INSERT`
* `SELECT`
* `<table>.schema` 파일로 테이블 구조를 읽는다.
* `<table>.data` 파일에 row를 저장하고 조회한다.
* 단위 테스트와 기능 테스트를 포함한다.
* `README.md`에 빌드/실행/테스트/구조 설명을 작성한다.

### 이번 버전의 제외 기능

* `CREATE TABLE`
* `UPDATE`
* `DELETE`
* `JOIN`
* 복잡한 SQL 문법
* 인덱스
* 트랜잭션

## 3. 병렬 작업 원칙

### 파일 소유권 원칙

한 파일에는 한 명의 주 작업자만 둔다. 다른 작업자는 그 파일을 읽을 수는 있지만 수정하지 않는다.

### 인터페이스 고정 원칙

아래 6장에 정의한 구조체와 함수 시그니처를 공통 계약으로 고정한다.

* 다른 파트의 헤더를 바꾸고 싶어도 먼저 이 문서의 계약을 기준으로 구현한다.
* 병합 전까지는 자기 소유 파일 내부에서만 해결한다.
* 인터페이스 변경이 꼭 필요하면 구현을 멈추기보다 메모를 남기고, 병합 시 한 번에 조정한다.

### 작업 순서 원칙

세 작업자는 동시에 시작해도 되지만, 첫 커밋은 각자 자기 파트의 public header와 테스트 스켈레톤을 먼저 만든다. 이후 구현을 진행한다.

## 4. 파트 분할과 책임

### A 파트: 입력과 문법 분석

책임:

* 빌드 스캐폴딩
* CLI 파싱
* 파일 읽기 유틸
* Lexer
* Parser
* AST 정의

생산 데이터:

* `CliOptions`
* `TokenArray`
* `Statement`

소유 파일:

* `Makefile`
* `include/ast.h`
* `include/cli.h`
* `include/lexer.h`
* `include/parser.h`
* `include/utils.h`
* `src/cli.c`
* `src/lexer.c`
* `src/parser.c`
* `src/utils.c`
* `tests/test_lexer.c`
* `tests/test_parser.c`

### B 파트: 스키마와 파일 저장소

책임:

* schema metadata 로드
* data 파일 생성/append/read
* escape / unescape 처리
* 샘플 DB 파일 관리

생산 데이터:

* `TableSchema`
* `Row`
* `Row[]`

소유 파일:

* `include/schema.h`
* `include/storage.h`
* `src/schema.c`
* `src/storage.c`
* `tests/test_storage.c`
* `db/users.schema`
* `db/products.schema`
* 필요 시 추가 샘플 `.schema`, `.data`

### C 파트: 실행, 출력, 통합

책임:

* 에러 코드 정의
* AST 실행
* 결과 출력
* `main.c` 조립
* executor 단위 테스트
* integration test
* 샘플 SQL 파일
* README

생산 데이터:

* `ExecResult`
* CLI 사용자 출력

소유 파일:

* `include/errors.h`
* `include/executor.h`
* `include/result.h`
* `src/executor.c`
* `src/result.c`
* `src/main.c`
* `tests/test_executor.c`
* `tests/test_integration.sh`
* `queries/insert_users.sql`
* `queries/select_users.sql`
* `queries/select_user_names.sql`
* `queries/invalid_missing_from.sql`
* `README.md`

## 5. 공통 실행 흐름

전체 프로그램의 상위 흐름은 아래 순서로 고정한다.

```text
main
 └─ parse_cli_args
 └─ read_text_file
 └─ tokenize_sql
 └─ parse_statement
 └─ execute_statement
     ├─ load_table_schema
     ├─ append_row_to_table
     └─ read_all_rows_from_table
 └─ print_exec_result
```

데이터 흐름은 아래와 같다.

```text
SQL text
  -> TokenArray
  -> Statement
  -> TableSchema / Row[]
  -> ExecResult
  -> stdout / stderr
```

## 6. 공통 데이터와 API 계약

아래 구조체와 함수 시그니처는 세 파트가 공유하는 고정 계약이다.

### 6-1. `cli.h`

```c
typedef struct {
    char *db_dir;
    char *sql_file;
    int help_requested;
} CliOptions;

int parse_cli_args(int argc, char **argv, CliOptions *out_options);
void print_usage(const char *program_name);
```

### 6-2. `utils.h`

```c
char *read_text_file(const char *path);
char *trim_whitespace(char *text);
char *strdup_safe(const char *src);
void *xmalloc(size_t size);
```

### 6-3. `lexer.h`

```c
typedef enum {
    TOKEN_EOF,
    TOKEN_IDENTIFIER,
    TOKEN_STRING,
    TOKEN_NUMBER,
    TOKEN_COMMA,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_SEMICOLON,
    TOKEN_STAR,
    TOKEN_INSERT,
    TOKEN_INTO,
    TOKEN_VALUES,
    TOKEN_SELECT,
    TOKEN_FROM
} TokenType;

typedef struct {
    TokenType type;
    char *text;
} Token;

typedef struct {
    Token *items;
    size_t count;
    size_t capacity;
} TokenArray;

int tokenize_sql(const char *sql, TokenArray *out_tokens, char *errbuf, size_t errbuf_size);
void free_token_array(TokenArray *tokens);
const char *token_type_name(TokenType type);
```

### 6-4. `ast.h`

```c
typedef enum {
    VALUE_STRING,
    VALUE_NUMBER
} ValueType;

typedef struct {
    ValueType type;
    char *text;
} LiteralValue;

typedef struct {
    char *table_name;
    char **columns;
    size_t column_count;
    LiteralValue *values;
    size_t value_count;
} InsertStatement;

typedef struct {
    char *table_name;
    int select_all;
    char **columns;
    size_t column_count;
} SelectStatement;

typedef enum {
    STMT_INSERT,
    STMT_SELECT
} StatementType;

typedef struct {
    StatementType type;
    union {
        InsertStatement insert_stmt;
        SelectStatement select_stmt;
    };
} Statement;
```

### 6-5. `parser.h`

```c
int parse_statement(const TokenArray *tokens, Statement *out_stmt, char *errbuf, size_t errbuf_size);
void free_statement(Statement *stmt);
```

### 6-6. `schema.h`

```c
typedef struct {
    char *table_name;
    char **columns;
    size_t column_count;
} TableSchema;

typedef struct {
    char **values;
    size_t value_count;
} Row;

int load_table_schema(const char *db_dir, const char *table_name,
                      TableSchema *out_schema,
                      char *errbuf, size_t errbuf_size);

int schema_find_column_index(const TableSchema *schema, const char *column_name);
void free_table_schema(TableSchema *schema);
```

### 6-7. `storage.h`

```c
int ensure_table_data_file(const char *db_dir, const char *table_name,
                           char *errbuf, size_t errbuf_size);

int append_row_to_table(const char *db_dir, const char *table_name,
                        const Row *row,
                        char *errbuf, size_t errbuf_size);

int read_all_rows_from_table(const char *db_dir, const char *table_name,
                             size_t expected_column_count,
                             Row **out_rows, size_t *out_row_count,
                             char *errbuf, size_t errbuf_size);

void free_rows(Row *rows, size_t row_count);
```

### 6-8. `result.h`

```c
typedef enum {
    RESULT_INSERT,
    RESULT_SELECT
} ExecResultType;

typedef struct {
    char **columns;
    size_t column_count;
    Row *rows;
    size_t row_count;
} QueryResult;

typedef struct {
    ExecResultType type;
    size_t affected_rows;
    QueryResult query_result;
} ExecResult;

void print_exec_result(const ExecResult *result);
```

### 6-9. `errors.h`

```c
typedef enum {
    STATUS_OK = 0,
    STATUS_INVALID_ARGS = 1,
    STATUS_FILE_ERROR = 2,
    STATUS_LEX_ERROR = 3,
    STATUS_PARSE_ERROR = 4,
    STATUS_SCHEMA_ERROR = 5,
    STATUS_STORAGE_ERROR = 6,
    STATUS_EXEC_ERROR = 7
} StatusCode;
```

### 6-10. `executor.h`

```c
int execute_statement(const char *db_dir, const Statement *stmt,
                      ExecResult *out_result,
                      char *errbuf, size_t errbuf_size);

void free_exec_result(ExecResult *result);
```

## 7. 공통 문법과 저장 포맷

### SQL 문법

```text
statement        ::= insert_stmt | select_stmt

insert_stmt      ::= INSERT INTO identifier
                     [ "(" identifier_list ")" ]
                     VALUES "(" literal_list ")"

select_stmt      ::= SELECT "*" FROM identifier
                   | SELECT identifier_list FROM identifier

identifier_list  ::= identifier { "," identifier }
literal_list     ::= literal { "," literal }
literal          ::= string_literal | number_literal
identifier       ::= [a-zA-Z_][a-zA-Z0-9_]*
```

### 입력 규칙

* 파일당 SQL 1문장만 처리한다.
* 세미콜론은 있어도 되고 없어도 된다.
* 키워드는 대소문자 구분 없이 처리한다.
* 문자열 리터럴은 `'Alice'` 형식이다.
* 숫자는 문자열로 저장하되 타입만 `VALUE_NUMBER`로 기록한다.

### 저장 포맷

`<table>.schema`

```text
id
name
age
```

`<table>.data`

```text
1|Alice|20
2|Bob|25
```

escape 규칙:

* `\` -> `\\`
* `|` -> `\|`
* 줄바꿈 -> `\n`

운영 규칙:

* `.schema` 파일이 테이블 존재 여부를 결정한다.
* `.data` 파일이 없으면 `INSERT` 시 생성한다.
* `.data` 파일이 없으면 `SELECT` 시 빈 테이블로 처리한다.

## 8. 메모리와 에러 처리 계약

### 메모리 계약

* `out_*` 구조체는 호출 전에 0으로 초기화되어 있다고 가정한다.
* 성공 시 동적 할당된 메모리는 해당 모듈의 `free_*` 함수로 해제한다.
* 실패 시 함수는 부분 할당을 정리하고 호출자가 안전하게 다음 단계로 넘어갈 수 있게 한다.

### 에러 계약

* 모든 에러 메시지는 사람이 읽을 수 있어야 한다.
* 에러 메시지는 `errbuf`에 기록하고, 최종 출력은 `main.c`에서 `stderr`로 보낸다.
* 대표 메시지 예시는 아래와 같다.

```text
LEX ERROR: unterminated string literal
PARSE ERROR: expected FROM after select list
SCHEMA ERROR: table 'users' not found
EXEC ERROR: unknown column 'age2'
```

## 9. 충돌 방지 규칙

* A는 `schema.h`, `storage.h`, `executor.h`, `result.h`, `errors.h`를 수정하지 않는다.
* B는 `ast.h`, `cli.h`, `lexer.h`, `parser.h`, `utils.h`, `executor.h`, `result.h`, `errors.h`, `main.c`를 수정하지 않는다.
* C는 `ast.h`, `cli.h`, `lexer.h`, `parser.h`, `utils.h`, `schema.h`, `storage.h`를 수정하지 않는다.
* README와 샘플 SQL은 C만 수정한다.
* 샘플 DB 파일은 B만 수정한다.
* 공통 계약이 불명확하면 구현을 멈추기보다 이 문서의 시그니처를 우선 따른다.

## 10. 병합 체크포인트

### 체크포인트 1: 헤더 고정

각 작업자는 자기 소유 헤더를 이 문서와 동일한 시그니처로 먼저 만든다.

### 체크포인트 2: 파트별 단위 테스트

* A: `test_lexer`, `test_parser`
* B: `test_storage`
* C: `test_executor`

### 체크포인트 3: 통합

병합 후 아래가 통과해야 한다.

* `make`
* `make test`
* `tests/test_integration.sh`

## 11. 완료 기준

프로젝트 완료는 아래를 모두 만족할 때다.

* `INSERT`와 `SELECT`가 명세대로 동작한다.
* 잘못된 SQL과 깨진 data 파일에 대해 명확한 에러를 낸다.
* 샘플 schema, 샘플 SQL, 테스트, README가 모두 준비되어 있다.
* 세 작업자의 파일 수정 범위가 겹치지 않는다.

## 12. 파트 문서 이동

* A 작업자는 `docs/workOrder_A.md`
* B 작업자는 `docs/workOrder_B.md`
* C 작업자는 `docs/workOrder_C.md`

이동 전후로도 공통 계약은 항상 이 문서를 기준으로 한다.
