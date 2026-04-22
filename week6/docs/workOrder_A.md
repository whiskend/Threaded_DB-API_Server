# SQL 처리기 A 작업 지시서

이 문서는 A 작업자 전용 지시서다. 구현 전 반드시 `docs/workOrder.md`를 먼저 읽는다.

## 1. 문서 사용 규칙

* 이 문서는 A 파트의 상세 범위만 다룬다.
* `docs/study.md`는 학습용이므로 읽지 않는다.
* 다른 파트 파일은 읽기만 가능하며 수정하지 않는다.

## 2. A 파트 목표

A 파트는 "입력 문자열을 실행 가능한 AST로 바꾸는 전 단계"를 끝내는 것이 목표다.

담당 범위:

* 프로젝트 빌드 스캐폴딩
* CLI 옵션 파싱
* 파일 읽기 유틸
* SQL lexer
* SQL parser
* AST 메모리 해제

A 파트의 최종 산출물:

* SQL 파일 문자열을 읽어 `Statement`까지 변환 가능
* lexer/parser 단위 테스트 준비 완료
* 다른 파트가 그대로 가져다 쓸 수 있는 public header 준비 완료

## 3. A 파트 소유 파일

수정 가능 파일:

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

수정 금지 파일:

* `include/schema.h`
* `include/storage.h`
* `include/errors.h`
* `include/executor.h`
* `include/result.h`
* `src/schema.c`
* `src/storage.c`
* `src/executor.c`
* `src/result.c`
* `src/main.c`
* `README.md`
* `db/*`
* `queries/*`

## 4. A 파트가 제공해야 하는 데이터 계약

A는 아래 세 가지를 다음 파트에 넘긴다.

### 4-1. CLI 결과

```c
typedef struct {
    char *db_dir;
    char *sql_file;
    int help_requested;
} CliOptions;
```

보장해야 할 것:

* `-d`, `--db`, `-f`, `--file`, `-h`, `--help` 지원
* 인자 오류 시 `STATUS_INVALID_ARGS`에 맞게 동작할 수 있도록 실패 코드 반환
* `main.c`가 직접 옵션 파싱 로직을 갖지 않게 한다

### 4-2. Lexer 결과

```c
typedef struct {
    Token *items;
    size_t count;
    size_t capacity;
} TokenArray;
```

보장해야 할 것:

* 키워드 대소문자 무시
* 공백/탭/개행 무시
* `TOKEN_EOF`까지 포함한 안정적인 토큰 배열 생성
* 문자열과 숫자 리터럴 텍스트가 그대로 유지됨

### 4-3. Parser 결과

```c
typedef struct {
    StatementType type;
    union {
        InsertStatement insert_stmt;
        SelectStatement select_stmt;
    };
} Statement;
```

보장해야 할 것:

* `INSERT`와 `SELECT`를 정확히 구분
* table 이름, column 목록, literal 목록이 AST에 채워짐
* `SELECT *`는 `select_all = 1`
* parser 성공 시 실행기가 추가 해석 없이 바로 사용할 수 있게 정규화

## 5. 구현해야 할 핵심 함수

### 5-1. `cli.c`

```c
int parse_cli_args(int argc, char **argv, CliOptions *out_options);
void print_usage(const char *program_name);
```

완료 조건:

* 인자 부족 시 실패
* `--help`는 사용법만 출력 가능하도록 표시
* 알 수 없는 옵션은 실패

### 5-2. `utils.c`

```c
char *read_text_file(const char *path);
char *trim_whitespace(char *text);
char *strdup_safe(const char *src);
void *xmalloc(size_t size);
```

완료 조건:

* SQL 파일 전체를 읽는다
* 빈 파일 처리 규칙을 C 파트가 바로 쓸 수 있게 명확한 실패를 제공한다
* 메모리 할당 실패에 대비한다

### 5-3. `lexer.c`

```c
int tokenize_sql(const char *sql, TokenArray *out_tokens, char *errbuf, size_t errbuf_size);
void free_token_array(TokenArray *tokens);
const char *token_type_name(TokenType type);
```

반드시 처리할 것:

