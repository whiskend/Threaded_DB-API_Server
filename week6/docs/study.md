좋아. 이번에는 **“Codex에게 시키기 위한 명세서”**가 아니라,
**네가 읽으면서 전체 구조를 Top-Down으로 이해할 수 있는 학습용 설명서**로 써줄게.

즉, 순서를 이렇게 가져가겠다.

1. 이 프로젝트가 본질적으로 뭘 만드는 건지
2. 전체 흐름이 어떻게 굴러가는지
3. 각 모듈이 어떤 책임을 가지는지
4. 데이터가 어떤 모양으로 이동하는지
5. 마지막에 각 함수가

   * 언제 호출되는지
   * 어디서 호출되는지
   * 왜 필요한지
   * 어떤 인자를 받는지
   * 무엇을 반환하는지
   * 함수 이름의 의미가 뭔지
     까지 내려간다.

---

# SQL 처리기 학습용 설명서

---

# 1. 이 프로젝트는 본질적으로 무엇을 만드는가

이 프로젝트는 한마디로 말하면 **“SQL 문장을 읽어서, 해석해서, 실제 파일에 반영하는 프로그램”**이다.

보통 DBMS는 내부적으로 굉장히 복잡하지만, 이번 과제에서는 아주 작은 규모로 그 핵심 흐름만 직접 구현하는 것이다.

즉, 우리가 만드는 것은 이런 프로그램이다.

* 사용자가 SQL 파일을 준다
* 프로그램이 그 SQL을 읽는다
* SQL이 무슨 뜻인지 분석한다
* 그 뜻에 따라 파일에 데이터를 저장하거나 읽는다
* 결과를 화면에 출력한다

예를 들어 사용자가 이런 SQL 파일을 준다고 하자.

```sql
INSERT INTO users VALUES (1, 'Alice', 20);
```

그러면 프로그램은 이 문장을 보고 이렇게 생각해야 한다.

* 아, 이건 `INSERT`구나
* `users` 테이블에 넣으라는 뜻이구나
* 값은 `1`, `Alice`, `20`이구나
* 그럼 `users.data` 파일에 한 줄 추가하면 되겠네

반대로 이런 SQL이라면

```sql
SELECT id, name FROM users;
```

이렇게 생각해야 한다.

* 아, 이건 `SELECT`구나
* `users` 테이블에서 읽으라는 뜻이구나
* 모든 컬럼이 아니라 `id`, `name`만 보여주라는 뜻이구나
* 그럼 `users.data` 파일을 읽고 필요한 컬럼만 골라 출력해야겠네

즉, 이 프로젝트의 핵심은 **SQL을 “문자열”로 보지 않고 “명령”으로 바꾸는 것**이다.

---

# 2. 가장 위에서 본 전체 흐름

이 프로젝트 전체는 아래 5단계로 보면 된다.

```text
입력 -> 파싱 -> 실행 -> 저장/조회 -> 출력
```

조금 더 자세히 쓰면 이렇다.

```text
1. CLI로 SQL 파일 경로를 받는다
2. SQL 파일 내용을 읽는다
3. SQL 문자열을 분석해서 구조화한다
4. 구조화된 결과를 실제 동작으로 실행한다
5. 결과를 사용자에게 출력한다
```

이 흐름을 코드 관점에서 보면 보통 이렇게 된다.

```text
main
 ├─ CLI 인자 파싱
 ├─ SQL 파일 읽기
 ├─ Lexer: 문자열을 token으로 나누기
 ├─ Parser: token을 SQL 구조체(AST)로 만들기
 ├─ Executor: 구조체를 실제 동작으로 실행하기
 ├─ Storage: 파일에 저장/읽기
 └─ Result: 결과 출력
```

---

# 3. 왜 이런 구조로 나누는가

처음 배우는 입장에서는 “그냥 main에 다 쓰면 안 되나?”라는 생각이 들 수 있다.
그런데 그렇게 하면 코드가 금방 엉킨다.

예를 들어 `SELECT id, name FROM users;`를 처리한다고 하자.

이 한 줄 안에는 사실 여러 일이 섞여 있다.

* 문자열 읽기
* 문법 분석
* `SELECT`인지 판단
* 테이블 이름 찾기
* 컬럼 이름 찾기
* schema 읽기
* data 파일 읽기
* 필요한 컬럼만 추출
* 출력 형식 만들기

이걸 한 함수에 몰아넣으면 “문자열 처리 코드”, “파일 처리 코드”, “출력 코드”가 다 섞인다.
그러면 디버깅도 어렵고 테스트도 어렵다.

그래서 역할을 나눈다.

* **CLI**: 사용자의 실행 옵션 처리
* **Lexer**: 문자열을 작은 조각(token)으로 자르기
* **Parser**: 그 조각들의 문법을 이해해서 구조체로 바꾸기
* **Executor**: 구조체의 의미를 실제 실행으로 연결하기
* **Schema**: 테이블 구조 읽기
* **Storage**: 데이터 파일 읽고 쓰기
* **Result**: 결과 출력

즉, 이 구조는 “복잡한 문제를 역할별로 분해한 것”이다.

---

