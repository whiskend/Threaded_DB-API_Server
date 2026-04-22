# Codex 실행 프롬프트: Threaded DB API Server 구현

> 이 파일 전체를 Codex에 그대로 넣어라.  
> 질문하지 말고 가능한 범위에서 바로 구현해라.  
> 애매한 부분은 과제 목표에 맞게 합리적으로 결정하고, 결정 이유는 `agent.md`와 `docs/decision.md`에 기록해라.

---

## 0. 목표

C 언어로 **미니 DBMS API 서버**를 구현한다.

최종 구조:

```txt
client(http/js)
→ server
→ queue
→ thread pool
→ SQL + B+ tree mini DB
→ server
→ client
```

외부 클라이언트가 HTTP/JSON으로 SQL을 보내면 서버가 내부 DB 엔진에서 처리하고 JSON으로 결과를 돌려준다.

반드시 구현할 것:

- C HTTP/JSON API 서버
- 작업 큐
- Thread Pool
- Worker Thread 병렬 SQL 처리
- 기존 SQL 처리기와 B+ Tree 인덱스 재사용
- READ / WRITE / MIXED 성능 비교
- worker thread 개수별 대기 시간, 처리 시간 비교
- 최소 1,000,000개 INSERT 벤치마크
- JS로 HTTP 요청을 보내고 결과 차트 출력
- 단위 테스트
- 기능 테스트
- 엣지 케이스 테스트
- GitHub Actions CI/CD
- `seonho` 브랜치 push
- 가능하면 Issue와 PR 생성

---

## 1. 레포 작업

대상 레포:

```txt
https://github.com/whiskend/Threaded_DB-API_Server
```

반드시 아래 흐름으로 작업한다.

```bash
git clone https://github.com/whiskend/Threaded_DB-API_Server.git
cd Threaded_DB-API_Server

git fetch origin
git checkout main || git checkout -b main origin/main
git pull --ff-only origin main

git checkout -B seonho
```

작업 완료 후:

```bash
git add .
git commit -m "feat: add threaded DB API server benchmark"
git push -u origin seonho
```

가능하면 GitHub CLI로 Issue와 PR을 만든다.

```bash
gh issue create \
  --title "Threaded DB API Server 구현" \
  --body-file docs/issue.md

gh pr create \
  --base main \
  --head seonho \
  --title "feat: threaded DB API server" \
  --body-file docs/pr.md
```

GitHub 인증이 안 되어 있으면 실패했다고 멈추지 말고 아래 파일을 만든다.

```txt
docs/issue.md
docs/pr.md
```

---

## 2. 제일 먼저 할 일

작업 시작 즉시 `agent.md`를 만든다.

이미 있으면 먼저 읽고 이어서 작성한다.

`agent.md`에는 매 단계마다 아래 내용을 기록한다.

```txt
현재 단계:
완료한 것:
남은 것:
의사결정:
문제:
해결:
테스트 결과:
벤치마크 결과:
```

컨텍스트가 줄어도 `agent.md`만 보고 이어서 작업할 수 있게 만든다.

작업 중간마다 아래도 확인한다.

```bash
find . -maxdepth 3 -type f | sort
grep -R "btree\|BTree\|sql\|SQL" -n src include db tests 2>/dev/null | head -100
```

기존 코드가 있으면 삭제하지 말고 재사용한다.

---

## 3. 코드 스타일

코드는 짧고 읽기 쉽게 작성한다.

- C 언어 사용
- 외부 C 라이브러리 최소화
- `pthread`, POSIX socket 사용
- 변수명은 가능하면 10자 이하
- 너무 줄여서 못 알아보게 만들지 말 것
- 주석은 초등학생도 이해하게 작성
- 복잡한 추상화 금지
- 블랙박스 코드 금지

좋은 변수명:

```c
int qlen;
int wcnt;
int rowid;
char sql[2048];
```

좋은 주석:

```c
// 큐가 비면 worker는 여기서 기다린다.
pthread_cond_wait(&q->cond, &q->lock);
```

---

## 4. 기존 DB 엔진 재사용 원칙

현재 레포에는 기존 SQL 처리기, B+ Tree, DB 관련 코드가 있을 수 있다.

원칙:

1. 기존 SQL 처리기를 먼저 찾는다.
2. 기존 B+ Tree를 먼저 찾는다.
3. 기존 인터페이스가 복잡하면 wrapper를 만든다.
4. 기존 코드를 망가뜨리지 말고 API 서버에서 호출한다.
5. 기존 코드가 부족하면 최소 기능만 추가한다.

wrapper 예시:

```c
int db_exec(char *sql, char *out, int max);
```

`db_exec()`는 서버가 SQL을 넘기는 유일한 입구로 만든다.

---

## 5. DB 기능

도서관 도서 조회 시스템으로 구현한다.

테이블:

```txt
books
```

컬럼:

```txt
id
title
author
year
```

지원 SQL:

```sql
CREATE TABLE books;
INSERT INTO books VALUES (1, 'title', 'author', 2024);
SELECT * FROM books WHERE id = 1;
SELECT * FROM books;
DELETE FROM books WHERE id = 1;
```

완전한 SQL 엔진이 아니어도 된다.  
과제에 필요한 명령만 정확히 처리한다.

필수 엣지 케이스:

- 빈 SQL
- 너무 긴 SQL
- 잘못된 SQL
- 없는 id 검색
- 중복 id INSERT
- DELETE 없는 id
- title/author에 공백 포함
- 1개 worker
- 많은 worker
- 동시에 SELECT
- 동시에 INSERT
- READ/WRITE 섞임

---

## 6. 동시성 정책

DB lock은 `pthread_rwlock_t`를 사용한다.

정책:

```txt
SELECT              → read 는 락을 하지 않고 바로 불러온다.
INSERT/DELETE/CREATE → write lock
```

이유를 `docs/decision.md`에 적는다.

핵심 설명:

```txt
SELECT는 DB 구조를 바꾸지 않으므로 여러 thread가 동시에 실행해도 된다.
INSERT/DELETE는 B+ Tree와 row 저장소를 바꾸므로 단독 실행해야 한다.
모든 요청에 mutex 전역락을 걸면 DB가 사실상 싱글 스레드가 된다.
rwlock을 쓰면 읽기는 병렬, 쓰기는 안전하게 처리할 수 있다.
```

---

## 7. HTTP/JSON 선택 이유

`docs/decision.md`에 반드시 적는다.

정리 문장:

```txt
HTTP도 TCP 위에서 동작한다.
비교 대상은 raw TCP 직접 프로토콜이다.

HTTP/JSON을 선택한 이유는 브라우저, curl, JS에서 바로 테스트할 수 있고
요청/응답 구조가 명확하기 때문이다.

이번 과제의 핵심은 네트워크 프로토콜 발명이 아니라
DBMS API 서버, Thread Pool, 동시성 처리이므로
통신 방식은 단순하고 검증하기 쉬운 HTTP/JSON을 선택했다.
```

---

## 8. 서버 API

최소 API:

### 8.1 Health

```http
GET /health
```

응답:

```json
{"ok":true}
```

### 8.2 SQL 실행

```http
POST /sql
Content-Type: application/json

{"sql":"SELECT * FROM books WHERE id = 1;"}
```

응답 예:

```json
{
  "ok": true,
  "rows": [{"id":1,"title":"C Book","author":"Kim","year":2024}],
  "wait_ms": 1.2,
  "work_ms": 0.8,
  "total_ms": 2.0,
  "worker": 3
}
```

실패 예:

```json
{
  "ok": false,
  "err": "bad sql",
  "wait_ms": 0.2,
  "work_ms": 0.1,
  "total_ms": 0.3,
  "worker": 1
}
```

### 8.3 통계

```http
GET /stats
```

응답:

```json
{
  "workers": 8,
  "queue_now": 2,
  "queue_max": 1024,
  "done": 1200
}
```

### 8.4 간단 벤치마크

```http
POST /bench
Content-Type: application/json

{"mode":"write","count":1000000}
```

응답:

```json
{
  "ok": true,
  "mode": "write",
  "count": 1000000,
  "workers": 8,
  "qps": 15000,
  "avg_wait_ms": 2.1,
  "avg_work_ms": 0.9,
  "total_ms": 67000
}
```

단, worker 개수별 비교는 `bench/bench.js`가 서버를 재시작하면서 수행한다.

---

## 9. HTTP 서버 구현 규칙

복잡하게 만들지 말고 최소 HTTP만 구현한다.

