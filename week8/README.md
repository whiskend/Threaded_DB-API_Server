# Week8 Mini DB Server

기존 `sql_processor` CLI는 유지하고, 같은 SQL 엔진 위에 HTTP/JSON 서버 `mini_db_server`를 추가한 구현입니다.

## Build

```sh
make
```

생성 파일:

- `./sql_processor`
- `./mini_db_server`

서버 타깃은 `src/main.c`를 링크하지 않고, `src/server_main.c`와 서버 모듈만 따로 링크합니다.

## CLI 유지

```sh
./sql_processor -d db -f queries/multi_statements.sql
make test
```

## Server

```sh
./mini_db_server -d db -p 8080 -t 4 -q 64
```

옵션:

- `-d, --db`: DB 디렉터리. 필수
- `-p, --port`: 포트. 기본값 `8080`
- `-t, --threads`: worker 수. 기본값 `4`
- `-q, --queue-size`: bounded queue capacity. 기본값 `64`
- `-h, --help`: 도움말

## API

### GET /health

```json
{"success":true,"service":"mini_db_server"}
```

### POST /query

요청:

```json
{"sql":"SELECT * FROM users WHERE id = 1;"}
```

응답 예시:

```json
{"success":true,"type":"select","used_index":true,"row_count":1,"columns":["id","name","age"],"rows":[["1","kim","25"]]}
```

```json
{"success":true,"type":"insert","affected_rows":1,"generated_id":1}
```

요청당 SQL statement는 1개만 허용하며 batch SQL은 `MULTI_STATEMENT_NOT_ALLOWED`로 거절합니다.

## Concurrency

- `SELECT`: tokenize/parse 후 table runtime을 write lock에서 preload하고, execute는 read lock에서 수행합니다.
- `INSERT`: tokenize/parse 후 execute 전체를 write lock에서 수행합니다.
- accept thread는 1개입니다.
- worker thread와 queue size는 실행 옵션으로 조절합니다.
- queue가 가득 차면 즉시 HTTP `503`과 `QUEUE_FULL`을 반환합니다.

## Tests

```sh
make test
```

포함 검증:

- 기존 lexer/parser/storage/runtime/executor 테스트
- 기존 CLI integration 테스트
- `GET /health`
- `POST /query` INSERT/SELECT
- `used_index: true`
- invalid JSON
- multi statement reject
- concurrent SELECT
- concurrent INSERT
- queue full `503`

## Benchmark

```sh
tests/bench_api_server.sh
```

기본 조합:

- requests per run: `2000`
- repeat runs: `3`
- concurrency: `64`
- workers: `1 2 4 8`
- queue size: `32 64 128`
- workload: `select-only insert-only mixed`

`concurrency`는 벤치마크에서 동시에 몇 개의 요청을 서버에 보내는지 뜻합니다.
동시 요청이 몰릴 때 서버가 실제로 얼마나 버티는지, queue가 차서 `503`이 나는지, worker 수를 늘리는 게 효과 있는지를 보려고 필요합니다.

결과 파일:

```text
build/api_benchmark.csv          # 3회 반복 평균 요약 CSV
build/api_benchmark_runs.csv     # 각 run별 raw CSV
build/api_benchmark_summary.txt  # 한눈 요약 텍스트
build/api_benchmark_report.html  # 한눈 HTML 리포트
```

환경변수로 빠르게 조정할 수 있습니다.

```sh
TOTAL_REQUESTS=2000 REPEAT_RUNS=3 CONCURRENCY=64 tests/bench_api_server.sh
```

추천 조합만 빨리 보려면:

```sh
cat build/api_benchmark_summary.txt
```