# 4. 이 프로젝트를 한 문장으로 다시 설명하면

이 프로젝트는

> **문자열 SQL을 구조화된 명령으로 바꾸고, 그 명령을 파일 시스템 위에서 실행하는 작은 DB 처리기**

라고 볼 수 있다.

---

# 5. 입력부터 출력까지, 데이터가 어떻게 변하는가

이제 진짜 중요한 흐름이다.

우리가 보는 SQL은 처음에는 그냥 문자열이다.

예:

```sql
SELECT id, name FROM users;
```

이 문자열은 프로그램 안에서 단계적으로 모양이 바뀐다.

---

## 5-1. 1단계: 원본 문자열

처음엔 그냥 문자 덩어리다.

```c
"SELECT id, name FROM users;"
```

이 상태에서는 컴퓨터가 “무슨 뜻인지” 이해하지 못한다.
그냥 글자일 뿐이다.

---

## 5-2. 2단계: Token 목록

Lexer가 문자열을 잘게 쪼갠다.

```text
[SELECT] [id] [,] [name] [FROM] [users] [;]
```

즉, “의미 있는 단위”로 분해한다.

이걸 tokenization이라고 본다.

---

## 5-3. 3단계: AST(구조화된 명령)

Parser는 token 목록을 보고 구조를 만든다.

예를 들어 이런 구조체가 만들어진다.

```c
type = STMT_SELECT
table_name = "users"
select_all = 0
columns = ["id", "name"]
```

이제 프로그램은 더 이상 이 SQL을 “문자열”로 다루지 않는다.
**의미가 정리된 구조체**로 다룬다.

이게 핵심이다.

---

## 5-4. 4단계: 실행용 데이터

Executor는 AST를 보고 실제로 해야 할 일을 결정한다.

예를 들면

* `users.schema`를 읽는다
* `users.data`를 읽는다
* `id`, `name` 컬럼의 위치를 알아낸다
* 각 row에서 필요한 값만 뽑는다

즉, AST는 “의미”, Executor는 “행동”이다.

---

## 5-5. 5단계: 출력 결과

마지막에는 사람이 보는 결과가 된다.

```text
id | name
---------
1  | Alice
2  | Bob

2 rows selected
```

즉, 흐름은 이렇게 요약된다.

```text
문자열 -> token -> AST -> 실행 -> 출력
```

---

# 6. 모듈별로 이 시스템을 이해하기

이제 각 모듈을 위에서 아래로 보자.

---

## 6-1. main 모듈

### 역할

프로그램 전체 흐름을 연결하는 “지휘자” 역할이다.

### main이 하는 일

* CLI 인자를 받는다
* SQL 파일을 읽는다
* Lexer를 호출한다
* Parser를 호출한다
* Executor를 호출한다
* 결과를 출력한다
* 메모리를 해제한다

### main이 하지 말아야 할 일

* 직접 SQL 문법을 분석하면 안 된다
* 직접 파일 row parsing을 하면 안 된다
* 직접 schema 유효성 검사를 세세하게 하면 안 된다

즉, main은 “실무자”가 아니라 “오케스트라 지휘자”다.

---

## 6-2. CLI 모듈

### 역할

사용자가 프로그램을 어떻게 실행했는지 해석한다.

예:

```bash
./sql_processor -d ./db -f ./queries/select_users.sql
```

CLI 모듈은 여기서

* DB 경로는 `./db`
* SQL 파일은 `./queries/select_users.sql`

라는 정보를 뽑아낸다.

### 핵심 의미

프로그램 바깥 세상과 프로그램 안쪽 로직 사이의 첫 번째 연결점이다.

---

## 6-3. Lexer 모듈

### 역할

SQL 문자열을 token으로 쪼갠다.

예:

```sql
INSERT INTO users VALUES (1, 'Alice', 20);
```

를

```text
INSERT / INTO / users / VALUES / ( / 1 / , / 'Alice' / , / 20 / ) / ;
```

처럼 분해한다.

### 핵심 의미

컴파일러로 치면 “문자열을 읽어서 어휘 단위로 자르는 단계”다.

### 왜 필요한가

Parser는 문자열을 바로 다루기보다 token을 다루는 게 훨씬 쉽다.
문자열을 직접 문법 분석하면 너무 복잡하다.

---

## 6-4. Parser 모듈

### 역할

Token 목록이 SQL 문법상 어떤 의미인지 분석한다.

예를 들어

```text
SELECT id, name FROM users;
```

라는 token 배열을 보고

* 이건 SELECT문이다
* 테이블은 users다
* 컬럼은 id, name이다

라고 구조화한다.

### 핵심 의미

**문법을 이해하는 단계**다.

### 왜 필요한가

Lexer는 단어를 잘라줄 뿐이고,
Parser는 그 단어들이 어떤 문장 구조를 이루는지 이해한다.

---

## 6-5. AST 모듈

### 역할

Parser가 이해한 SQL 문장의 구조를 담는 데이터 구조를 제공한다.

예를 들어 `Statement`, `InsertStatement`, `SelectStatement` 같은 구조체가 여기에 들어간다.

### 핵심 의미

AST는 **“SQL 문장을 프로그램이 이해한 결과물”**이다.

