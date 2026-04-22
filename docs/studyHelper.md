좋아요. 이번 문서는 **“읽는 순서 = 이해 순서”**가 되도록 구성하겠습니다. 먼저 아주 위에서 전체 흐름을 잡고, 그다음 “이번 B+트리 과제가 기존 코드에 정확히 어디 끼어드는지”를 설명하고, 마지막에는 **함수 단위**로 내려가서 **언제 호출되고, 왜 필요하고, 어떤 인자를 받고, 무엇을 반환하는지**까지 정리하겠습니다.

---

# 1. 먼저 가장 위에서 보는 전체 그림

지금 베이스 코드는 **C로 만든 파일 기반 미니 SQL 엔진**입니다. SQL 파일을 읽고, Lexer가 토큰으로 쪼개고, Parser가 AST를 만들고, Executor가 그 AST를 실제 동작으로 바꾸고, Storage가 `.schema` / `.data` 파일을 읽고 쓰는 구조입니다. 현재는 `INSERT`, `SELECT`, 단일 `WHERE column = literal` 정도만 지원하고, **B+Tree나 인덱스는 아직 없습니다.** 

즉 현재 구조는 이렇게 보면 됩니다.

```text
SQL 파일
  ↓
CLI / main
  ↓
Lexer (tokenize_sql)
  ↓
Parser (parse_next_statement)
  ↓
Executor (execute_statement)
  ↓
Storage (.schema, .data 파일 읽기/쓰기)
  ↓
Result 출력
```

이번 과제는 이 흐름 전체를 바꾸는 게 아니라, **Executor와 Storage 사이에 “Runtime Cache + B+Tree 인덱스 계층”을 추가하는 작업**입니다.

확장 후 큰 그림은 이렇게 됩니다.

```text
SQL 파일
  ↓
main
  ↓
Lexer
  ↓
Parser
  ↓
Executor
  ├─ INSERT
  │    ├─ 자동 id 생성
  │    ├─ .data 파일 append
  │    └─ B+Tree에 (id -> row_offset) 삽입
  │
  └─ SELECT
       ├─ WHERE id = ? 이면
       │    ├─ B+Tree에서 row_offset 검색
       │    └─ 해당 위치의 row만 파일에서 읽음
       │
       └─ 그 외 조건이면
            └─ 기존처럼 전체 row 선형 탐색
```

이걸 한 문장으로 줄이면:

**기존 SQL 처리기는 그대로 두고, “id 기반 조회만 빠르게 만드는 인덱스 경로”를 새로 뚫는 작업**입니다.

---

# 2. 현재 코드의 의미를 먼저 이해해야 하는 이유

지금 코드베이스는 이미 다음 철학으로 만들어져 있습니다.

* SQL 문자열을 바로 실행하지 않고 **토큰 → AST → 실행**으로 나눈다.
* 테이블은 DBMS 페이지 파일이 아니라 **아주 단순한 텍스트 파일**로 저장한다.
* `.schema`는 컬럼 목록, `.data`는 실제 row 데이터다.
* `SELECT`는 지금까지는 **항상 전체 row를 읽고 검사**하는 방식이다.
* `id`가 있어도 현재는 “인덱스”가 아니라, INSERT 전에 중복 검사용으로 전체 데이터를 읽는 방식에 가깝다. 

그래서 이번 과제에서 핵심은 새로운 SQL 문법을 만드는 게 아닙니다.
핵심은 이미 있는 파이프라인에서:

* **INSERT 경로를 자동 id 부여 방식으로 바꾸고**
* **SELECT 경로를 인덱스 경로 / 전체 스캔 경로로 분기시키고**
* **Storage에 offset 기반 접근을 추가하는 것**

입니다.

---

# 3. 이번 과제를 Top-Down으로 보면, 바뀌는 건 딱 4군데입니다

## 3-1. `main` 수준

프로그램 전체 실행 동안 테이블 인덱스를 재사용하려면, statement마다 상태를 잃어버리면 안 됩니다.
그래서 실행 중 살아 있는 `ExecutionContext`가 필요합니다.

## 3-2. `executor` 수준

기존에는 `execute_statement()`가 곧바로 storage를 읽고 쓰는 구조였다면, 이제는:

* 테이블 runtime 상태를 가져오고
* 필요하면 index를 만들고
* INSERT면 index도 업데이트하고
* SELECT면 index를 쓸지 full scan 할지 결정

해야 합니다.

## 3-3. `storage` 수준

기존 `read_all_rows_from_table()`만으로는 인덱스가 의미가 없습니다.
인덱스가 가리키는 건 row 자체가 아니라 **row가 파일에서 시작하는 위치(row_offset)** 여야 하고, 그 위치의 row를 **바로 읽어오는 함수**가 필요합니다.

## 3-4. 새 `bptree` 모듈

`id -> row_offset`을 저장하는 메모리 기반 B+Tree가 필요합니다.
이건 SQL을 이해하는 모듈이 아니라, **정렬된 키를 빠르게 저장/검색하는 자료구조 모듈**입니다.

---

# 4. 이제 실행 흐름을 시나리오로 이해해보겠습니다

## 4-1. 프로그램 시작 시

현재 프로그램은 CLI 인자를 읽고 SQL 파일을 읽은 뒤, 토큰화하고, statement를 하나씩 파싱해서 실행합니다. 이 기본 구조는 유지됩니다. 

확장 후 흐름은 다음과 같습니다.

1. `main()`이 `-d`, `-f`를 읽는다.
2. SQL 파일 전체를 읽는다.
3. `tokenize_sql()`로 토큰 배열을 만든다.
4. `ExecutionContext`를 만든다.
5. `parse_next_statement()`로 statement를 하나씩 꺼낸다.
6. 각 statement를 `execute_statement(ctx, stmt, ...)`로 실행한다.
7. 실행 결과를 출력한다.
8. 끝나면 context와 tree 메모리를 해제한다.

여기서 중요한 변화는 딱 하나입니다.

**예전에는 statement 실행이 서로 독립적이었는데, 이제는 같은 실행 안에서 테이블 index 상태를 공유한다는 점**입니다.

---

## 4-2. 테이블을 처음 접근할 때

예를 들어 SQL 파일 안에 아래처럼 여러 문장이 있다고 해봅시다.

```sql
INSERT INTO users VALUES ('Alice', '20');
INSERT INTO users VALUES ('Bob', '21');
SELECT * FROM users WHERE id = 2;
```

첫 번째 `users` 접근 때 해야 할 일은:

1. `users.schema`를 읽는다.
2. `users` 테이블에 `id` 컬럼이 있는지 찾는다.
3. `users.data`가 없으면 빈 파일로 만든다.
4. `id` 컬럼이 있으면 기존 `.data` 전체를 한 번 스캔해서
   `id -> row_offset` B+Tree를 만든다.
