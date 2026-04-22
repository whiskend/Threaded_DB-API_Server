좋아요. 아래는 **Codex 작업 지시용 상세 명세서**입니다.
목표는 두 가지입니다.

1. Codex가 무엇을 만들고 있는지 단계별로 추적 가능해야 한다.
2. 이 명세서만 읽어도 이후 생성된 C 코드를 바로 이해할 수 있어야 한다.

---

# SQL 처리기 구현 명세서 (Codex 작업 지시용)

## 1. 프로젝트 목표

C 언어로 동작하는 **파일 기반 SQL 처리기**를 구현한다.

이 프로그램은 다음 흐름으로 동작해야 한다.

```text
SQL 파일 입력 -> SQL 파싱 -> 실행 -> 파일 저장/조회 -> 결과 출력
```

최소 지원 기능은 다음 두 가지다.

* `INSERT`
* `SELECT`

`CREATE TABLE`은 구현하지 않는다.
테이블 구조는 이미 존재한다고 가정하며, 이를 **schema metadata 파일**로 표현한다.

---

## 2. 반드시 지켜야 할 범위

### 2-1. 필수 구현 범위

* 구현 언어는 **C**
* 입력은 **텍스트 파일(.sql)** 로 받는다
* CLI로 SQL 파일 경로를 전달받는다
* SQL 문을 파싱해서 구조화한다
* `INSERT`는 파일에 데이터를 추가한다
* `SELECT`는 파일에서 데이터를 읽어 출력한다
* 각 테이블은 파일 기반으로 관리한다
* 단위 테스트와 기능 테스트를 포함한다
* README에 빌드/실행/테스트/구조 설명을 작성한다

### 2-2. 이번 버전에서 제외

* `CREATE TABLE`
* `UPDATE`
* `DELETE`
* `JOIN`
* 복잡한 SQL 문법
* 타입 시스템 고도화
* 인덱스
* 트랜잭션

---

## 3. 최종 산출물

Codex는 최종적으로 아래 결과를 만들어야 한다.

1. 빌드 가능한 C 프로젝트
2. `Makefile`
3. 실행 파일 `sql_processor`
4. 샘플 DB 디렉터리
5. 샘플 SQL 파일
6. 단위 테스트 + 기능 테스트
7. 발표용 README

---

## 4. 프로젝트 디렉터리 구조

아래 구조를 기준으로 구현한다.

```text
sql_processor/
├─ Makefile
├─ README.md
├─ db/
│  ├─ users.schema
│  ├─ users.data
│  ├─ products.schema
│  └─ products.data
├─ queries/
│  ├─ insert_users.sql
│  ├─ select_users.sql
│  ├─ select_user_names.sql
│  └─ invalid_missing_from.sql
├─ include/
│  ├─ ast.h
│  ├─ cli.h
│  ├─ errors.h
│  ├─ executor.h
│  ├─ lexer.h
│  ├─ parser.h
│  ├─ result.h
│  ├─ schema.h
│  ├─ storage.h
│  └─ utils.h
├─ src/
│  ├─ main.c
│  ├─ cli.c
│  ├─ lexer.c
│  ├─ parser.c
│  ├─ executor.c
│  ├─ schema.c
│  ├─ storage.c
│  ├─ result.c
│  └─ utils.c
└─ tests/
   ├─ test_lexer.c
   ├─ test_parser.c
   ├─ test_storage.c
   ├─ test_executor.c
   └─ test_integration.sh
```

---

## 5. 전체 실행 흐름

프로그램의 최상위 실행 흐름은 반드시 아래 순서를 따른다.

```text
main
 └─ parse_cli_args
 └─ read_text_file
 └─ tokenize_sql
 └─ parse_statement
 └─ execute_statement
     ├─ load_table_schema
     ├─ append_row_to_table   (INSERT)
     └─ read_all_rows_from_table (SELECT)
 └─ print_exec_result
```

즉, `main.c`는 조립만 담당하고, 실제 로직은 각 모듈로 분리한다.

---

## 6. CLI 명세

CLI는 아래 형식을 지원한다.

```bash
./sql_processor --db ./db --file ./queries/insert_users.sql
```

축약형도 허용한다.

```bash
./sql_processor -d ./db -f ./queries/insert_users.sql
```

### 필수 옵션

