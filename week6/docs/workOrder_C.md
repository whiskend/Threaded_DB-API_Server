# SQL 처리기 C 작업 지시서

이 문서는 C 작업자 전용 지시서다. 구현 전 반드시 `docs/workOrder.md`를 먼저 읽는다.

## 1. 문서 사용 규칙

* 이 문서는 C 파트의 상세 범위만 다룬다.
* `docs/study.md`는 학습용이므로 읽지 않는다.
* 다른 파트 파일은 읽기만 가능하며 수정하지 않는다.

## 2. C 파트 목표

C 파트는 "AST를 실제 동작으로 실행하고 사용자에게 결과를 보여 주는 계층"을 완성하는 것이 목표다.

담당 범위:

* 에러 코드 정의
* executor
* result 출력
* `main.c` 조립
* executor 단위 테스트
* integration test
* 샘플 SQL 파일
* README

C 파트의 최종 산출물:

* `Statement`를 받아 `ExecResult`를 생성할 수 있음
* `INSERT`, `SELECT`가 사용자 관점에서 끝까지 동작함
* 실행 예시, 테스트 방법, 구조 설명이 README에 정리됨

## 3. C 파트 소유 파일

수정 가능 파일:

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

수정 금지 파일:

* `Makefile`
* `include/ast.h`
* `include/cli.h`
* `include/lexer.h`
* `include/parser.h`
* `include/utils.h`
* `include/schema.h`
* `include/storage.h`
* `src/cli.c`
* `src/lexer.c`
* `src/parser.c`
* `src/utils.c`
* `src/schema.c`
* `src/storage.c`
* `db/*`

## 4. C 파트가 제공해야 하는 데이터 계약

### 4-1. 에러 코드

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

보장해야 할 것:

* `main.c`는 최종 종료 코드를 이 enum 체계에 맞춰 반환한다.
* 에러 출력은 `stderr`를 사용한다.

### 4-2. 실행 결과

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
```

보장해야 할 것:

* `INSERT`는 `affected_rows = 1`
* `SELECT`는 헤더/row/row_count를 모두 출력 가능하도록 결과를 채운다

## 5. 구현해야 할 핵심 함수

### 5-1. `executor.c`

```c
int execute_statement(const char *db_dir, const Statement *stmt,
                      ExecResult *out_result,
                      char *errbuf, size_t errbuf_size);

void free_exec_result(ExecResult *result);
```

내부 static helper:

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

반드시 구현할 규칙:

* `INSERT`는 schema 로드 -> 컬럼 검증 -> row 생성 -> data 파일 보장 -> append 순서로 실행
* column list가 없으면 `value_count == schema.column_count`
* column list가 있으면 schema index에 맞춰 row를 재구성
* 지정되지 않은 컬럼은 빈 문자열 `""`
* 중복 컬럼, 존재하지 않는 컬럼, 값 개수 불일치는 에러
* `SELECT *`는 전체 컬럼 반환
* `SELECT col1, col2`는 projection 수행

### 5-2. `result.c`

```c
void print_exec_result(const ExecResult *result);
```

출력 규칙:

`INSERT` 성공

```text
INSERT 1
```

`SELECT` 성공 예

```text
id | name  | age
----------------
1  | Alice | 20
2  | Bob   | 25

2 rows selected
```

핵심:

* 컬럼 헤더 출력
* row 출력
* 마지막 row count 출력

### 5-3. `main.c`

상위 조립 순서:

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

`main.c`의 역할:

* 조립만 담당
* 세부 로직은 각 모듈 호출로 끝낸다
* 에러 발생 시 적절한 `StatusCode` 반환

## 6. 구현 순서

1. `include/errors.h`, `include/result.h`, `include/executor.h`를 공통 계약과 동일하게 작성한다.
2. `executor.c`를 구현한다.
3. `result.c`를 구현한다.
4. `main.c`를 조립한다.
5. `tests/test_executor.c`를 작성한다.
6. `queries/*.sql` 샘플을 만든다.
7. `tests/test_integration.sh`를 작성한다.
8. `README.md`를 작성한다.

## 7. 테스트 범위

### `tests/test_executor.c`

필수 케이스:

* 정상 INSERT
* 정상 SELECT `*`
* 정상 SELECT partial columns
* unknown table 에러
* unknown column 에러
* INSERT column/value count mismatch 에러
* 부분 컬럼 INSERT 시 빈 문자열 보정 확인

### `tests/test_integration.sh`

시나리오:

1. `users.schema` 준비
2. `INSERT INTO users VALUES (1, 'Alice', 20);`
3. `INSERT INTO users (name, id) VALUES ('Bob', 2);`
4. `SELECT * FROM users;`
5. `SELECT name, id FROM users;`

확인 항목:

* 실행 파일 정상 종료
* data 파일 내용 기대값 일치
* SELECT 출력에 기대 컬럼/row 포함

## 8. README 작성 범위

README에는 반드시 아래 항목을 포함한다.

* 프로젝트 소개
* 지원 SQL 범위
* 전체 동작 흐름
* 디렉터리 구조
* 빌드 방법
* 실행 방법
* 데이터 저장 포맷 설명
* 테스트 방법
* 예제 실행 결과
* 한계와 향후 개선점

README에서 설명해야 할 핵심:

* 왜 schema 파일과 data 파일을 분리했는지
* parser / executor / storage 책임 분리
* 테스트 전략
* edge case 검증 내용

## 9. 다른 파트와의 연결 포인트

### A 파트와의 연결

C는 A가 생성한 `Statement`만 신뢰하고 동작한다. 따라서 AST가 이미 문법적으로 유효하다는 가정 하에 실행 로직을 작성한다.

### B 파트와의 연결

C는 B가 제공한 schema/storage API만 사용한다. 직접 파일 포맷을 다시 해석하지 않는다.

## 10. 완료 기준

* C 소유 파일만 수정했다.
* `INSERT`, `SELECT` 실행과 출력이 끝까지 연결된다.
* executor 테스트와 integration test가 작성되어 있다.
* README와 샘플 SQL이 준비되어 있다.

## 11. Codex 실행 문구

Codex가 이 문서를 읽고 시작할 때는 아래처럼 이해하면 된다.

```text
나는 C 작업자다. docs/workOrder.md의 공통 계약을 따른다.
docs/workOrder_C.md 범위의 파일만 수정한다.
docs/study.md는 읽지 않는다.
목표는 Statement를 실행하고 결과를 출력하는 최종 파이프라인을 완성하는 것이다.
```