- 한 연결에 요청 1개만 처리
- `GET /health`
- `GET /stats`
- `POST /sql`
- `POST /bench`
- `GET /` 또는 `GET /chart`는 간단한 chart HTML 반환 가능
- chunked request는 지원하지 않아도 됨
- `Content-Length` 기반 body 읽기
- body 최대 8192 bytes
- SQL 최대 2048 bytes
- 너무 크면 HTTP 413
- 큐가 꽉 차면 HTTP 503
- 잘못된 요청은 HTTP 400
- 응답은 JSON

CORS 헤더를 넣는다.

```txt
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET,POST,OPTIONS
Access-Control-Allow-Headers: Content-Type
```

---

## 10. Thread Pool

구조:

```txt
main thread
  ↓
HTTP 요청 수신
  ↓
작업 큐에 넣음
  ↓
worker thread가 큐에서 꺼냄
  ↓
SQL 실행
  ↓
worker가 client socket에 응답 작성
```

파일 예시:

```txt
src/main.c
src/http.c
src/http.h
src/queue.c
src/queue.h
src/pool.c
src/pool.h
src/db_api.c
src/db_api.h
src/time_ms.c
src/time_ms.h
```

기존 파일 구조가 있으면 그 구조에 맞게 조정한다.

Job 구조 예:

```c
typedef struct {
    int fd;
    char sql[2048];
    double in_ms;
} Job;
```

worker는 아래 값을 측정한다.

```txt
wait_ms  = worker가 시작한 시간 - 큐에 들어온 시간
work_ms  = SQL 처리 끝난 시간 - SQL 처리 시작 시간
total_ms = 응답 직전 시간 - 큐에 들어온 시간
```

---

## 11. Queue

고정 크기 ring buffer로 만든다.

기본값:

```txt
QUEUE_MAX = 1024
```

이유:

```txt
무제한 큐는 요청이 폭주하면 메모리를 터뜨릴 수 있다.
1024는 과제용 서버에서 충분히 크고 관리하기 쉽다.
큐가 가득 차면 요청을 오래 붙잡지 말고 503으로 거절한다.
```

필수 함수:

```c
void q_init(Queue *q);
int q_push(Queue *q, Job *j);
int q_pop(Queue *q, Job *j);
int q_len(Queue *q);
```

---

## 12. Worker 개수

서버 실행 옵션:

```bash
./server --port 8080 --workers 8
```

기본값:

```txt
CPU core 수 * 2
```

벤치마크 비교:

```txt
1, 2, 4, 8, 16, 32
```

문서 설명:

```txt
이 서버는 네트워크 I/O와 DB 처리를 함께 한다.
I/O 작업은 기다리는 시간이 있으므로 CPU 코어 수와 같은 thread만 쓰면 자원이 남을 수 있다.
그래서 CPU 코어 수의 2~3배까지 worker를 늘려보고 실제 wait time, work time, QPS를 비교한다.
```

---

## 13. 벤치마크

폴더:

```txt
bench/
```

파일:

```txt
bench/bench.js
bench/chart.html
bench/result.json
```

### 13.1 bench.js

Node.js 기본 모듈만 사용한다.  
외부 npm 패키지 금지.

역할:

1. `make`로 서버 빌드
2. worker 개수별로 서버 실행
3. `/health` 확인
4. HTTP로 `/sql` 요청을 많이 보냄
5. `read`, `write`, `mixed` 측정
6. 결과를 `bench/result.json`에 저장
7. 서버 종료 후 다음 worker 개수 테스트

실행 예:

```bash
node bench/bench.js --workers 1,2,4,8,16,32 --count 1000000 --conc 128
```

측정 모드:

```txt
write: INSERT만 실행
read: SELECT만 실행
mixed: INSERT와 SELECT를 섞음
```

결과 형식:

```json
[
  {
    "workers": 1,
    "mode": "write",
    "count": 1000000,
    "conc": 128,
    "total_ms": 100000,
    "avg_wait_ms": 5.1,
    "avg_work_ms": 1.2,
    "qps": 10000
  }
]
```

주의:

- CI에서는 1,000,000개 벤치마크를 돌리지 않는다.
- 실제 과제 벤치마크는 아래로 실행한다.

```bash
make bench COUNT=1000000
```

### 13.2 chart.html

브라우저에서 열면 `bench/result.json`을 읽어 차트를 그린다.

외부 라이브러리 금지.  
HTML + JS + Canvas만 사용한다.