* `--db`, `-d`: DB 디렉터리 경로
* `--file`, `-f`: SQL 파일 경로
* `--help`, `-h`: 사용법 출력

### 관련 함수

```c
// cli.h
typedef struct {
    char *db_dir;
    char *sql_file;
    int help_requested;
} CliOptions;

int parse_cli_args(int argc, char **argv, CliOptions *out_options);
void print_usage(const char *program_name);
```

### 완료 기준

* 인자가 부족하면 사용법 출력 후 종료
* `--help`가 들어오면 사용법만 출력
* 잘못된 옵션은 에러 처리
* 옵션 파싱 로직은 `main.c` 안에 직접 쓰지 말고 `cli.c`로 분리

---

## 7. SQL 지원 문법

이번 버전의 SQL 문법은 아래로 고정한다.

### 7-1. INSERT

지원 형태 1:

```sql
INSERT INTO users VALUES (1, 'Alice', 20);
```

지원 형태 2:

```sql
INSERT INTO users (id, name, age) VALUES (1, 'Alice', 20);
```

부분 컬럼도 허용:

```sql
INSERT INTO users (name, id) VALUES ('Bob', 2);
```

### 7-2. SELECT

지원 형태 1:

```sql
SELECT * FROM users;
```

지원 형태 2:

```sql
SELECT id, name FROM users;
```

### 7-3. 이번 버전의 제약

* 파일당 **SQL 1문장만** 처리
* 세미콜론은 **있어도 되고 없어도 된다**
* 키워드는 대소문자 구분 없이 처리

  * `insert`, `INSERT`, `Insert` 모두 허용
* 식별자(table/column)는 schema 파일 기준으로 비교
* 리터럴은 아래 두 가지만 지원

  * 문자열: `'Alice'`
  * 숫자: `1`, `20`, `3.14`, `-7`

### 7-4. 문법 EBNF

```text
statement      ::= insert_stmt | select_stmt

insert_stmt    ::= INSERT INTO identifier
                   [ "(" identifier_list ")" ]
                   VALUES "(" literal_list ")"

select_stmt    ::= SELECT "*" FROM identifier
                 | SELECT identifier_list FROM identifier

identifier_list ::= identifier { "," identifier }
literal_list    ::= literal { "," literal }

literal        ::= string_literal | number_literal
identifier     ::= [a-zA-Z_][a-zA-Z0-9_]*
```

---

## 8. 데이터 저장 포맷 명세

테이블은 **2개의 파일**로 관리한다.

### 8-1. schema 파일

예: `db/users.schema`

```text
id
name
age
```

규칙:

* 한 줄에 하나의 컬럼명
* 빈 줄은 무시
* 순서가 곧 저장 순서다

### 8-2. data 파일

예: `db/users.data`

```text
1|Alice|20
2|Bob|25
```

규칙:

* 한 줄 = 한 row
* 컬럼 구분자는 `|`
* 저장 시 특수문자는 escape 처리

### 8-3. escape 규칙

저장 시 아래 규칙을 사용한다.

* `\` -> `\\`
* `|` -> `\|`
* 줄바꿈 -> `\n`

읽을 때는 다시 원복한다.

### 8-4. 중요한 설계 원칙

* **schema 파일이 테이블 존재 여부를 결정**한다
* `.data` 파일이 없으면

  * `INSERT` 시 새로 생성
  * `SELECT` 시 빈 테이블로 간주

---

## 9. AST와 핵심 데이터 구조

아래 구조체 이름을 고정해서 사용한다.

```c
// ast.h
typedef enum {
    VALUE_STRING,
    VALUE_NUMBER
} ValueType;

typedef struct {
    ValueType type;
    char *text;   // 숫자도 문자열로 저장
} LiteralValue;

typedef struct {
    char *table_name;
    char **columns;          // NULL 가능
    size_t column_count;     // 0이면 column list 생략
    LiteralValue *values;
    size_t value_count;
} InsertStatement;