5. 동시에 현재 최대 id를 찾아서 `next_id = max_id + 1`로 둔다.
6. 이 상태를 `TableRuntime`에 저장한다.

이 작업은 **같은 테이블에 대해 딱 한 번만** 일어납니다.
그 뒤 같은 실행 안에서 다시 `users`를 쓰거나 읽을 때는 이미 만들어 둔 runtime을 재사용합니다.

---

## 4-3. INSERT 시 실제로 일어나는 일

이번 과제에서 INSERT는 더 이상 “사용자가 id를 넣는 동작”이 아닙니다.
**id는 시스템이 자동 생성합니다.**

예를 들어 schema가:

```text
id
name
age
```

이고 사용자가:

```sql
INSERT INTO users VALUES ('Alice', '20');
```

를 넣으면, 내부에서는 이런 일이 일어납니다.

1. `users` runtime을 가져온다.
2. `generated_id = table->next_id`
3. 최종 row를 schema 길이에 맞게 조립한다.

   * `id = "1"`
   * `name = "Alice"`
   * `age = "20"`
4. `.data` 파일 맨 끝에 append 한다.
5. append 하기 직전 파일 위치를 `row_offset`으로 저장한다.
6. B+Tree에 `(1 -> row_offset)`를 넣는다.
7. `next_id++`
8. 결과는 `INSERT 1`

핵심은 이 부분입니다.

**파일 append와 인덱스 insert가 한 쌍으로 움직여야 한다.**

즉 row는 파일에만 있고, tree에는 row 전체가 아니라
**“이 id의 row는 파일의 몇 바이트 위치에서 시작한다”** 라는 정보만 들어갑니다.

---

## 4-4. `SELECT ... WHERE id = ?` 시 실제로 일어나는 일

예를 들어:

```sql
SELECT * FROM users WHERE id = 100;
```

이면 executor는 먼저 묻습니다.

* 조건 컬럼이 정확히 `id`인가?
* 이 테이블에 `id` 컬럼이 실제로 있는가?
* literal이 인덱스로 바로 바꿔도 되는 “정상적인 정수”인가?

셋 다 맞으면:

1. B+Tree에서 `100`을 찾는다.
2. 있으면 `row_offset`을 얻는다.
3. `.data` 파일에서 그 offset으로 바로 이동한다.
4. 그 줄 하나만 읽어서 row를 복원한다.
5. projection 적용 후 결과 1건 반환

즉 이 경로에서는 **전체 파일을 읽지 않습니다.**

---

## 4-5. `SELECT ... WHERE name = ?` 시 실제로 일어나는 일

예를 들어:

```sql
SELECT * FROM users WHERE name = 'Alice';
```

이면 B+Tree는 못 씁니다.
인덱스는 id에 대해서만 있으니까요.

이 경우는 기존 방식 그대로 갑니다.

1. `.data` 전체 스캔
2. 각 row에 대해 `name == 'Alice'` 검사
3. 맞는 row만 projection
4. 결과 배열에 append

즉 이번 과제의 목표는 **모든 SELECT를 빠르게 만드는 것**이 아니라
**“WHERE id = ?” 하나를 빠르게 만드는 것**입니다.

---

# 5. 여기서 꼭 이해해야 하는 핵심 개념 5개

## 5-1. `ExecutionContext`

프로그램 실행 중 살아 있는 전역 runtime 상태입니다.

이게 필요한 이유는, statement마다 테이블 인덱스를 다시 만들면 너무 비효율적이기 때문입니다.

쉽게 말해:

* `main()`이 시작할 때 만든다
* 모든 statement 실행이 공유한다
* 프로그램 종료 시 해제한다

즉 **“이번 실행 세션의 메모리 속 DB 상태 캐시”** 입니다.

---

## 5-2. `TableRuntime`

테이블 하나에 대한 runtime 정보입니다.

예를 들어 `users` 테이블의 runtime 안에는 이런 정보가 들어갑니다.

* 테이블 이름
* schema
* `id` 컬럼 존재 여부
* `id` 컬럼의 index
* B+Tree root
* 현재 다음에 발급할 id (`next_id`)
* index가 이미 빌드됐는지 여부

즉 **“한 테이블을 빠르게 다루기 위한 실행용 부가 정보”** 입니다.

---

## 5-3. `row_offset`

이건 이번 과제에서 제일 중요한 low-level 개념입니다.

`.data` 파일이 이런 식이라고 합시다.

```text
1|Alice|20
2|Bob|21
3|Chris|22
```

그러면 각 row는 파일 안에서 시작하는 바이트 위치가 있습니다.

* row 1 시작 위치
* row 2 시작 위치
* row 3 시작 위치

B+Tree는 row 내용을 저장하지 않고,

```text
1 -> row1 시작 위치
2 -> row2 시작 위치
3 -> row3 시작 위치
```

만 저장합니다.

이렇게 해야 B+Tree가 가볍고, 실제 DB 인덱스처럼 동작합니다.

---

## 5-4. `next_id`

자동 증가 id의 다음 값입니다.

규칙은 단순합니다.

* 빈 테이블이면 `1`
* 데이터가 있으면 `max(existing_id) + 1`

즉 테이블을 처음 runtime에 올릴 때 기존 `.data`를 스캔하면서
현재 최대 id를 계산해야 합니다.

---

## 5-5. “canonical integer” 여부

현재 엔진은 literal을 결국 문자열처럼 저장하고 비교하는 구조입니다. 숫자 literal도 실행 단계에서는 문자열 취급에 가깝습니다. 

그래서 인덱스를 쓸 때는 조심해야 합니다.

예를 들어:

* `WHERE id = 123` → 인덱스 사용 가능
* `WHERE id = '123'` → 인덱스 사용 가능
* `WHERE id = '00123'` → 인덱스 사용 금지
* `WHERE id = 1.0` → 인덱스 사용 금지
* `WHERE id = -1` → 인덱스 사용 금지

이유는 기존 엔진의 문자열 비교 의미를 깨면 안 되기 때문입니다.
인덱스를 잘못 쓰면 기존 SQL 의미와 달라질 수 있습니다.

---

# 6. Low-Level 설계로 내려가기

이제 진짜 구조체 수준으로 내려가겠습니다.

## 6-1. `ExecResult` 확장

기존 `ExecResult`는 INSERT/SELECT 결과 자체만 담습니다. 이번 과제에서는 아래 메타데이터를 추가하는 게 좋습니다.

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

### 의미

* `used_index`: 이번 SELECT가 B+Tree를 탔는지 테스트에서 확인하기 위한 플래그
* `has_generated_id`: INSERT에서 자동 생성 id가 있는지
* `generated_id`: 실제로 생성된 id

### 왜 필요한가

CLI 출력만 보면 “정말 인덱스를 썼는지” 알기 어렵습니다.
테스트와 벤치마크를 쉽게 하려면 result에 이 메타정보가 있어야 합니다.