즉, Parser의 출력물이고 Executor의 입력물이다.

---

## 6-6. Schema 모듈

### 역할

테이블 구조를 파일에서 읽는다.

예를 들어 `users.schema`가 이렇게 생겼다면

```text
id
name
age
```

Schema 모듈은 이걸 읽어서

```c
columns = ["id", "name", "age"]
```

로 만든다.

### 핵심 의미

이 테이블이 어떤 컬럼 순서를 가지는지 알려준다.

### 왜 필요한가

데이터 파일은 단순히 한 줄씩 들어 있을 뿐이다.
그 한 줄의 첫 번째 값이 `id`인지, `name`인지 알려면 schema가 필요하다.

---

## 6-7. Storage 모듈

### 역할

실제 `.data` 파일에 데이터를 저장하거나 읽는다.

예:

```text
1|Alice|20
2|Bob|25
```

### INSERT 때 하는 일

새 row를 파일 끝에 추가한다.

### SELECT 때 하는 일

파일 전체를 읽고 row 목록으로 복원한다.

### 핵심 의미

“파일 기반 DB”라는 말의 실제 구현 담당이다.

---

## 6-8. Executor 모듈

### 역할

AST를 보고 실제 행동을 수행한다.

예를 들어 AST가 `INSERT users VALUES (...)`라면

* schema를 읽고
* 값 개수를 맞추고
* 저장할 row를 만들고
* 파일에 append 한다

AST가 `SELECT id, name FROM users`라면

* schema를 읽고
* data 파일을 읽고
* 필요한 컬럼만 골라서
* 결과 구조체를 만든다

### 핵심 의미

이 프로젝트의 “엔진”이다.

Parser가 “무슨 뜻인지” 해석했다면
Executor는 “그래서 실제로 무엇을 할지” 수행한다.

---

## 6-9. Result 모듈

### 역할

실행 결과를 사람이 보기 좋게 출력한다.

예:

* `INSERT 1`
* 표 형태의 SELECT 결과

### 핵심 의미

프로그램의 마지막 사용자 인터페이스다.

---

# 7. 이 프로젝트에서 가장 중요한 추상화 3개

이 프로젝트를 공부할 때 꼭 잡아야 할 핵심 개념이 있다.

---

## 7-1. Token

문자열을 의미 단위로 자른 것

예:

* `SELECT`
* `FROM`
* `users`
* `,`
* `(`

---

## 7-2. AST

SQL 문장의 구조를 프로그램이 이해한 결과

예:

* statement type = SELECT
* table = users
* columns = [id, name]

---

## 7-3. Row

파일에 저장되거나 파일에서 읽혀 나온 실제 데이터 한 줄

예:

* `["1", "Alice", "20"]`

---

# 8. INSERT를 Top-Down으로 따라가 보기

이제 실제 흐름을 하나 따라가 보자.

SQL:

```sql
INSERT INTO users (name, id) VALUES ('Bob', 2);
```

---

## 8-1. main 단계

* SQL 파일을 읽는다
* Lexer 호출
* Parser 호출
* Executor 호출

---

## 8-2. Lexer 단계

이 SQL을 token으로 자른다.

```text
INSERT INTO users ( name , id ) VALUES ( 'Bob' , 2 ) ;
```

---

## 8-3. Parser 단계

이 token들을 보고 AST를 만든다.

```c
type = STMT_INSERT
table_name = "users"
columns = ["name", "id"]
values = ["Bob", "2"]
```

---

## 8-4. Executor 단계

Executor는 이 AST를 보고 생각한다.

* `users` 테이블 schema를 읽자
* schema 순서는 `id`, `name`, `age` 라고 하자
* 그런데 입력 컬럼은 `name`, `id` 순서네?
* 그럼 최종 저장 row는 schema 순서대로 다시 맞춰야겠다

최종 row:

```text
["2", "Bob", ""]
```

여기서 `age`는 입력되지 않았으니 빈 문자열로 채운다.

---

## 8-5. Storage 단계

최종 row를 파일에 저장한다.

```text
2|Bob|
```

---

## 8-6. Result 단계

성공 메시지 출력

```text
INSERT 1
```

---

# 9. SELECT를 Top-Down으로 따라가 보기

SQL:

```sql
SELECT id, name FROM users;
```

---

## 9-1. Parser 결과

```c
type = STMT_SELECT
table_name = "users"
select_all = 0
columns = ["id", "name"]
```

---

## 9-2. Executor 단계

* schema 읽기: `id`, `name`, `age`
* data 읽기:

  * `1|Alice|20`
  * `2|Bob|25`
* `id`, `name`의 위치 찾기

  * id는 0번
  * name은 1번
* 각 row에서 필요한 값만 projection

결과:

```text
["1", "Alice"]
["2", "Bob"]
```

---

## 9-3. Result 단계

출력:

```text
id | name
---------
1  | Alice
2  | Bob
```

---

# 10. 자료구조를 이해해야 전체 코드가 읽힌다

이제 Low-level로 내려가자.

---

## 10-1. LiteralValue

### 의미

SQL 안의 “값 하나”를 표현한다.

예:

* `1`
* `'Alice'`
* `20`

### 왜 필요한가

INSERT의 values 목록은 값들의 집합이기 때문이다.

### 구조 예시

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

### 해석

* `type`: 이 값이 문자열인지 숫자인지
* `text`: 실제 문자열 표현

숫자도 문자열로 저장하는 이유는 구현을 단순하게 하기 위해서다.

---

## 10-2. InsertStatement

### 의미

INSERT 문 전체를 담는 구조체

```c
typedef struct {
    char *table_name;
    char **columns;
    size_t column_count;
    LiteralValue *values;
    size_t value_count;
} InsertStatement;
```

### 해석

* `table_name`: 어느 테이블에 넣을지
* `columns`: 명시된 컬럼들
* `column_count`: 컬럼 개수
* `values`: 넣을 값들
* `value_count`: 값 개수

### 특징

컬럼 리스트가 없는 경우도 있으므로 `columns == NULL`일 수 있다.

---

## 10-3. SelectStatement

```c
typedef struct {
    char *table_name;
    int select_all;
    char **columns;
    size_t column_count;
} SelectStatement;
```

### 해석

* `table_name`: 어디서 읽을지
* `select_all`: `SELECT *`인지 여부
* `columns`: 선택된 컬럼들
* `column_count`: 컬럼 개수

---

## 10-4. Statement

### 의미

INSERT와 SELECT를 공통으로 담는 최상위 SQL 문 구조체

```c
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

### 왜 필요한가

Parser는 “무슨 문장인지 아직 모르다가” 분석 후 INSERT나 SELECT 중 하나를 만든다.
Executor도 “statement type”을 보고 분기한다.

즉, SQL 문 전체를 다루는 공통 껍데기다.

---

## 10-5. TableSchema

```c
typedef struct {
    char *table_name;
    char **columns;
    size_t column_count;
} TableSchema;
```

### 의미

테이블 구조 정보

### 왜 필요한가

* 컬럼 존재 여부 검사
* 컬럼 위치 찾기
* INSERT 값 재배치
* SELECT projection

---

## 10-6. Row

```c
typedef struct {
    char **values;
    size_t value_count;
} Row;
```

### 의미

데이터 한 줄

### 예시

`1|Alice|20`이면

```c
values = ["1", "Alice", "20"]
value_count = 3
```

---

## 10-7. ExecResult / QueryResult

### 의미

실행 결과를 담는다.

INSERT는 “영향받은 row 수”가 중요하고
SELECT는 “컬럼 목록 + row 목록”이 중요하다.

즉, 실행 후 출력에 필요한 데이터를 담는 구조체다.

---

# 11. 함수는 어떤 계층으로 나뉘는가

이제 함수들을 보기 전에 먼저 계층을 나눠보자.

---

## 11-1. 최상위 orchestration 함수

전체 흐름을 연결한다.

예:

* `main`
* `execute_statement`

---

## 11-2. 변환 함수

한 형태의 데이터를 다른 형태로 바꾼다.

예:

* `read_text_file`
* `tokenize_sql`
* `parse_statement`
* `load_table_schema`
* `read_all_rows_from_table`

---

## 11-3. 실행 보조 함수

실행에 필요한 하위 작업을 담당한다.

예:

* `build_insert_row`
* `validate_select_columns`
* `project_rows_for_select`

---

## 11-4. 유틸리티 함수

중복되는 작은 작업을 처리한다.

예:

* `trim_whitespace`
* `strdup_safe`
* `xmalloc`

---

# 12. 이제 함수별로 가장 위에서부터 내려가자

---

## 12-1. `main`

### 의미

프로그램의 시작점

### 언제 호출되는가

프로그램이 실행되면 운영체제가 가장 먼저 호출한다.

### 어디서 호출되는가

직접 호출되지 않는다. 프로그램 시작 시 자동 호출된다.

### 왜 필요한가

전체 흐름을 조립하는 진입점이 필요하기 때문이다.

### 형태 예시

```c
int main(int argc, char **argv);
```

### 인자

* `argc`: CLI 인자 개수
* `argv`: CLI 인자 문자열 배열

### 반환

* `0`: 성공
* 0이 아닌 값: 실패

### main이 하는 일

1. 옵션 파싱
2. SQL 파일 읽기
3. token화
4. 파싱
5. 실행
6. 결과 출력
7. 메모리 정리

---

## 12-2. `parse_cli_args`

### 의미

CLI 인자를 해석한다
이름 그대로 “command line arguments를 parse한다”

### 언제 호출되는가

`main` 초반부

### 어디서 호출되는가

`main`

### 왜 필요한가

실행 옵션을 구조체에 정리해두면 이후 코드가 훨씬 단순해지기 때문이다.

### 시그니처 예시

```c
int parse_cli_args(int argc, char **argv, CliOptions *out_options);
```

### 인자 설명

* `argc`: 인자 개수
* `argv`: 인자 목록
* `out_options`: 파싱 결과를 저장할 구조체

### 반환

* 성공 시 `STATUS_OK`
* 실패 시 `STATUS_INVALID_ARGS`

### 함수가 하는 일

* `-d`, `--db` 읽기
* `-f`, `--file` 읽기
* `-h`, `--help` 처리
* 잘못된 옵션 감지

---

## 12-3. `print_usage`

### 의미

사용법 출력

### 언제 호출되는가

* help 요청 시
* 인자 오류 시

### 어디서 호출되는가

주로 `main` 또는 `parse_cli_args` 이후

### 왜 필요한가

사용자에게 실행 방법을 알려줘야 하기 때문이다.

### 시그니처

```c
void print_usage(const char *program_name);
```

### 인자

* `program_name`: 실행 파일 이름

### 반환

없음

---

## 12-4. `read_text_file`

### 의미

파일 전체를 읽어서 문자열 하나로 반환

### 언제 호출되는가

CLI로 전달된 SQL 파일 경로를 읽을 때

### 어디서 호출되는가

`main`

### 왜 필요한가

Lexer는 문자열 입력을 받아야 하므로, 먼저 파일 전체를 문자열로 읽어야 한다.

### 시그니처

```c
char *read_text_file(const char *path);
```

### 인자

* `path`: 읽을 파일 경로

### 반환

* 성공: 동적 할당된 문자열
* 실패: `NULL`

### 주의

호출한 쪽이 `free`해야 한다.

---

## 12-5. `tokenize_sql`

### 의미

SQL 문자열을 token 배열로 바꾼다

### 언제 호출되는가

SQL 파일 내용을 읽은 직후

### 어디서 호출되는가

`main`

### 왜 필요한가

Parser는 token 단위로 문법을 분석하기 때문이다.

### 시그니처

```c
int tokenize_sql(const char *sql, TokenArray *out_tokens, char *errbuf, size_t errbuf_size);
```

### 인자

* `sql`: 원본 SQL 문자열
* `out_tokens`: 결과 token 배열
* `errbuf`: 에러 메시지 저장 버퍼
* `errbuf_size`: 버퍼 크기

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_LEX_ERROR`