* `INSERT`, `INTO`, `VALUES`, `SELECT`, `FROM`
* `(` `)` `,` `;` `*`
* `'Alice'`, `'hello world'`
* `1`, `20`, `3.14`, `-7`
* 닫히지 않은 문자열 에러
* 잘못된 문자 에러

가능하면 지원:

* `O''Reilly` 형식의 SQL string escape

### 5-4. `parser.c`

```c
int parse_statement(const TokenArray *tokens, Statement *out_stmt, char *errbuf, size_t errbuf_size);
void free_statement(Statement *stmt);
```

내부 static helper:

```c
static const Token *parser_peek(Parser *parser);
static const Token *parser_previous(Parser *parser);
static const Token *parser_advance(Parser *parser);
static int parser_match(Parser *parser, TokenType type);
static int parser_expect(Parser *parser, TokenType type, char *errbuf, size_t errbuf_size);

static int parse_insert(Parser *parser, Statement *out_stmt, char *errbuf, size_t errbuf_size);
static int parse_select(Parser *parser, Statement *out_stmt, char *errbuf, size_t errbuf_size);

static int parse_identifier_list(Parser *parser, char ***out_items, size_t *out_count,
                                 char *errbuf, size_t errbuf_size);
static int parse_literal_list(Parser *parser, LiteralValue **out_items, size_t *out_count,
                              char *errbuf, size_t errbuf_size);
```

반드시 지원할 문법:

```sql
INSERT INTO users VALUES (1, 'Alice', 20);
INSERT INTO users (id, name, age) VALUES (1, 'Alice', 20);
INSERT INTO users (name, id) VALUES ('Bob', 2);
SELECT * FROM users;
SELECT id, name FROM users;
```

반드시 실패해야 할 예:

```sql
SELECT FROM users;
INSERT users VALUES (1);
SELECT id, name users;
```

## 6. 구현 순서

1. `Makefile`부터 만든다.
2. `include/ast.h`, `include/cli.h`, `include/lexer.h`, `include/parser.h`, `include/utils.h`를 공통 계약과 동일하게 작성한다.
3. `utils.c`와 `cli.c`를 먼저 구현한다.
4. `lexer.c`를 구현한다.
5. `parser.c`를 구현한다.
6. `test_lexer.c`, `test_parser.c`를 작성한다.

## 7. 테스트 범위

### `tests/test_lexer.c`

필수 케이스:

* `INSERT INTO users VALUES (1, 'Alice');`
* `SELECT * FROM users;`
* `SELECT id, name FROM users;`
* 문자열 literal 파싱
* 숫자 literal 파싱
* 잘못된 문자 실패
* 닫히지 않은 문자열 실패

### `tests/test_parser.c`

필수 케이스:

* INSERT without column list
* INSERT with column list
* SELECT `*`
* SELECT column list
* `SELECT FROM users` 실패
* `INSERT users VALUES (...)` 실패
* 괄호/콤마 누락 실패

## 8. 다른 파트와의 연결 포인트

### B 파트와의 연결

A는 직접 `schema`나 `storage`를 호출하지 않는다.

### C 파트와의 연결

C는 A가 만든 `Statement`를 그대로 받아 실행한다. 따라서 아래를 지켜야 한다.

* `INSERT`의 `columns == NULL` 또는 `column_count == 0`이면 column list 생략으로 해석 가능해야 한다.
* `LiteralValue.text`에는 원본 값 문자열이 들어 있어야 한다.
* `SELECT *`는 `select_all = 1`, `column_count = 0`으로 두어도 일관되게 해석 가능해야 한다.

## 9. 완료 기준

* A 소유 파일만 수정했다.
* `Statement` 구조체가 명세 문법을 정확히 표현한다.
* lexer/parser 테스트가 작성되어 있다.
* 빌드 설정이 C99와 `-Wall -Wextra -Werror`를 사용한다.

## 10. Codex 실행 문구

Codex가 이 문서를 읽고 시작할 때는 아래처럼 이해하면 된다.

```text
나는 A 작업자다. docs/workOrder.md의 공통 계약을 따른다.
docs/workOrder_A.md 범위의 파일만 수정한다.
docs/study.md는 읽지 않는다.
목표는 SQL 파일 문자열을 Statement까지 안정적으로 변환하는 것이다.
```