차트:

1. worker 개수별 total time
2. worker 개수별 avg wait time
3. worker 개수별 avg work time
4. worker 개수별 QPS

---

## 14. Makefile

반드시 아래 명령이 동작하게 만든다.

```bash
make
make run
make test
make api-test
make bench
make clean
```

예:

```bash
make run WORKERS=8 PORT=8080
make bench COUNT=1000000
make bench COUNT=10000
```

`make bench` 기본값은 가능하면 `COUNT=1000000`으로 둔다.  
너무 오래 걸릴 수 있으므로 README에 `COUNT=10000` 빠른 테스트 방법도 적는다.

---

## 15. 테스트

폴더:

```txt
tests/
```

필수:

```txt
tests/test_btree.c
tests/test_sql.c
tests/test_queue.c
tests/api_test.sh
```

기존 테스트 구조가 있으면 거기에 맞춘다.

### 15.1 단위 테스트

검증:

- B+ Tree insert/search
- 없는 key 검색
- 중복 key insert
- delete
- SQL 파싱
- Queue push/pop
- Queue full
- Queue empty
- worker 1개 처리

### 15.2 API 테스트

`tests/api_test.sh`에서 서버를 띄우고 curl로 검증한다.

검증:

```bash
GET /health
POST /sql INSERT
POST /sql SELECT
POST /sql DELETE
GET /stats
bad json
bad sql
```

### 15.3 엣지 케이스

테스트에 포함한다.

- 빈 body
- 빈 SQL
- 긴 SQL
- 잘못된 JSON
- 없는 id
- 중복 insert
- queue full 가능하면 테스트
- 동시 요청 100개 이상

---

## 16. CI/CD

파일:

```txt
.github/workflows/ci.yml
```

실행:

```yaml
name: ci

on:
  push:
  pull_request:

jobs:
  build-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install tools
        run: sudo apt-get update && sudo apt-get install -y build-essential curl nodejs
      - name: Build
        run: make
      - name: Unit test
        run: make test
      - name: API test
        run: make api-test
```

CI에서는 대규모 1,000,000 벤치마크를 돌리지 않는다.

---

## 17. 문서

반드시 만든다.

```txt
README.md
docs/architecture.md
docs/decision.md
docs/review_phase.md
docs/test.md
docs/bench.md
docs/issue.md
docs/pr.md
agent.md
```

### README.md

포함:

- 프로젝트 설명
- 구조
- 빌드
- 실행
- API
- 테스트
- 벤치마크
- 차트 확인
- 한계점

### docs/architecture.md

아래 구조 설명:

```txt
client(http/js)
→ server
→ queue
→ thread pool
→ SQL + B+ tree mini DB
→ server
→ client
```

### docs/decision.md

반드시 정리:

1. 왜 HTTP/JSON인가?
2. 왜 Thread Pool인가?
3. 왜 Queue인가?
4. 왜 Queue max 1024인가?
5. 왜 rwlock인가?
6. 왜 WRITE는 배타 lock인가?
7. 왜 READ는 공유 lock인가?
8. 왜 worker 수를 1,2,4,8,16,32로 비교하는가?
9. 왜 B+ Tree인가?

### docs/review_phase.md

학습용 코드리뷰 단계를 나눈다.

```txt
Phase 1. 전체 구조
Phase 2. HTTP 요청 흐름
Phase 3. Queue
Phase 4. Thread Pool
Phase 5. SQL 처리
Phase 6. B+ Tree
Phase 7. Lock
Phase 8. Benchmark
Phase 9. Chart
```

각 단계마다 볼 파일과 질문을 적는다.

---

## 18. 개발 순서

아래 순서로 진행한다.

### Phase 0. 준비

- repo 구조 확인
- `seonho` 브랜치 생성
- `agent.md` 작성 시작
- 기존 SQL/B+Tree 위치 확인

### Phase 1. DB API 연결

- 기존 SQL 처리기와 B+Tree 파악
- `db_exec()` wrapper 작성
- books table 명령 연결
- read/write 판별 함수 작성

### Phase 2. Queue

- ring buffer queue 구현
- full/empty 처리
- 단위 테스트 작성

### Phase 3. Thread Pool

- worker 생성
- job pop
- wait/work/total time 측정
- 응답 작성

### Phase 4. HTTP 서버