### 함수가 하는 일

* 공백 건너뛰기
* 키워드 인식
* identifier 인식
* 숫자 인식
* 문자열 인식
* 기호 인식
* 잘못된 문자 에러 처리

---

## 12-6. `free_token_array`

### 의미

token 배열이 사용한 메모리를 해제

### 언제 호출되는가

Parser 이후 또는 에러 종료 직전

### 어디서 호출되는가

보통 `main`

### 왜 필요한가

token 내부 문자열들이 동적 할당되기 때문이다.

### 시그니처

```c
void free_token_array(TokenArray *tokens);
```

### 반환

없음

---

# 13. Parser 내부 함수들

Parser는 SQL 문법을 이해하는 단계다.

---

## 13-1. `parse_statement`

### 의미

token 배열 전체를 읽어서 하나의 SQL 문장 구조체를 만든다

### 언제 호출되는가

Lexer가 끝난 직후

### 어디서 호출되는가

`main`

### 왜 필요한가

Executor는 문자열이 아니라 구조화된 `Statement`를 입력으로 받기 때문이다.

### 시그니처

```c
int parse_statement(const TokenArray *tokens, Statement *out_stmt, char *errbuf, size_t errbuf_size);
```

### 인자

* `tokens`: token 목록
* `out_stmt`: 결과 Statement
* `errbuf`: 에러 메시지 버퍼
* `errbuf_size`: 버퍼 크기

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_PARSE_ERROR`

### 함수가 하는 일

* 첫 token 확인
* `INSERT`면 `parse_insert`
* `SELECT`면 `parse_select`
* 아니면 parse error

---

## 13-2. `parse_insert`

### 의미

INSERT 문법만 분석하는 내부 함수

### 언제 호출되는가

`parse_statement`가 첫 token이 INSERT라고 판단했을 때

### 어디서 호출되는가

`parse_statement`

### 왜 필요한가

INSERT 문법 처리와 SELECT 문법 처리를 분리해야 코드가 깔끔해진다.

### 인자 / 반환

`parse_statement`와 비슷하지만 보통 `static` 내부 함수로 구현한다.

### 하는 일

* `INSERT` 확인
* `INTO` 확인
* 테이블명 읽기
* 선택적 컬럼 리스트 읽기
* `VALUES` 확인
* literal list 읽기
* AST 채우기

---

## 13-3. `parse_select`

### 의미

SELECT 문법만 분석하는 내부 함수

### 언제 호출되는가

`parse_statement`가 SELECT 문장을 만났을 때

### 어디서 호출되는가

`parse_statement`

### 왜 필요한가

SELECT는 `*` 케이스와 컬럼 리스트 케이스가 있어 별도 처리하는 게 자연스럽다.

### 하는 일

* `SELECT` 확인
* `*` 또는 identifier list 읽기
* `FROM` 확인
* 테이블명 읽기
* AST 채우기

---

## 13-4. `parse_identifier_list`

### 의미

`id, name, age` 같은 컬럼 목록을 읽는다

### 언제 호출되는가

* INSERT 컬럼 리스트 파싱 시
* SELECT 컬럼 리스트 파싱 시

### 어디서 호출되는가

`parse_insert`, `parse_select`

### 왜 필요한가

반복되는 리스트 문법을 공통화하기 위해서다.

### 시그니처 예시

```c
static int parse_identifier_list(Parser *parser, char ***out_items, size_t *out_count, char *errbuf, size_t errbuf_size);
```

### 인자

* `parser`: 현재 parser 상태
* `out_items`: 문자열 배열 결과
* `out_count`: 개수 결과
* `errbuf`, `errbuf_size`: 오류 메시지

### 반환

성공/실패 상태 코드

---

## 13-5. `parse_literal_list`

### 의미

`(1, 'Alice', 20)` 안의 값 목록을 읽는다

### 언제 호출되는가

INSERT 파싱 시

### 어디서 호출되는가

`parse_insert`

### 왜 필요한가

값 목록도 identifier list처럼 반복되는 문법 구조이기 때문이다.

---

## 13-6. `free_statement`

### 의미

Statement 내부에 할당된 문자열/배열들을 해제한다

### 언제 호출되는가

실행이 끝난 후 또는 파싱 성공 후 더 이상 안 쓸 때

### 어디서 호출되는가

주로 `main`

### 왜 필요한가

AST 내부도 동적 메모리를 사용하기 때문이다.

---

# 14. Schema 관련 함수

---

## 14-1. `load_table_schema`

### 의미

schema 파일을 읽어서 `TableSchema` 구조체를 만든다

### 언제 호출되는가

INSERT/SELECT 실행 직전

### 어디서 호출되는가

주로 `execute_insert`, `execute_select`

### 왜 필요한가

컬럼 구조를 알아야 INSERT도 SELECT도 할 수 있기 때문이다.

### 시그니처

```c
int load_table_schema(const char *db_dir, const char *table_name,
                      TableSchema *out_schema,
                      char *errbuf, size_t errbuf_size);
