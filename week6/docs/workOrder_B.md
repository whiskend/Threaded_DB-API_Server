# SQL 처리기 B 작업 지시서

이 문서는 B 작업자 전용 지시서다. 구현 전 반드시 `docs/workOrder.md`를 먼저 읽는다.

## 1. 문서 사용 규칙

* 이 문서는 B 파트의 상세 범위만 다룬다.
* `docs/study.md`는 학습용이므로 읽지 않는다.
* 다른 파트 파일은 읽기만 가능하며 수정하지 않는다.

## 2. B 파트 목표

B 파트는 "테이블 정의와 row 파일 저장소"를 완성하는 것이 목표다.

담당 범위:

* schema 파일 로드
* schema 유효성 검사
* `.data` 파일 생성
* row append
* row read
* escape / unescape
* 샘플 DB 파일 준비

B 파트의 최종 산출물:

* 테이블 스키마를 `TableSchema`로 로드 가능
* row를 `Row` 배열로 읽고 쓸 수 있음
* malformed data 파일에 대한 방어 로직 준비 완료

## 3. B 파트 소유 파일

수정 가능 파일:

* `include/schema.h`
* `include/storage.h`
* `src/schema.c`
* `src/storage.c`
* `tests/test_storage.c`
* `db/users.schema`
* `db/products.schema`
* 필요 시 추가 샘플 `.schema`, `.data`

수정 금지 파일:

* `Makefile`
* `include/ast.h`
* `include/cli.h`
* `include/lexer.h`
* `include/parser.h`
* `include/utils.h`
* `include/errors.h`
* `include/executor.h`
* `include/result.h`
* `src/cli.c`
* `src/lexer.c`
* `src/parser.c`
* `src/utils.c`
* `src/executor.c`
* `src/result.c`
* `src/main.c`
* `README.md`
* `queries/*`

## 4. B 파트가 제공해야 하는 데이터 계약

### 4-1. 테이블 스키마

```c
typedef struct {
    char *table_name;
    char **columns;
    size_t column_count;
} TableSchema;
```

보장해야 할 것:

* `<db_dir>/<table_name>.schema`를 읽어 컬럼 순서를 그대로 유지한다.
* 빈 줄은 무시한다.
* 중복 컬럼은 에러다.
* 컬럼이 하나도 없으면 에러다.

### 4-2. Row 표현

```c
typedef struct {
    char **values;
    size_t value_count;
} Row;
```

보장해야 할 것:

* `values` 순서는 schema 순서와 정확히 일치한다.
* `read_all_rows_from_table`가 반환하는 모든 row는 동일한 `value_count`를 가진다.
* malformed row는 즉시 에러 처리한다.

## 5. 구현해야 할 핵심 함수

### 5-1. `schema.c`

```c
int load_table_schema(const char *db_dir, const char *table_name,
                      TableSchema *out_schema,
                      char *errbuf, size_t errbuf_size);

int schema_find_column_index(const TableSchema *schema, const char *column_name);
void free_table_schema(TableSchema *schema);
```

반드시 처리할 것:

* schema 파일 없음 -> table not found 에러
* 빈 줄 무시
* 중복 컬럼 에러
* 0개 컬럼 에러

### 5-2. `storage.c`

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

내부 static helper:

```c
static char *escape_field(const char *value);
static char *unescape_field(const char *value);
static int split_escaped_row(const char *line, char ***out_fields, size_t *out_count);
```

반드시 처리할 것:

* data 파일이 없으면 `INSERT` 전에 생성 가능해야 한다.
* data 파일이 없으면 `SELECT`는 빈 테이블로 읽힐 수 있어야 한다.
* field 구분자는 `|`
* escape 규칙은 아래를 따른다.

```text
\  -> \\
|  -> \|
\n -> 실제 줄바꿈의 역직렬화
```

## 6. 구현 순서

1. `include/schema.h`, `include/storage.h`를 공통 계약과 동일하게 작성한다.
2. `schema.c`를 먼저 구현한다.
3. `storage.c`의 escape / split helper를 구현한다.
4. append / read 경로를 구현한다.
5. `tests/test_storage.c`를 작성한다.
6. 샘플 DB 파일을 추가한다.

## 7. 테스트 범위

### `tests/test_storage.c`

필수 케이스:

* row append 후 file line 증가
* escape / unescape 정상 동작
* 빈 `.data` 파일 읽기
* data 파일 없음 처리
* malformed row column count mismatch 에러
* schema 중복 컬럼 에러

### 샘플 DB 파일

최소 준비 파일:

* `db/users.schema`
* `db/products.schema`

권장 예시:

`db/users.schema`

```text
id
name
age
```

## 8. 다른 파트와의 연결 포인트

### A 파트와의 연결

B는 SQL 문법이나 AST를 알 필요가 없다. `Statement`를 직접 해석하지 않는다.

### C 파트와의 연결

C는 B가 제공하는 `TableSchema`, `Row`, `Row[]`를 그대로 사용한다. 따라서 아래를 지켜야 한다.

* `schema_find_column_index`는 존재하지 않는 컬럼에 대해 음수를 반환한다.
* `read_all_rows_from_table`는 성공 시 `out_rows`, `out_row_count`를 채운다.
* 빈 data 파일과 존재하지 않는 data 파일을 C가 구분 없이 빈 테이블처럼 다룰 수 있게 한다.

## 9. 완료 기준

* B 소유 파일만 수정했다.
* schema와 storage가 파일 기반 입출력을 독립적으로 수행한다.
* escape/unescape가 round-trip 가능하다.
* storage 테스트가 작성되어 있다.

## 10. Codex 실행 문구

Codex가 이 문서를 읽고 시작할 때는 아래처럼 이해하면 된다.

```text
나는 B 작업자다. docs/workOrder.md의 공통 계약을 따른다.
docs/workOrder_B.md 범위의 파일만 수정한다.
docs/study.md는 읽지 않는다.
목표는 schema와 data 파일 계층을 안정적으로 제공하는 것이다.
```