typedef struct {
    char *table_name;
    int select_all;          // 1이면 SELECT *
    char **columns;          // select_all=0일 때 사용
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

추가로 실행용 구조체도 정의한다.

```c
// schema.h
typedef struct {
    char *table_name;
    char **columns;
    size_t column_count;
} TableSchema;

typedef struct {
    char **values;
    size_t value_count;
} Row;
```

```c
// result.h
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
```

---

## 10. 에러 코드 명세

```c
// errors.h
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

규칙:

* 성공 시 `0`
* 에러 메시지는 `stderr`로 출력
* 사용자 메시지는 사람이 읽을 수 있게 명확해야 한다

예:

* `LEX ERROR: unterminated string literal`
* `PARSE ERROR: expected FROM after select list`
* `SCHEMA ERROR: table 'users' not found`
* `EXEC ERROR: unknown column 'age2'`

---

# 11. 단계별 구현 명세

이제부터가 Codex가 실제로 따라야 하는 핵심 순서다.

---

## 단계 1. 프로젝트 스캐폴딩과 빌드 시스템 구성

### 목표

컴파일 가능한 빈 프로젝트를 먼저 만든다.

### 작업 파일

* `Makefile`
* `include/*.h`
* `src/*.c`
* `tests/*.c`

### 구현 요구

* C 표준은 `C99`
* 컴파일 옵션은 아래를 기본으로 사용

```bash
-std=c99 -Wall -Wextra -Werror
```

### Makefile 필수 타겟

* `make`
* `make test`
* `make clean`

### 완료 기준

* 빈 로직이어도 `make`가 성공해야 한다
* 테스트 바이너리도 함께 빌드되도록 구성한다

---

## 단계 2. CLI와 파일 읽기 구현

### 목표

실행 인자를 받고 SQL 파일 내용을 읽을 수 있게 한다.

### 관련 함수

```c
// cli.h
int parse_cli_args(int argc, char **argv, CliOptions *out_options);
void print_usage(const char *program_name);

// utils.h
char *read_text_file(const char *path);
char *trim_whitespace(char *text);
char *strdup_safe(const char *src);
void *xmalloc(size_t size);
```

### 세부 동작

* SQL 파일 전체를 문자열로 읽는다
* 빈 파일이면 에러 처리
* 메모리 할당 실패도 처리

### 완료 기준

* `./sql_processor -d ./db -f ./queries/select_users.sql` 형태로 실행 가능
* SQL 파일 내용이 정상적으로 메모리에 로드됨

---

## 단계 3. Lexer 구현

### 목표

SQL 문자열을 token 리스트로 변환한다.

### Token 타입

```c
// lexer.h
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
```

### 관련 함수

```c
int tokenize_sql(const char *sql, TokenArray *out_tokens, char *errbuf, size_t errbuf_size);
void free_token_array(TokenArray *tokens);
const char *token_type_name(TokenType type);
```

### lexer 규칙

* 공백, 탭, 개행은 무시
* 키워드는 대소문자 무시
* 문자열은 작은따옴표 기반
* 숫자는 부호와 소수점 허용
* `(` `)` `,` `;` `*` 처리
* 잘못된 문자는 즉시 에러

### 문자열 처리

아래는 지원해야 한다.

```sql
'Alice'
'hello world'
```

가능하면 이것도 지원한다.

```sql
'O''Reilly'
```

즉, SQL 스타일의 `''`를 단일 `'`로 해석한다.

### 완료 기준

다음 입력이 token화되어야 한다.

```sql
INSERT INTO users (id, name) VALUES (1, 'Alice');
SELECT * FROM users;
SELECT id, name FROM users;
```

---

## 단계 4. Parser와 AST 구현

### 목표

token 리스트를 AST `Statement` 구조체로 변환한다.

### 내부 Parser 구조체

```c
typedef struct {
    const TokenArray *tokens;
    size_t pos;
} Parser;
```

### 관련 함수

```c
// parser.h
int parse_statement(const TokenArray *tokens, Statement *out_stmt, char *errbuf, size_t errbuf_size);
void free_statement(Statement *stmt);
```

### parser 내부 helper

아래 함수들은 `static` internal helper로 구현한다.

```c
static const Token *parser_peek(Parser *parser);
static const Token *parser_previous(Parser *parser);
static const Token *parser_advance(Parser *parser);
static int parser_match(Parser *parser, TokenType type);
static int parser_expect(Parser *parser, TokenType type, char *errbuf, size_t errbuf_size);

static int parse_insert(Parser *parser, Statement *out_stmt, char *errbuf, size_t errbuf_size);
static int parse_select(Parser *parser, Statement *out_stmt, char *errbuf, size_t errbuf_size);

static int parse_identifier_list(Parser *parser, char ***out_items, size_t *out_count, char *errbuf, size_t errbuf_size);
static int parse_literal_list(Parser *parser, LiteralValue **out_items, size_t *out_count, char *errbuf, size_t errbuf_size);
```

### parser 동작

* 첫 토큰이 `INSERT`면 `parse_insert`
* 첫 토큰이 `SELECT`면 `parse_select`
* 그 외는 parse error

### validation 규칙

* `INSERT INTO table VALUES (...)`
* `INSERT INTO table (col1, col2) VALUES (...)`
* `SELECT * FROM table`
* `SELECT col1, col2 FROM table`

문법상 잘못된 경우 즉시 실패해야 한다.

### 완료 기준

AST 구조체에 아래 정보가 정확히 들어가야 한다.

* statement type
* table name
* 컬럼 리스트
* 값 리스트
* `SELECT *` 여부

---

## 단계 5. Schema 로더 구현

### 목표

schema metadata 파일을 읽어 테이블 구조를 로드한다.

### 관련 함수

```c
// schema.h
int load_table_schema(const char *db_dir, const char *table_name,
                      TableSchema *out_schema,
                      char *errbuf, size_t errbuf_size);

int schema_find_column_index(const TableSchema *schema, const char *column_name);
void free_table_schema(TableSchema *schema);
```

### 동작

* `<db_dir>/<table_name>.schema` 파일을 연다
* 각 줄을 컬럼명으로 읽는다
* 빈 줄은 무시
* 컬럼이 0개면 에러

### validation

* 중복 컬럼명은 에러
* schema 파일이 없으면 table not found 에러

### 완료 기준

`users.schema`를 읽으면 `id`, `name`, `age` 순서가 정확히 로드되어야 한다.

---

## 단계 6. Storage 구현

### 목표

row를 파일에 append하고, 모든 row를 다시 읽을 수 있어야 한다.

### 관련 함수

```c
// storage.h
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

### storage 내부 helper

아래 함수는 `static` 내부 구현으로 둔다.

```c
static char *escape_field(const char *value);
static char *unescape_field(const char *value);
static int split_escaped_row(const char *line, char ***out_fields, size_t *out_count);
```

### 동작

* `append_row_to_table`는 row를 schema 순서대로 저장
* `read_all_rows_from_table`는 모든 row를 읽어 메모리 구조로 반환
* 데이터 파일 row의 field 개수가 schema와 다르면 에러

### 완료 기준

* INSERT 후 `.data` 파일에 줄이 추가됨
* SELECT 시 `.data` 파일 내용이 정상 복원됨

---

## 단계 7. Executor 구현

### 목표

AST를 실제 동작으로 연결한다.

### 관련 함수

```c
// executor.h
int execute_statement(const char *db_dir, const Statement *stmt,
                      ExecResult *out_result,
                      char *errbuf, size_t errbuf_size);

void free_exec_result(ExecResult *result);
```

### executor 내부 helper

아래는 내부 helper로 구현한다.

```c
static int execute_insert(const char *db_dir, const InsertStatement *stmt,
                          ExecResult *out_result,
                          char *errbuf, size_t errbuf_size);

static int execute_select(const char *db_dir, const SelectStatement *stmt,
                          ExecResult *out_result,
                          char *errbuf, size_t errbuf_size);

static int build_insert_row(const TableSchema *schema, const InsertStatement *stmt,
                            Row *out_row,
                            char *errbuf, size_t errbuf_size);

static int validate_select_columns(const TableSchema *schema, const SelectStatement *stmt,
                                   char *errbuf, size_t errbuf_size);

static int project_rows_for_select(const TableSchema *schema, const SelectStatement *stmt,
                                   const Row *all_rows, size_t all_row_count,
                                   QueryResult *out_result,
                                   char *errbuf, size_t errbuf_size);
```

### INSERT 실행 규칙

1. schema 로드
2. 컬럼 유효성 검사
3. 최종 row 생성
4. `.data` 파일 보장
5. 파일 append
6. `affected_rows = 1`

### INSERT 상세 규칙

* 컬럼 리스트가 없는 경우
  `value_count == schema.column_count` 여야 한다
* 컬럼 리스트가 있는 경우
  각 컬럼을 schema index에 매핑해서 저장한다
* 명시되지 않은 컬럼은 빈 문자열 `""` 저장
* 중복 컬럼 지정은 에러
* 존재하지 않는 컬럼은 에러
* 컬럼 수와 값 수가 다르면 에러

### SELECT 실행 규칙

1. schema 로드
2. select 대상 컬럼 검증
3. 모든 row 읽기
4. `SELECT *`이면 전체 컬럼 반환
5. 특정 컬럼만 선택했으면 projection 수행
6. 결과를 `QueryResult`에 채운다

### 완료 기준

* `INSERT`가 파일에 반영된다
* `SELECT *`가 전체 데이터를 출력한다
* `SELECT id, name`이 부분 컬럼만 출력한다

---

## 단계 8. 결과 출력 구현

### 목표

실행 결과를 사람이 보기 좋은 형태로 출력한다.

### 관련 함수

```c
// result.h
void print_exec_result(const ExecResult *result);
```

### 출력 규칙

#### INSERT 성공 시

```text
INSERT 1
```

#### SELECT 성공 시

예:

```text
id | name  | age
----------------
1  | Alice | 20
2  | Bob   | 25

2 rows selected
```

정렬 폭은 단순 구현으로 가도 된다.
핵심은 **컬럼 헤더 + row 출력 + row count**가 보이는 것이다.

### 에러 출력

* `stderr` 사용
* 에러 prefix 포함

---

## 단계 9. main.c 조립

### 목표

지금까지 만든 모든 모듈을 연결한다.

### main 흐름

아래 순서 그대로 조립한다.

```c
int main(int argc, char **argv) {
    CliOptions options;
    char *sql_text = NULL;
    TokenArray tokens = {0};
    Statement stmt = {0};
    ExecResult result = {0};
    char errbuf[256] = {0};

    // 1. parse_cli_args
    // 2. read_text_file
    // 3. tokenize_sql
    // 4. parse_statement
    // 5. execute_statement
    // 6. print_exec_result
    // 7. free all resources
}
```

### 완료 기준

단일 실행 파일만으로 전체 흐름이 동작해야 한다.

---

# 12. 테스트 명세

테스트는 반드시 **단위 테스트 + 기능 테스트**를 모두 포함한다.

---

## 12-1. Unit Test: lexer

### 파일

* `tests/test_lexer.c`

### 테스트 항목

1. `INSERT INTO users VALUES (1, 'Alice');`
2. `SELECT * FROM users;`
3. `SELECT id, name FROM users;`
4. 문자열 literal 파싱
5. 숫자 literal 파싱
6. 잘못된 문자 입력 시 실패
7. 닫히지 않은 문자열 입력 시 실패

---

## 12-2. Unit Test: parser

### 파일

* `tests/test_parser.c`

### 테스트 항목

1. INSERT without column list
2. INSERT with column list
3. SELECT *
4. SELECT column list
5. `SELECT FROM users` 실패
6. `INSERT users VALUES (...)` 실패
7. 괄호/콤마 누락 실패

---

## 12-3. Unit Test: storage

### 파일

* `tests/test_storage.c`

### 테스트 항목

1. row append 후 file line 증가
2. escape/unescape 정상 동작
3. 빈 `.data` 파일 읽기
4. malformed row column count mismatch 에러

---

## 12-4. Unit Test: executor

### 파일

* `tests/test_executor.c`

### 테스트 항목

1. 정상 INSERT
2. 정상 SELECT *
3. 정상 SELECT partial columns
4. unknown table 에러
5. unknown column 에러
6. INSERT column/value count mismatch 에러
7. 부분 컬럼 INSERT 시 빈 문자열 보정 확인

---

## 12-5. 기능 테스트

### 파일

* `tests/test_integration.sh`

### 시나리오

1. `users.schema` 생성
2. `INSERT INTO users VALUES (1, 'Alice', 20);`
3. `INSERT INTO users (name, id) VALUES ('Bob', 2);`
4. `SELECT * FROM users;`
5. `SELECT name, id FROM users;`

### 확인 항목

* 실행 파일이 정상 종료되는가
* data 파일 내용이 기대값과 같은가
* SELECT 출력이 기대 컬럼/row를 포함하는가

---

# 13. 샘플 데이터와 샘플 SQL

## 샘플 schema

`db/users.schema`

```text
id
name
age
```

## 샘플 SQL

`queries/insert_users.sql`

```sql
INSERT INTO users VALUES (1, 'Alice', 20);
```

`queries/select_users.sql`

```sql
SELECT * FROM users;
```

`queries/select_user_names.sql`

```sql
SELECT id, name FROM users;
```

`queries/invalid_missing_from.sql`

```sql
SELECT id, name users;
```

---

# 14. README 명세

README는 발표 자료를 대신하므로 반드시 아래 내용을 포함한다.

## README 필수 목차

1. 프로젝트 소개
2. 지원 SQL 범위
3. 전체 동작 흐름
4. 디렉터리 구조
5. 빌드 방법
6. 실행 방법
7. 데이터 저장 포맷 설명
8. 테스트 방법
9. 예제 실행 결과
10. 한계와 향후 개선점
11. 추가 구현 사항(있다면)

## README에 반드시 들어갈 내용

* 왜 schema 파일과 data 파일을 분리했는지
* parser / executor / storage 책임 분리 설명
* 테스트 전략
* edge case 검증 내용
* 데모 순서

---

# 15. 발표용 데모 시나리오

발표는 4분이므로 README 기준으로 아래 순서가 가장 좋다.

1. 프로젝트 목표 20초
2. 구조 설명 40초
3. SQL 파일 입력 흐름 설명 40초
4. INSERT 데모 40초
5. SELECT 데모 40초
6. 테스트 소개 30초
7. 추가 구현 or 개선 포인트 30초

---

# 16. 엣지 케이스 명세

Codex는 아래 케이스를 반드시 고려해야 한다.

1. 빈 SQL 파일
2. 알 수 없는 키워드
3. 잘못된 토큰
4. 닫히지 않은 문자열
5. 존재하지 않는 테이블
6. 존재하지 않는 컬럼
7. INSERT 값 개수 불일치
8. schema 파일이 비어 있음
9. data 파일 row 형식 깨짐
10. 빈 테이블 SELECT
11. 공백/개행이 많은 SQL
12. 세미콜론 유무
13. 부분 컬럼 INSERT
14. 중복 컬럼 INSERT
15. `SELECT *`와 `SELECT col1, col2` 분기

---

# 17. 추가 구현 요소(차별화 포인트)

이건 **필수 기능이 모두 끝난 뒤**에만 진행한다.

우선순위는 아래 순서로 한다.

## 우선순위 1

`WHERE column = literal` 지원

예:

```sql
SELECT * FROM users WHERE id = 1;
```

## 우선순위 2

한 파일에 여러 SQL 문장 처리

## 우선순위 3

`-- comment` 무시

## 우선순위 4

출력 테이블 정렬 개선

## 우선순위 5

`NULL` 지원

주의:
필수 기능과 테스트가 모두 통과하기 전에는 bonus를 만들지 않는다.

---

# 18. Codex 작업 순서 요약

Codex는 반드시 아래 순서대로 작업한다.

1. 프로젝트 구조 + Makefile 생성
2. CLI + 파일 읽기
3. Lexer
4. Parser + AST
5. Schema 로더
6. Storage
7. Executor
8. Result 출력
9. Main 조립
10. Unit Test
11. Integration Test
12. README 작성
13. Bonus 기능은 마지막

---

# 19. Codex에게 줄 핵심 지시문

아래 문장을 프롬프트 맨 위에 붙이면 좋다.

```text
C로 파일 기반 SQL 처리기를 구현하라. 
반드시 INSERT와 SELECT를 지원하고, SQL 파일을 CLI로 입력받아 파싱 후 실행하라. 
스키마는 <table>.schema 파일로, 데이터는 <table>.data 파일로 관리하라. 
프로젝트를 lexer / parser / executor / storage / schema / result / cli 모듈로 분리하라. 
각 모듈의 함수 이름과 책임은 명세서에 맞춰라. 
필수 기능 구현 후 unit test와 integration test를 작성하고, 마지막에 README까지 작성하라. 
필수 기능이 모두 통과하기 전에는 bonus 기능을 구현하지 마라.
```

---

이 명세의 핵심은 **“어떤 기능을 만들지”**보다 **“어떤 순서로 어떤 파일과 함수로 쪼개서 만들지”**를 고정해 준다는 점입니다.
그래서 나중에 생성된 코드를 봐도 `main -> parser -> executor -> storage` 흐름이 바로 읽히고, 팀원끼리 설명하기도 훨씬 쉬워집니다.