```

### 인자

* `db_dir`: DB 디렉터리
* `table_name`: 테이블 이름
* `out_schema`: 결과 schema
* `errbuf`, `errbuf_size`: 에러 메시지

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_SCHEMA_ERROR`

### 하는 일

* `<db_dir>/<table_name>.schema` 열기
* 줄 단위로 컬럼 읽기
* 빈 줄 무시
* 중복 컬럼 검사
* schema 구조체 채우기

---

## 14-2. `schema_find_column_index`

### 의미

어떤 컬럼이 schema에서 몇 번째인지 찾는다

### 언제 호출되는가

* INSERT 때 컬럼 매핑
* SELECT 때 projection

### 어디서 호출되는가

`build_insert_row`, `validate_select_columns`, `project_rows_for_select`

### 왜 필요한가

결국 row는 “순서 기반 배열”이므로 컬럼 이름을 index로 바꾸는 작업이 꼭 필요하다.

### 시그니처

```c
int schema_find_column_index(const TableSchema *schema, const char *column_name);
```

### 인자

* `schema`: 테이블 구조
* `column_name`: 찾을 컬럼명

### 반환

* 찾으면 index
* 못 찾으면 `-1`

---

## 14-3. `free_table_schema`

### 의미

schema 구조체 메모리 해제

### 언제 호출되는가

실행 완료 후

### 어디서 호출되는가

`execute_insert`, `execute_select`

---

# 15. Storage 관련 함수

---

## 15-1. `ensure_table_data_file`

### 의미

`.data` 파일이 없으면 생성해두는 함수

### 언제 호출되는가

INSERT 전

### 어디서 호출되는가

`execute_insert`

### 왜 필요한가

처음 INSERT하는 테이블은 data 파일이 아직 없을 수 있기 때문이다.

### 시그니처

```c
int ensure_table_data_file(const char *db_dir, const char *table_name,
                           char *errbuf, size_t errbuf_size);
```

### 반환

성공/실패 상태 코드

---

## 15-2. `append_row_to_table`

### 의미

row 하나를 `.data` 파일 끝에 추가

### 언제 호출되는가

INSERT 실행 시

### 어디서 호출되는가

`execute_insert`

### 왜 필요한가

INSERT의 실제 저장 동작이기 때문이다.

### 시그니처

```c
int append_row_to_table(const char *db_dir, const char *table_name,
                        const Row *row,
                        char *errbuf, size_t errbuf_size);
```

### 인자

* `db_dir`: DB 경로
* `table_name`: 테이블 이름
* `row`: 저장할 row
* `errbuf`, `errbuf_size`: 에러 메시지 버퍼

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_STORAGE_ERROR`

### 하는 일

* row의 각 value escape
* `|`로 join
* 파일 끝에 append

---

## 15-3. `read_all_rows_from_table`

### 의미

`.data` 파일 전체를 읽어서 row 배열로 복원

### 언제 호출되는가

SELECT 실행 시

### 어디서 호출되는가

`execute_select`

### 왜 필요한가

SELECT는 파일의 기존 데이터를 읽어야 하기 때문이다.

### 시그니처

```c
int read_all_rows_from_table(const char *db_dir, const char *table_name,
                             size_t expected_column_count,
                             Row **out_rows, size_t *out_row_count,
                             char *errbuf, size_t errbuf_size);