- socket accept
- HTTP parse
- JSON에서 sql 추출
- `/health`, `/stats`, `/sql`, `/bench`

### Phase 5. Lock

- `pthread_rwlock_t db_lock`
- SELECT read lock
- INSERT/DELETE write lock
- 동시성 테스트

### Phase 6. Benchmark

- `bench/bench.js`
- worker 개수별 서버 재시작
- read/write/mixed 요청
- `result.json` 저장

### Phase 7. Chart

- `bench/chart.html`
- canvas 차트 4개
- 결과를 쉽게 비교

### Phase 8. Test

- unit test
- api test
- edge test

### Phase 9. CI/CD

- GitHub Actions
- build/test/api-test 자동화

### Phase 10. 문서화

- README
- docs
- issue/pr body

### Phase 11. Push/PR

- commit
- push
- issue/pr 생성 또는 파일 생성

---

## 19. 구현 디테일

### 19.1 JSON 파서

복잡한 JSON 파서 만들지 말고 단순 추출한다.

예:

```json
{"sql":"SELECT * FROM books WHERE id = 1;"}
```

`"sql"` 키 뒤의 문자열만 뽑는다.

단, 아래는 처리한다.

- 키 없음
- 값 없음
- 너무 긴 값
- escape 처리는 최소한 `\"`, `\\` 정도만 처리

### 19.2 HTTP 응답

함수 예:

```c
void send_json(int fd, int code, const char *body);
```

응답 헤더:

```txt
HTTP/1.1 200 OK
Content-Type: application/json
Access-Control-Allow-Origin: *
Content-Length: N
```

### 19.3 시간 측정

`clock_gettime(CLOCK_MONOTONIC, ...)` 사용.

함수:

```c
double now_ms(void);
```

### 19.4 통계

전역 통계:

```c
typedef struct {
    int workers;
    int qmax;
    long done;
} Stat;
```

`done` 갱신은 mutex 또는 atomic 사용.

---

## 20. 추가 차별점

가능하면 구현한다.

우선순위:

1. `/stats` 제공
2. 요청별 `wait_ms`, `work_ms`, `total_ms`
3. queue full이면 503
4. JS chart
5. worker별 벤치마크 자동화
6. read/write/mixed 비교
7. 문서에 의사결정 정리
8. `GET /chart`로 chart HTML 제공 가능하면 추가

---

## 21. 최종 체크리스트

작업 완료 전 반드시 확인한다.

```txt
[ ] seonho 브랜치에서 작업했다.
[ ] C 서버가 빌드된다.
[ ] make가 동작한다.
[ ] make run이 동작한다.
[ ] /health가 동작한다.
[ ] /sql INSERT가 동작한다.
[ ] /sql SELECT가 동작한다.
[ ] /sql DELETE가 동작한다.
[ ] /stats가 동작한다.
[ ] Thread Pool이 동작한다.
[ ] Queue가 동작한다.
[ ] SELECT는 read lock이다.
[ ] WRITE는 write lock이다.
[ ] 단위 테스트가 있다.
[ ] 기능 테스트가 있다.
[ ] 엣지 케이스 테스트가 있다.
[ ] make test가 통과한다.
[ ] make api-test가 통과한다.
[ ] make bench COUNT=1000000 명령이 있다.
[ ] worker 개수별 결과가 저장된다.
[ ] bench/chart.html이 있다.
[ ] GitHub Actions가 있다.
[ ] README가 있다.
[ ] docs/decision.md가 있다.
[ ] docs/review_phase.md가 있다.
[ ] agent.md에 진행 내용이 기록됐다.
[ ] commit 했다.
[ ] origin seonho로 push 했다.
[ ] 가능하면 Issue/PR을 만들었다.
```

---

## 22. 최종 보고 형식

작업이 끝나면 아래 형식으로만 보고한다.

```txt
완료 요약

1. 브랜치
- seonho

2. 구현한 기능
-

3. 만든/수정한 파일
-

4. 테스트 결과
- make:
- make test:
- make api-test:

5. 벤치마크
- 실행 명령:
- result.json 위치:
- chart.html 위치:

6. 실행 방법
-

7. PR
- PR 링크:
- 인증 실패 시 docs/pr.md 위치:

8. 한계점
-
```

실제로 실행한 결과만 적어라.  
안 돌린 테스트를 통과했다고 쓰지 마라.