---

## 6-2. `TableRuntime`

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

### 필드 의미

* `table_name`: runtime이 어느 테이블 것인지 식별
* `schema`: 매번 다시 파일에서 읽지 않기 위해 캐시
* `has_id_column`: 이 테이블이 인덱스 대상인지 여부
* `id_column_index`: row에서 id가 몇 번째 필드인지
* `id_index_ready`: 이 테이블의 index가 빌드되었는지
* `next_id`: 다음 INSERT가 쓸 id
* `id_index`: 실제 B+Tree

### 핵심 불변식

* `has_id_column == 1`이면 `id_column_index >= 0`
* `id_index_ready == 1`이면 `next_id`는 올바른 다음 id
* tree 안의 모든 key는 유일한 id다

---

## 6-3. `ExecutionContext`

```c
typedef struct {
    char *db_dir;
    TableRuntime *tables;
    size_t table_count;
    size_t table_capacity;
} ExecutionContext;
```

### 의미

현재 실행 중 로드된 테이블 runtime들의 배열입니다.

### 왜 배열인가

처음에는 `users`만 쓸 수 있지만, 나중에는 `users`, `orders`, `posts` 등 여러 테이블을 한 SQL 파일에서 다룰 수 있기 때문입니다.

---

## 6-4. `BPTreeNode`