```

### 인자

* `db_dir`: DB 경로
* `table_name`: 테이블 이름
* `expected_column_count`: schema 기준 컬럼 수
* `out_rows`: 읽어온 row 배열
* `out_row_count`: row 개수
* `errbuf`, `errbuf_size`: 에러 메시지

### 반환

성공/실패 상태 코드

### 왜 `expected_column_count`를 받는가

data 파일에 깨진 row가 있을 수 있기 때문이다.
읽은 field 수가 schema와 맞는지 검증해야 한다.

---

## 15-4. `free_rows`

### 의미

row 배열 메모리 해제

### 언제 호출되는가

SELECT 결과 생성 후, 중간 row가 더 이상 필요 없을 때

### 어디서 호출되는가

`execute_select`, `free_exec_result`

---

# 16. Executor 관련 함수

Executor는 가장 중요하다.

---

## 16-1. `execute_statement`

### 의미

Statement를 실제 동작으로 실행한다

### 언제 호출되는가

Parser가 끝난 직후

### 어디서 호출되는가

`main`

### 왜 필요한가

INSERT와 SELECT를 공통 진입점 하나로 처리하기 위해서다.

### 시그니처

```c
int execute_statement(const char *db_dir, const Statement *stmt,
                      ExecResult *out_result,
                      char *errbuf, size_t errbuf_size);
```

### 인자

* `db_dir`: DB 경로
* `stmt`: 실행할 AST
* `out_result`: 실행 결과
* `errbuf`, `errbuf_size`: 에러 메시지

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_EXEC_ERROR` 또는 하위 에러 코드

### 하는 일

* `stmt->type` 확인
* INSERT면 `execute_insert`
* SELECT면 `execute_select`

---

## 16-2. `execute_insert`

### 의미

INSERT 문 실행 전용 함수

### 언제 호출되는가

`execute_statement`가 INSERT라고 판단했을 때

### 어디서 호출되는가

`execute_statement`

### 왜 필요한가

INSERT 실행 로직을 SELECT와 분리하기 위해서다.

### 하는 일

1. schema 로드
2. 입력 컬럼 검사
3. 최종 row 생성
4. data 파일 보장
5. 파일에 append
6. 결과 채우기

---

## 16-3. `build_insert_row`

### 의미

INSERT 문으로부터 “schema 순서에 맞는 최종 row”를 만든다

### 언제 호출되는가

INSERT 실행 중

### 어디서 호출되는가

`execute_insert`

### 왜 필요한가

입력 컬럼 순서와 schema 순서는 다를 수 있기 때문이다.

### 예

입력:

```sql
INSERT INTO users (name, id) VALUES ('Bob', 2);
```

schema:

```text
id
name
age
```

최종 row는

```text
["2", "Bob", ""]
```

이어야 한다.

### 시그니처

```c
static int build_insert_row(const TableSchema *schema, const InsertStatement *stmt,
                            Row *out_row,
                            char *errbuf, size_t errbuf_size);
```

### 인자

* `schema`: 테이블 구조
* `stmt`: INSERT AST
* `out_row`: 결과 row
* `errbuf`, `errbuf_size`: 에러 메시지

### 반환

성공/실패 상태 코드

---

## 16-4. `execute_select`

### 의미

SELECT 문 실행 전용 함수

### 언제 호출되는가

`execute_statement`가 SELECT라고 판단했을 때

### 어디서 호출되는가

`execute_statement`

### 왜 필요한가

SELECT는 읽기, projection, 출력용 결과 생성 등 INSERT와 다른 흐름이기 때문이다.

### 하는 일

1. schema 로드
2. 컬럼 유효성 검사
3. row 전체 읽기
4. 필요한 컬럼만 projection
5. 결과 구조체 채우기

---

## 16-5. `validate_select_columns`

### 의미

SELECT에서 요청한 컬럼들이 실제 schema에 존재하는지 검사

### 언제 호출되는가

SELECT 실행 초반

### 어디서 호출되는가

`execute_select`

### 왜 필요한가

존재하지 않는 컬럼을 읽으려고 하면 실행하면 안 되기 때문이다.

---

## 16-6. `project_rows_for_select`

### 의미

전체 row에서 필요한 컬럼만 골라 새로운 결과를 만든다

### 언제 호출되는가

SELECT 실행 중, schema와 row를 다 읽은 후

### 어디서 호출되는가

`execute_select`

### 왜 필요한가

`SELECT *`가 아닌 경우 row 전체가 아니라 일부 컬럼만 보여줘야 하기 때문이다.

### 예

전체 row가

```text
["1", "Alice", "20"]
```

이고 요청 컬럼이 `id`, `name`이면

```text
["1", "Alice"]
```

만 결과로 만든다.

---

## 16-7. `free_exec_result`

### 의미

실행 결과 구조체 내부 메모리 해제

### 언제 호출되는가

출력 후

### 어디서 호출되는가

`main`

---

# 17. Result 관련 함수

---

## 17-1. `print_exec_result`

### 의미

실행 결과를 사람이 읽을 수 있게 출력

### 언제 호출되는가

실행 성공 직후

### 어디서 호출되는가

`main`

### 왜 필요한가

INSERT와 SELECT 결과 출력 형식이 다르기 때문이다.

