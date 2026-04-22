# Review Phase

## Phase 1. 전체 구조

볼 파일:

- `README.md`
- `docs/architecture.md`
- `Makefile`

질문:

- 서버 binary와 기존 `sql_processor`가 분리되어 있는가?
- 요청 흐름이 queue와 worker를 거치는가?

## Phase 2. HTTP 요청 흐름

볼 파일:

- `src/http.c`
- `include/http.h`

질문:

- `Content-Length` 기반으로 body를 읽는가?
- `/health`, `/stats`, `/sql`, `/bench`, `/chart`가 분리되어 있는가?
- 잘못된 요청은 400, 너무 큰 요청은 413, 큐 full은 503인가?

## Phase 3. Queue

볼 파일:

- `src/queue.c`
- `include/queue.h`
- `tests/test_queue.c`

질문:

- ring buffer가 FIFO 순서를 지키는가?
- `QUEUE_MAX=1024`에서 push를 거절하는가?
- 비어 있을 때 worker가 condition variable에서 기다리는가?

## Phase 4. Thread Pool

볼 파일:

- `src/pool.c`
- `include/pool.h`

질문:

- worker 수만큼 thread가 생성되는가?
- worker가 `wait_ms`, `work_ms`, `total_ms`를 측정하는가?
- 완료된 작업 수가 `/stats`에 반영되는가?

## Phase 5. SQL 처리

볼 파일:

- `src/db_api.c`
- `include/db_api.h`
- `tests/test_sql.c`

질문:

- `db_exec()`가 서버의 유일한 DB entry point인가?
- CREATE/INSERT/SELECT/DELETE가 books 스키마에 맞게 동작하는가?
- 빈 SQL, 잘못된 SQL, 긴 SQL을 거절하는가?

## Phase 6. B+Tree

볼 파일:

- `src/bptree.c`
- `include/bptree.h`
- `tests/test_bptree.c`
- `tests/test_btree.c`

질문:

- id -> row offset 매핑에 기존 B+Tree를 쓰는가?
- 중복 key가 거절되는가?
- 없는 key 검색이 안전한가?

## Phase 7. Lock

볼 파일:

- `src/db_api.c`
- `docs/decision.md`

질문:

- SELECT는 lock 없이 바로 읽는가?
- CREATE/INSERT/DELETE는 write lock인가?
- write 중 B+Tree와 row 저장소가 동시에 바뀌어도 안전한가?

## Phase 8. Benchmark

볼 파일:

- `bench/bench.js`
- `docs/bench.md`

질문:

- worker 수별로 서버를 재시작하는가?
- read/write/mixed를 모두 측정하는가?
- `bench/result.json`에 total, wait, work, qps가 저장되는가?

## Phase 9. Chart

볼 파일:

- `bench/chart.html`

질문:

- `result.json`을 읽는가?
- total time, avg wait, avg work, QPS를 각각 그리는가?
- 외부 라이브러리 없이 Canvas만 쓰는가?