```c
#define BPTREE_ORDER 64
#define BPTREE_MAX_KEYS (BPTREE_ORDER - 1)
#define BPTREE_MAX_CHILDREN (BPTREE_ORDER)

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

### 필드 의미

* `is_leaf`: leaf 노드인지 internal 노드인지
* `key_count`: 현재 이 노드에 들어 있는 key 개수
* `keys[]`: 정렬된 key 배열
* `parent`: 위 노드 포인터
* `next`: leaf 끼리 옆으로 연결하는 포인터
* `ptrs.children[]`: internal 노드일 때 자식 포인터
* `ptrs.row_offsets[]`: leaf 노드일 때 key와 매칭되는 offset 값

### 왜 union인가

internal 노드와 leaf 노드가 저장하는 대상이 다르기 때문입니다.

* internal: 자식 노드 포인터
* leaf: 실제 value (`row_offset`)

---

## 6-5. `BPTree`

```c
typedef struct {
    BPTreeNode *root;
    size_t key_count;
} BPTree;
```

### 의미

트리 전체의 진입점입니다.

### 핵심 불변식

* `root == NULL`이면 빈 트리
* 모든 leaf는 같은 depth에 있어야 함
* leaf key는 오름차순 정렬
* leaf chain도 전체 key 순서를 보장해야 함

---

# 7. 이제 함수 단위로 내려가겠습니다

여기서부터는 두 층으로 나눠서 보시면 됩니다.

1. **기존 코드에서 이미 있는 함수**
2. **이번 과제에서 새로 추가하거나 바꿔야 하는 함수**

---

# 8. 기존 코드의 핵심 함수들 먼저 이해하기

기존 프로젝트는 이미 다음 함수들로 흐릅니다. 이 흐름을 알아야 새 함수가 어디에 꽂히는지 이해할 수 있습니다. 

## 8-1. `main`

```c
int main(int argc, char **argv);
```

### 의미

프로그램 전체 orchestration 함수입니다.

### 언제/어디서 호출되나

프로그램 시작 시 OS가 진입합니다.

### 왜 필요한가

각 모듈은 자기 일만 하고, 전체 순서를 조립하는 역할은 `main`이 맡아야 하기 때문입니다.

### 인자

* `argc`, `argv`: CLI 인자들
  예: `-d db -f query.sql`

### 내부 연산

* CLI 파싱
* SQL 파일 읽기
* 공백 정리
* 토큰화
* statement loop 파싱
* 각 statement 실행
* 결과 출력
* 에러 처리 및 종료 코드 반환

### 반환

* 성공이면 `0`
* 실패면 status code

### 이번 과제에서 바뀌는 점

`ExecutionContext` 생성/해제가 추가되고, `execute_statement()`에 context를 넘기게 됩니다.

---

## 8-2. `parse_cli_args`

```c
int parse_cli_args(int argc, char **argv, CliOptions *out_options);
```

### 의미

사용자가 넣은 프로그램 인자를 구조체로 정리합니다.

### 언제 호출되나

`main()` 초반

### 왜 필요한가

`argv`를 직접 여기저기서 해석하면 코드가 지저분해지기 때문입니다.

### 인자

* `argc`, `argv`: 원본 CLI 인자
* `out_options`: 파싱 결과를 담을 구조체

### 반환

* 성공 시 0
* 잘못된 인자면 1

### 이번 과제에서의 위치

거의 변경 없음

---

## 8-3. `read_text_file`

```c
char *read_text_file(const char *path);
```

### 의미

파일 전체를 읽어 NUL 종료 문자열로 반환합니다.

### 언제 호출되나

`main()`이 SQL 파일을 읽을 때

### 인자

* `path`: 읽을 파일 경로

### 반환

* 성공: heap에 할당된 문자열 포인터
* 실패: `NULL`

### 왜 이런 반환형인가

Lexer는 파일 스트림이 아니라 “전체 SQL 문자열”을 입력으로 받기 때문입니다.

---

## 8-4. `tokenize_sql`

```c
int tokenize_sql(
    const char *sql,
    TokenArray *out_tokens,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

SQL 문자열을 토큰 배열로 바꿉니다.

### 언제 호출되나

`main()`에서 SQL 파일을 읽은 직후

### 왜 필요한가

Parser가 직접 문자열을 문자 단위로 다루지 않고, 의미 있는 단위(token)로 받도록 하기 위해서입니다.

### 인자

* `sql`: 원본 SQL 문자열
* `out_tokens`: 결과 토큰 배열
* `errbuf`, `errbuf_size`: 에러 메시지 출력용

### 내부 연산

* 식별자/키워드 분리
* 숫자 literal 분리
* string literal 파싱
* 쉼표, 괄호, 세미콜론, `*`, `=` 토큰화

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_LEX_ERROR`

### 이번 과제에서의 위치

변경 없음
이번 과제는 문법을 바꾸는 게 아니라 실행 경로를 바꾸는 작업입니다.

---

## 8-5. `parse_next_statement`

```c
int parse_next_statement(
    const TokenArray *tokens,
    size_t *cursor,
    Statement *out_stmt,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

토큰 배열에서 statement 하나만 파싱하고, 다음 시작 위치로 cursor를 옮깁니다.

### 언제 호출되나

`main()`의 statement loop 안에서 반복 호출

### 왜 필요한가

한 SQL 파일 안에 여러 문장이 `;`로 들어갈 수 있기 때문입니다. 

### 인자

* `tokens`: 전체 토큰 스트림
* `cursor`: 현재 읽기 위치
* `out_stmt`: 생성된 AST
* `errbuf`, `errbuf_size`: 에러 버퍼

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_PARSE_ERROR`

### 이번 과제에서의 위치

변경 없음

---

## 8-6. `load_table_schema`

```c
int load_table_schema(
    const char *db_dir,
    const char *table_name,
    TableSchema *out_schema,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

`<table>.schema` 파일을 읽어 컬럼 목록을 메모리 구조체로 만듭니다.

### 언제 호출되나

기존에는 INSERT/SELECT 직전 executor에서 필요할 때마다 호출됩니다.

### 왜 필요한가

row를 해석하려면 컬럼 순서를 알아야 하기 때문입니다.

### 인자

* `db_dir`: DB 루트 디렉터리
* `table_name`: 테이블 이름
* `out_schema`: 결과 schema
* `errbuf`, `errbuf_size`: 에러 버퍼

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_SCHEMA_ERROR`

### 이번 과제에서의 위치

이제는 `get_or_load_table_runtime()`가 내부에서 호출하고, schema를 캐시하게 됩니다.

---

## 8-7. `ensure_table_data_file`

```c
int ensure_table_data_file(
    const char *db_dir,
    const char *table_name,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

`<table>.data` 파일이 없으면 생성합니다.

### 언제 호출되나

INSERT나 SELECT 전에 데이터 파일 존재를 보장할 때

### 왜 필요한가

빈 테이블도 파일 관점에서는 “없는 파일”일 수 있기 때문입니다.

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_STORAGE_ERROR`

### 이번 과제에서의 위치

table runtime 로드 시 한 번 보장하면 됩니다.

---

## 8-8. `append_row_to_table`

```c
int append_row_to_table(
    const char *db_dir,
    const char *table_name,
    const Row *row,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

row를 escape해서 `.data` 파일 끝에 추가합니다.

### 언제 호출되나

기존 INSERT 실행 경로

### 왜 필요한가

Storage 포맷 책임을 executor와 분리하기 위해서입니다.

### 인자

* `db_dir`, `table_name`: 대상 테이블
* `row`: 저장할 row
* `errbuf`, `errbuf_size`: 에러 버퍼

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_STORAGE_ERROR`

### 이번 과제에서의 위치

이 함수는 `append_row_to_table_with_offset()`로 확장되거나 대체됩니다.
이유는 인덱스에 넣을 `row_offset`이 필요하기 때문입니다.

---

## 8-9. `read_all_rows_from_table`

```c
int read_all_rows_from_table(
    const char *db_dir,
    const char *table_name,
    size_t expected_column_count,
    Row **out_rows,
    size_t *out_row_count,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

테이블의 모든 row를 읽어 메모리 배열로 반환합니다.

### 언제 호출되나

기존 SELECT 전체 스캔, id 중복 검사

### 왜 필요한가

기존 엔진은 전부 full scan 방식이었기 때문입니다. 

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_STORAGE_ERROR`

### 이번 과제에서의 위치

non-id SELECT에서는 계속 쓸 수 있지만,
index build와 indexed select에는 더 low-level한 함수가 필요합니다.

---

## 8-10. `execute_statement`

```c
int execute_statement(
    const char *db_dir,
    const Statement *stmt,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

AST를 실제 동작으로 바꾸는 진입 함수입니다.

### 언제 호출되나

`main()`이 statement 하나를 파싱한 직후

### 왜 필요한가

Parser는 “무슨 문장인지”만 만들고, 실제 의미 실행은 executor가 맡아야 하기 때문입니다.

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_EXEC_ERROR`, `STATUS_SCHEMA_ERROR`, `STATUS_STORAGE_ERROR` 등

### 이번 과제에서의 위치

가장 중요하게 바뀌는 함수입니다.

새 시그니처는 다음처럼 바뀌는 것이 좋습니다.

```c
int execute_statement(
    ExecutionContext *ctx,
    const Statement *stmt,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);
```

---

## 8-11. `print_exec_result`

```c
void print_exec_result(const ExecResult *result);
```

### 의미

실행 결과를 CLI 형식으로 렌더링합니다.

### 언제 호출되나

`main()`에서 각 statement 실행 직후

### 왜 필요한가

실행 로직과 출력 포맷을 분리하기 위해서입니다.

### 이번 과제에서의 위치

출력 형식은 거의 그대로 둬도 됩니다.
`used_index`나 `generated_id`는 테스트용 메타이므로, 꼭 출력할 필요는 없습니다.

---

# 9. 이제부터는 “이번 과제를 위해 새로 추가/변경할 함수”입니다

---

# 10. Runtime 계층 함수

이 계층은 “테이블 접근 시 매번 schema와 index를 다시 만들지 않도록” 하는 역할입니다.

## 10-1. `init_execution_context`

```c
int init_execution_context(
    const char *db_dir,
    ExecutionContext *out_ctx,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

실행 전체에서 공유할 runtime context를 초기화합니다.

### 언제 호출되나

`main()`에서 토큰화를 마친 뒤, statement loop 시작 전에 한 번

### 왜 필요한가

테이블별 cache와 B+Tree를 담을 루트 구조가 필요하기 때문입니다.

### 인자

* `db_dir`: 모든 테이블 파일이 있는 디렉터리
* `out_ctx`: 초기화할 context 구조체
* `errbuf`, `errbuf_size`: 초기화 실패 시 에러 메시지

### 내부 연산

* `db_dir` 복사
* `tables` 배열 초기화
* `table_count = 0`, `table_capacity = 0`

### 반환

* 성공: `STATUS_OK`
* 실패: 메모리 할당 실패 등 적절한 에러 코드

### 한 줄 의미

**“이번 실행 세션 전체를 담는 메모리 컨테이너를 만든다.”**

---

## 10-2. `get_or_load_table_runtime`

```c
int get_or_load_table_runtime(
    ExecutionContext *ctx,
    const char *table_name,
    TableRuntime **out_table,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

테이블 runtime이 이미 있으면 꺼내고, 없으면 새로 로드합니다.

### 언제 호출되나

INSERT와 SELECT 시작 시 거의 항상

### 왜 필요한가

executor는 “이 테이블이 이미 준비됐는지”를 직접 알 필요 없이, 이 함수만 호출하면 되게 만들기 위해서입니다.

### 인자

* `ctx`: 실행 전체 context
* `table_name`: 필요한 테이블 이름
* `out_table`: 찾거나 새로 만든 runtime 포인터를 돌려줌
* `errbuf`, `errbuf_size`: 에러 메시지

### 내부 연산

1. `ctx->tables`에서 `table_name` 검색
2. 있으면 그대로 반환
3. 없으면 schema 로드
4. `id` 컬럼 유무 확인
5. `.data` 파일 보장
6. `id`가 있으면 index build
7. `TableRuntime` 생성 후 cache에 저장

### 반환

* 성공: `STATUS_OK`
* 실패: schema/storage/index 관련 적절한 에러

### 한 줄 의미

**“이 테이블을 실행 가능한 상태로 준비해서 돌려주는 함수”**

---

## 10-3. `free_execution_context`

```c
void free_execution_context(ExecutionContext *ctx);
```

### 의미

실행 종료 시 context 내부의 모든 메모리를 해제합니다.

### 언제 호출되나

`main()` 마지막

### 왜 필요한가

schema, table_name, B+Tree 노드, tables 배열 등 heap 메모리가 많이 생기기 때문입니다.

### 내부 연산

* 각 `TableRuntime` 순회
* schema 해제
* tree 해제
* table_name 해제
* tables 배열 해제
* db_dir 해제

### 반환

없음

### 한 줄 의미

**“세션 전체 메모리 정리 함수”**

---

# 11. 인덱스 빌드 함수

## 11-1. `build_id_index_for_table`

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
```

### 의미

기존 `.data` 파일 전체를 스캔해서 `id -> row_offset` 인덱스를 만듭니다.

### 언제 호출되나

`get_or_load_table_runtime()`가 처음 테이블을 로드할 때

### 왜 필요한가

프로그램이 재실행되면 메모리 인덱스는 사라지므로, 파일 데이터로부터 다시 복원해야 하기 때문입니다.

### 인자

* `db_dir`: DB 디렉터리
* `schema`: 테이블 schema
* `id_column_index`: row에서 id가 몇 번째 필드인지
* `out_tree`: 완성된 B+Tree
* `out_next_id`: 다음 자동 id
* `errbuf`, `errbuf_size`: 에러 버퍼

### 내부 연산

1. 빈 tree 초기화
2. `.data`를 처음부터 스트리밍 스캔
3. 각 row의 id 필드 읽기
4. id 문자열 검증
5. `bptree_insert(id, row_offset)`
6. 최대 id 추적
7. 종료 후 `next_id = max_id + 1`

### 반환

* 성공: `STATUS_OK`
* 실패: malformed id, duplicate id, storage 오류 등

### 한 줄 의미

**“디스크 데이터로부터 메모리 인덱스를 재구성하는 함수”**

---

## 11-2. `parse_stored_id_value`

```c
int parse_stored_id_value(
    const char *text,
    uint64_t *out_id,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

`.data`에 저장된 id 텍스트가 정상적인 자동 증가 id 형식인지 검사합니다.

### 언제 호출되나

index build 중, 기존 row들의 id를 읽을 때

### 왜 필요한가

인덱스는 정확한 정수 id를 가정하므로, 저장된 값이 이상하면 조용히 무시하면 안 되기 때문입니다.

### 인자

* `text`: 저장된 id 문자열
* `out_id`: 파싱 성공 시 숫자 값
* `errbuf`, `errbuf_size`: 실패 이유 기록

### 내부 연산

* 빈 문자열 검사
* 음수 여부 검사
* 정수 형식 검사
* leading zero 정책 검사
* overflow 검사

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_INDEX_ERROR` 또는 동급 에러

### 한 줄 의미

**“파일에 저장된 id가 인덱스에 올릴 수 있는 정상 값인지 확인”**

---

## 11-3. `try_parse_indexable_id_literal`

```c
int try_parse_indexable_id_literal(
    const LiteralValue *literal,
    uint64_t *out_id
);
```

### 의미

`WHERE id = literal`의 literal이 인덱스로 바로 바꿀 수 있는 값인지 검사합니다.

### 언제 호출되나

`can_use_id_index()` 내부

### 왜 필요한가

현재 엔진은 문자열 의미를 가지고 있으므로, 인덱스를 아무 숫자 비슷한 값에나 쓰면 안 되기 때문입니다.

### 인자

* `literal`: parser가 만든 literal AST
* `out_id`: 변환 가능한 경우 숫자 값

### 내부 연산

* `"123"` 또는 `123`은 허용
* `"00123"`, `1.0`, `-1` 등은 거부

### 반환

* 사용 가능: `1`
* 사용 불가: `0`

### 한 줄 의미

**“이 WHERE literal에 인덱스를 써도 SQL 의미가 안 깨지는지 판단”**

---

# 12. B+Tree 공개 함수

## 12-1. `bptree_init`

```c
int bptree_init(
    BPTree *out_tree,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

빈 B+Tree를 초기화합니다.

### 언제 호출되나

index build 시작 시, 또는 빈 테이블 runtime 생성 시

### 왜 필요한가

루트가 없는 초기 상태를 명확하게 만들기 위해서입니다.

### 인자

* `out_tree`: 초기화할 tree
* `errbuf`, `errbuf_size`: 필요 시 에러 메시지

### 반환

* 성공: `STATUS_OK`

### 한 줄 의미

**“트리의 시작 상태 만들기”**

---

## 12-2. `bptree_search`

```c
int bptree_search(
    const BPTree *tree,
    uint64_t key,
    long *out_offset,
    int *out_found,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

주어진 id key를 검색해서 row offset을 얻습니다.

### 언제 호출되나

`SELECT ... WHERE id = ?` 경로

### 왜 필요한가

이게 바로 인덱스를 쓰는 핵심 동작이기 때문입니다.

### 인자

* `tree`: 검색할 tree
* `key`: 찾을 id
* `out_offset`: 찾으면 대응되는 row_offset
* `out_found`: 찾았는지 여부
* `errbuf`, `errbuf_size`: 구조 이상 시 에러 메시지

### 내부 연산

* root부터 leaf까지 내려감
* leaf에서 key 이진/선형 검색
* 있으면 offset 반환

### 반환

* 성공: `STATUS_OK`
* tree 손상 등: `STATUS_INDEX_ERROR`

### 한 줄 의미

**“id를 파일 위치로 바꾸는 함수”**

---

## 12-3. `bptree_insert`

```c
int bptree_insert(
    BPTree *tree,
    uint64_t key,
    long row_offset,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

새로운 `(id -> row_offset)` 매핑을 tree에 삽입합니다.

### 언제 호출되나

* index build 중 각 기존 row를 읽을 때
* 새 INSERT가 성공한 직후

### 왜 필요한가

tree는 실행 중 계속 성장해야 하기 때문입니다.

### 인자

* `tree`: 수정할 tree
* `key`: id
* `row_offset`: 이 id row가 파일에서 시작하는 위치
* `errbuf`, `errbuf_size`: 에러 버퍼

### 내부 연산

* 삽입할 leaf 찾기
* 정렬 위치에 key 삽입
* overflow 시 split
* separator를 parent로 전파
* 필요하면 root split

### 반환

* 성공: `STATUS_OK`
* duplicate key, 메모리 부족, 구조 오류: `STATUS_INDEX_ERROR`

### 한 줄 의미

**“새 row가 파일에 생겼다는 사실을 인덱스에도 반영”**

---

## 12-4. `bptree_validate`

```c
int bptree_validate(
    const BPTree *tree,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

트리 구조가 B+Tree 불변식을 만족하는지 검사합니다.

### 언제 호출되나

주로 테스트에서

### 왜 필요한가

B+Tree는 “겉보기엔 동작해도 내부 구조가 이미 깨져 있는” 버그가 많기 때문입니다.

### 검사 항목 예시

* 모든 leaf depth 동일
* leaf chain 정렬
* 각 노드 key 정렬
* internal separator 범위 일관성
* parent 연결 정합성

### 반환

* 정상: `STATUS_OK`
* 이상: `STATUS_INDEX_ERROR`

### 한 줄 의미

**“트리가 멀쩡한지 확인하는 테스트용 안전장치”**

---

## 12-5. `bptree_destroy`

```c
void bptree_destroy(BPTree *tree);
```

### 의미

tree 전체 메모리를 해제합니다.

### 언제 호출되나

`free_execution_context()` 내부

### 왜 필요한가

노드가 다 heap 메모리이기 때문입니다.

### 반환

없음

### 한 줄 의미

**“트리 전체 정리”**

---

# 13. B+Tree 내부 helper 함수

이 함수들은 외부 API는 아니지만, 코드를 읽을 때 반드시 의미를 알아야 합니다.

## 13-1. `create_node`

```c
static BPTreeNode *create_node(int is_leaf);
```

### 의미

leaf인지 internal인지에 따라 새 노드를 만듭니다.

### 언제 호출되나

트리 초기 root 생성, split 시 새 노드 생성

### 반환

* 성공: 새 노드 포인터
* 실패: `NULL`

---

## 13-2. `find_leaf_node`

```c
static BPTreeNode *find_leaf_node(BPTreeNode *root, uint64_t key);
```

### 의미

주어진 key가 들어가야 할 leaf를 찾습니다.

### 언제 호출되나

search, insert 둘 다

### 왜 중요한가

B+Tree 연산의 시작점이 항상 “올바른 leaf 찾기”이기 때문입니다.

---

## 13-3. `insert_into_leaf`

```c
static int insert_into_leaf(BPTreeNode *leaf, uint64_t key, long row_offset);
```

### 의미

overflow가 없는 leaf에 key와 offset을 정렬 삽입합니다.

### 언제 호출되나

삽입 대상 leaf에 아직 공간이 있을 때

### 반환

* 성공: `STATUS_OK`
* duplicate 등: 에러

---

## 13-4. `split_leaf_and_insert`

```c
static int split_leaf_and_insert(
    BPTree *tree,
    BPTreeNode *leaf,
    uint64_t key,
    long row_offset,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

가득 찬 leaf를 둘로 나누고 새 key를 넣습니다.

### 언제 호출되나

leaf overflow 발생 시

### 내부 연산

* 기존 key + 새 key를 임시 버퍼에 모음
* 절반 기준으로 왼쪽/오른쪽 leaf 재배치
* `next` 포인터 연결 수정
* 오른쪽 첫 key를 separator로 parent에 전달

---

## 13-5. `insert_into_parent`

```c
static int insert_into_parent(
    BPTree *tree,
    BPTreeNode *left,
    uint64_t separator_key,
    BPTreeNode *right,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

split 후 만들어진 두 자식과 separator key를 parent에 반영합니다.

### 언제 호출되나

leaf split 또는 internal split 직후

### 왜 중요한가

B+Tree split의 본질은 “자식 분리 사실을 부모가 알게 하는 것”이기 때문입니다.

---

## 13-6. `split_internal_and_insert`

```c
static int split_internal_and_insert(
    BPTree *tree,
    BPTreeNode *node,
    uint64_t separator_key,
    BPTreeNode *right_child,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

가득 찬 internal node를 split하고 separator를 상위로 전파합니다.

### 언제 호출되나

parent도 overflow났을 때

### 한 줄 의미

**“split의 연쇄 전파를 처리하는 함수”**

---

# 14. Storage 확장 함수

인덱스를 제대로 쓰려면 storage가 “전체 row 읽기” 말고도 더 세밀한 기능을 제공해야 합니다.

## 14-1. `append_row_to_table_with_offset`

```c
int append_row_to_table_with_offset(
    const char *db_dir,
    const char *table_name,
    const Row *row,
    long *out_row_offset,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

row를 append하면서, 그 row가 시작한 파일 위치를 같이 반환합니다.

### 언제 호출되나

새 INSERT 경로

### 왜 필요한가

B+Tree에 넣을 value가 바로 이 `row_offset`이기 때문입니다.

### 인자

* `db_dir`, `table_name`: 대상 테이블
* `row`: 저장할 row
* `out_row_offset`: append 시작 위치
* `errbuf`, `errbuf_size`: 에러 버퍼

### 내부 연산

* append 모드로 파일 열기
* write 직전 `ftell()`
* row 직렬화 후 쓰기
* 시작 offset 반환

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_STORAGE_ERROR`

### 한 줄 의미

**“저장과 동시에 인덱스용 위치 정보도 얻는 함수”**

---

## 14-2. `scan_table_rows_with_offsets`

```c
typedef int (*RowScanCallback)(
    const Row *row,
    long row_offset,
    void *user_data,
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
```

### 의미

테이블을 처음부터 끝까지 스캔하되, 각 row의 파일 offset도 함께 callback에 넘깁니다.

### 언제 호출되나

* index build
* full scan SELECT
* 향후 다른 streaming 작업

### 왜 필요한가

1,000,000건 데이터를 한 번에 전부 메모리에 올리면 부담이 크기 때문입니다.

### 인자

* `expected_column_count`: schema 기준 컬럼 수 정규화용
* `callback`: 각 row마다 호출할 함수
* `user_data`: callback에 전달할 사용자 상태

### callback 반환 규칙

* `0`: 계속 스캔
* `1`: 정상 조기 종료
* `-1`: 에러

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_STORAGE_ERROR`

### 한 줄 의미

**“전체 데이터를 스트리밍으로 읽는 저수준 순회 함수”**

---

## 14-3. `read_row_at_offset`

```c
int read_row_at_offset(
    const char *db_dir,
    const char *table_name,
    long row_offset,
    size_t expected_column_count,
    Row *out_row,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

파일의 특정 위치로 바로 이동해서 row 하나만 읽어옵니다.

### 언제 호출되나

`SELECT ... WHERE id = ?` 인덱스 경로

### 왜 필요한가

B+Tree의 이점을 실제 성능으로 연결하는 마지막 고리이기 때문입니다.

### 인자

* `row_offset`: 읽고 싶은 row의 시작 위치
* `expected_column_count`: schema 길이 맞춤용
* `out_row`: 복원된 row

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_STORAGE_ERROR`

### 한 줄 의미

**“인덱스가 가리킨 row만 정확히 집어서 읽는 함수”**

---

## 14-4. `free_row`

```c
void free_row(Row *row);
```

### 의미

단일 row의 내부 메모리를 해제합니다.

### 언제 호출되나

`read_row_at_offset()` 결과나 callback 내부 임시 row 정리 시

### 왜 필요한가

기존 `free_rows()`는 배열용이고, indexed select는 row 하나만 다루는 경우가 많기 때문입니다.

---

# 15. Executor 수정 함수

여기가 이번 과제의 중심입니다.
AST를 보고 “인덱스를 쓸지 말지”, “자동 id를 붙일지”, “파일과 인덱스를 어떻게 같이 업데이트할지”를 결정합니다.

## 15-1. 수정된 `execute_statement`

```c
int execute_statement(
    ExecutionContext *ctx,
    const Statement *stmt,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

statement 실행의 최상위 분기 함수

### 언제 호출되나

`main()`에서 statement 하나를 파싱한 직후

### 왜 시그니처가 바뀌나

이제는 `db_dir`만으로 부족하고, 런타임 cache와 index 상태가 필요하기 때문입니다.

### 내부 연산

* `stmt->type` 검사
* INSERT면 `execute_insert(ctx, ...)`
* SELECT면 `execute_select(ctx, ...)`

---

## 15-2. `execute_insert`

```c
static int execute_insert(
    ExecutionContext *ctx,
    const InsertStatement *stmt,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

INSERT 실행 전용 함수

### 언제 호출되나

`execute_statement()` 내부에서 INSERT 분기 시

### 내부 연산

1. `get_or_load_table_runtime()`
2. insert 입력 검증
3. `generated_id = table->next_id`
4. 최종 row 조립
5. row append + offset 획득
6. B+Tree 삽입
7. `next_id++`
8. `ExecResult` 채우기

### 반환

* 성공: `STATUS_OK`
* 실패: exec/schema/storage/index 관련 에러

### 한 줄 의미

**“INSERT를 파일과 인덱스 둘 다 반영하는 함수”**

---

## 15-3. `validate_insert_columns_for_auto_id`

```c
static int validate_insert_columns_for_auto_id(
    const TableRuntime *table,
    const InsertStatement *stmt,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

`id` 자동 생성 정책에 맞는 INSERT 형태인지 검증합니다.

### 언제 호출되나

`execute_insert()` 초반

### 왜 필요한가

이번 과제에서는 `id`를 사용자가 직접 넣으면 안 되기 때문입니다.

### 검사 규칙

* `INSERT INTO table VALUES (...)`이면 값 개수는 `schema - 1`
* column list 사용 시 `id` 컬럼이 들어오면 에러
* 중복 컬럼 에러
* 없는 컬럼 에러

### 반환

* 성공: `STATUS_OK`
* 실패: `STATUS_EXEC_ERROR`

### 한 줄 의미

**“이 INSERT는 자동 id 모델에 맞는가?”를 검사**

---

## 15-4. `build_insert_row_with_generated_id`

```c
static int build_insert_row_with_generated_id(
    const TableRuntime *table,
    const InsertStatement *stmt,
    uint64_t generated_id,
    Row *out_row,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

schema 길이에 맞는 최종 저장 row를 만듭니다.

### 언제 호출되나

검증이 끝난 뒤, 실제 저장 직전

### 왜 필요한가

INSERT 입력 형태는 다양하지만, `.data`에 저장할 row는 항상 완전한 schema 길이여야 하기 때문입니다.

### 인자

* `table`: schema와 id 위치를 알기 위해 필요
* `stmt`: 사용자가 입력한 원본 INSERT AST
* `generated_id`: 시스템이 만든 id
* `out_row`: 최종 row

### 내부 연산

* schema 길이만큼 빈 row 생성
* id 위치에 `generated_id` 문자열 저장
* 나머지 컬럼을 column list 규칙에 맞게 채움

### 반환

* 성공: `STATUS_OK`

### 한 줄 의미

**“사용자 입력을 실제 저장 포맷 row로 변환”**

---

## 15-5. `execute_select`

```c
static int execute_select(
    ExecutionContext *ctx,
    const SelectStatement *stmt,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

SELECT 실행 진입 함수

### 언제 호출되나

`execute_statement()`의 SELECT 분기

### 내부 연산

1. `get_or_load_table_runtime()`
2. projection/where 컬럼 유효성 검사
3. `can_use_id_index()` 판정
4. 가능하면 indexed path
5. 아니면 full scan path

### 한 줄 의미

**“이 SELECT를 빠른 길로 갈지, 기존 길로 갈지 결정하는 함수”**

---

## 15-6. `can_use_id_index`

```c
static int can_use_id_index(
    const TableRuntime *table,
    const SelectStatement *stmt,
    uint64_t *out_id_key
);
```

### 의미

이번 SELECT가 B+Tree를 사용할 수 있는지 판정합니다.

### 언제 호출되나

`execute_select()` 초반

### 왜 필요한가

SELECT가 모두 인덱스를 탈 수 있는 건 아니기 때문입니다.

### 검사 조건

* WHERE가 있는가
* WHERE 컬럼이 `id`인가
* 테이블에 id 컬럼이 있는가
* literal이 indexable integer인가

### 반환

* 사용 가능: `1`
* 사용 불가: `0`

### 한 줄 의미

**“인덱스 경로 진입 허가 여부 판단”**

---

## 15-7. `execute_select_with_id_index`

```c
static int execute_select_with_id_index(
    ExecutionContext *ctx,
    TableRuntime *table,
    const SelectStatement *stmt,
    uint64_t id_key,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

B+Tree를 사용해 id 기반 SELECT를 수행합니다.

### 언제 호출되나

`can_use_id_index()`가 참일 때

### 내부 연산

1. `bptree_search(id_key)`
2. 못 찾으면 빈 결과
3. 찾으면 `read_row_at_offset()`
4. 방어적으로 where 재검사
5. projection 적용
6. 결과 1 row 구성
7. `used_index = 1`

### 반환

* 성공: `STATUS_OK`

### 한 줄 의미

**“id 조회용 빠른 경로”**

---

## 15-8. `execute_select_with_full_scan`

```c
static int execute_select_with_full_scan(
    ExecutionContext *ctx,
    TableRuntime *table,
    const SelectStatement *stmt,
    ExecResult *out_result,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

기존 방식대로 전체 테이블을 스캔해서 SELECT를 수행합니다.

### 언제 호출되나

인덱스를 사용할 수 없을 때

### 내부 연산

* `scan_table_rows_with_offsets()` 또는 기존 `read_all_rows_from_table()`
* row마다 where 검사
* 통과하면 projection
* 결과 배열에 append
* `used_index = 0`

### 한 줄 의미

**“기존 동작을 유지하는 느린 경로”**

---

## 15-9. `project_single_row`

```c
static int project_single_row(
    const TableSchema *schema,
    const SelectStatement *stmt,
    const Row *source_row,
    Row *out_projected,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

원본 row에서 SELECT 대상 컬럼만 골라 새 row를 만듭니다.

### 언제 호출되나

indexed path, full scan path 모두에서 공통 사용

### 왜 필요한가

`SELECT *`와 `SELECT name, age`가 다르기 때문입니다.

### 반환

* 성공: `STATUS_OK`

### 한 줄 의미

**“결과로 보여줄 컬럼만 잘라내는 함수”**

---

## 15-10. `append_result_row`

```c
static int append_result_row(
    QueryResult *result,
    const Row *projected_row,
    char *errbuf,
    size_t errbuf_size
);
```

### 의미

SELECT 결과 집합에 새 row를 하나 추가합니다.

### 언제 호출되나

조건에 맞는 row를 찾을 때마다

### 왜 필요한가

동적 결과 배열 관리가 반복되므로 분리하는 게 깔끔하기 때문입니다.

### 반환

* 성공: `STATUS_OK`
* 실패: 메모리 부족 등

### 한 줄 의미

**“SELECT 결과 배열 확장 함수”**

---

# 16. 조건 검사 관련 핵심 helper

## 16-1. `row_matches_where_clause`

기존 executor에도 유사한 함수가 있습니다. 이번에도 계속 필요합니다. 

```c
static int row_matches_where_clause(
    const TableSchema *schema,
    const Row *row,
    const WhereClause *where
);
```

### 의미

주어진 row가 WHERE 조건을 만족하는지 검사합니다.

### 언제 호출되나

* full scan SELECT
* indexed SELECT에서 방어적 재검증

### 왜 필요한가

where 의미를 한 곳에 모아야 하기 때문입니다.

### 반환

* 만족: `1`
* 불만족: `0`

### 한 줄 의미

**“한 row에 대한 WHERE 판정기”**

---

# 17. 벤치마크 함수

이 부분은 구현보다 “비교 실험”이 목적입니다.

## 17-1. `run_benchmark`

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

### 의미

대용량 삽입과 조회 성능 비교를 한 번에 실행합니다.

### 언제 호출되나

별도 benchmark 바이너리 또는 테스트 도구에서

### 왜 필요한가

이번 과제 요구사항이 “1,000,000건 insert 후 indexed select와 linear select 비교”이기 때문입니다.

### 내부 연산

* benchmark schema 준비
* bulk insert
* 여러 번 id SELECT 측정
* 여러 번 non-id SELECT 측정
* 평균 시간, 배율 계산

### 반환

* 성공: `STATUS_OK`
* 실패: 적절한 에러

### 한 줄 의미

**“이번 과제의 성능 데모 자동화 함수”**

---

# 18. 테스트 관점에서 각 함수의 역할을 연결해서 보기

학습할 때는 “함수 정의”만 보지 말고, **이 함수가 어떤 테스트로 검증되는지** 같이 보면 이해가 빨라집니다.

## 18-1. B+Tree 함수는 이런 질문에 답해야 합니다

* 순차 삽입해도 검색이 되나?
* split가 나도 key를 잃지 않나?
* duplicate key를 막나?
* leaf chain이 정렬돼 있나?

즉 `bptree_insert`, `bptree_search`, `bptree_validate`를 함께 봐야 합니다.

## 18-2. Runtime 함수는 이런 질문에 답해야 합니다

* 한 번 만든 index를 재사용하나?
* 기존 `.data`에서 next_id를 올바르게 복원하나?
* malformed id가 있으면 조용히 넘어가지 않고 실패하나?

즉 `get_or_load_table_runtime`, `build_id_index_for_table`, `parse_stored_id_value`를 함께 봐야 합니다.

## 18-3. Storage 확장 함수는 이런 질문에 답해야 합니다

* append 직후 얻은 offset으로 다시 읽으면 같은 row가 나오나?
* 전체 scan이 메모리 폭발 없이 동작하나?

즉 `append_row_to_table_with_offset`, `scan_table_rows_with_offsets`, `read_row_at_offset`를 세트로 봐야 합니다.

## 18-4. Executor 함수는 이런 질문에 답해야 합니다

* insert 시 id가 자동으로 붙나?
* index path와 full scan path가 정확히 갈리나?
* 결과는 기존 SQL 의미와 동일한가?

즉 `execute_insert`, `execute_select`, `can_use_id_index`를 중심으로 보면 됩니다.

---

# 19. 코드를 읽을 때의 추천 순서

학습용으로는 아래 순서가 제일 좋습니다.

## 1단계

`main -> tokenize_sql -> parse_next_statement -> execute_statement`

이걸 먼저 보고 “전체 파이프라인”을 이해합니다. 현재 프로젝트의 핵심이 바로 이 흐름입니다. 

## 2단계

`execute_insert`, `execute_select`, `load_table_schema`, `read_all_rows_from_table`

이걸 보고 현재 엔진이 “어떻게 실제 저장과 조회를 하는지”를 이해합니다. 

## 3단계

이번 과제 추가분:
`get_or_load_table_runtime -> build_id_index_for_table -> bptree_insert/search`

여기서 “인덱스가 어디에 붙는지”를 이해합니다.

## 4단계

`append_row_to_table_with_offset`, `read_row_at_offset`

여기서 “왜 인덱스 value가 row 자체가 아니라 offset인지”를 이해합니다.

## 5단계

B+Tree split helper들

마지막에 자료구조 내부를 봅니다.
처음부터 split 코드부터 보면 전체 목적을 놓치기 쉽습니다.

---

# 20. 마지막으로, 이 설계를 한 문장씩 압축하면

* **main**은 흐름을 조립한다.
* **lexer**는 SQL을 토큰으로 자른다.
* **parser**는 토큰을 AST로 만든다.
* **executor**는 AST를 실제 행동으로 바꾼다.
* **storage**는 파일 포맷을 책임진다.
* **runtime**은 실행 중 테이블 상태를 캐시한다.
* **bptree**는 id를 파일 위치로 빠르게 바꿔준다.
* **indexed select**는 파일 전체가 아니라 한 row만 읽는다.
* **full scan select**는 기존 의미를 그대로 유지한다.

즉 이번 과제의 진짜 핵심은:

**“기존 SQL 처리기의 의미는 그대로 보존하면서, id 조회에만 별도 빠른 경로를 추가하는 것”**입니다.

원하시면 다음 답변에서는 이 내용을 바탕으로 바로 Codex에 붙일 수 있게 **“구현 순서 체크리스트 + 파일별 작업 순서 + 함수 stub까지 포함한 작업 지시서”** 형태로 바꿔드리겠습니다.