### 시그니처

```c
void print_exec_result(const ExecResult *result);
```

### 인자

* `result`: 실행 결과

### 반환

없음

### 하는 일

* INSERT면 `INSERT 1`
* SELECT면 헤더/row/row count 출력

---

# 18. Utility 함수들

---

## 18-1. `xmalloc`

### 의미

메모리 할당을 안전하게 감싸는 함수

### 왜 필요한가

매번 `malloc` 실패 체크를 중복 작성하지 않기 위해

### 시그니처 예시

```c
void *xmalloc(size_t size);
```

### 반환

할당된 메모리 포인터

---

## 18-2. `strdup_safe`

### 의미

문자열 복사

### 왜 필요한가

token text, AST 문자열, schema 컬럼명 등을 독립적으로 저장해야 하기 때문

---

## 18-3. `trim_whitespace`

### 의미

문자열 앞뒤 공백 제거

### 왜 필요한가

파일 줄 처리 시 불필요한 공백 제거에 유용하기 때문

---

# 19. 함수 관계를 한 번에 보는 호출 흐름

```text
main
 ├─ parse_cli_args
 ├─ read_text_file
 ├─ tokenize_sql
 ├─ parse_statement
 │   ├─ parse_insert
 │   │   ├─ parse_identifier_list
 │   │   └─ parse_literal_list
 │   └─ parse_select
 │       └─ parse_identifier_list
 ├─ execute_statement
 │   ├─ execute_insert
 │   │   ├─ load_table_schema
 │   │   ├─ build_insert_row
 │   │   │   └─ schema_find_column_index
 │   │   ├─ ensure_table_data_file
 │   │   └─ append_row_to_table
 │   └─ execute_select
 │       ├─ load_table_schema
 │       ├─ validate_select_columns
 │       │   └─ schema_find_column_index
 │       ├─ read_all_rows_from_table
 │       └─ project_rows_for_select
 │           └─ schema_find_column_index
 ├─ print_exec_result
 ├─ free_exec_result
 ├─ free_statement
 ├─ free_token_array
 └─ free(...)
```

이 그림을 이해하면 전체 코드가 거의 다 읽힌다.

---

# 20. 공부할 때 특히 중요하게 봐야 하는 포인트

---

## 20-1. Lexer와 Parser의 차이

이건 꼭 구분해야 한다.

* Lexer: 단어를 자른다
* Parser: 문장 구조를 이해한다

즉,

```sql
SELECT id, name FROM users;
```

에서

Lexer는
“SELECT”, “id”, “,”, “name”, “FROM”, “users”로 자르는 역할이다.

Parser는
“이건 SELECT문이고, 테이블은 users고, 컬럼은 id와 name이야”라고 이해하는 역할이다.

---

## 20-2. AST와 실행의 차이

AST는 “의미를 담는 구조체”이고
Executor는 “그 의미를 실제 행동으로 바꾸는 단계”다.

즉, AST는 설명서이고 Executor는 작업자다.

---

## 20-3. Schema와 Data의 차이

* schema: 구조
* data: 실제 값

예를 들어

```text
id
name
age
```

는 “형식”이고,

```text
1|Alice|20
```

는 “내용”이다.

---

## 20-4. INSERT에서 가장 중요한 부분

입력 컬럼 순서를 schema 순서로 바꾸는 것이다.

이 부분을 이해하면 Executor의 절반은 이해한 셈이다.

---

## 20-5. SELECT에서 가장 중요한 부분

필요한 컬럼만 잘라내는 projection이다.

---

# 21. 네가 코드 읽을 때 추천하는 순서

이 프로젝트 코드를 처음 읽을 때는 아래 순서가 가장 좋다.

### 1순위

`main.c`

왜냐하면 전체 흐름이 보이기 때문이다.

### 2순위

`ast.h`

왜냐하면 프로그램이 SQL을 어떤 구조로 이해하는지 보여주기 때문이다.

### 3순위

`parser.c`

왜냐하면 문자열 SQL이 구조체로 바뀌는 지점을 이해해야 하기 때문이다.

### 4순위

`executor.c`

왜냐하면 이 프로젝트의 실제 핵심 로직이 여기에 있기 때문이다.

### 5순위

`schema.c`, `storage.c`

왜냐하면 파일 기반 DB가 실제로 어떻게 동작하는지 여기서 보이기 때문이다.

### 6순위

`result.c`, `utils.c`

부가적인 출력/편의 로직이다.

---

# 22. 마지막으로, 이 프로젝트를 한 번에 기억하는 방법

이 프로젝트를 아주 짧게 기억하면 이렇게 외우면 된다.

```text
SQL 문자열을
token으로 자르고,
문법 구조(AST)로 만들고,
그 구조를 파일 DB 위에서 실행하는 프로그램
```

또는 더 짧게:

```text
읽고 -> 해석하고 -> 실행하고 -> 보여준다
```

---

원하면 다음 답변에서 이걸 이어서
**“학습용 PDF 문서 형식”**으로 다시 정리해주거나,
아니면 **“함수별 설명만 표처럼 따로 뽑은 버전”**으로 정리해줄게.
