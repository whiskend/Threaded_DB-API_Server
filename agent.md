현재 단계:
완료

완료한 것:
- 작업 폴더 확인: /Users/gang-yeong-im/JUNGLE/Threaded_DB-API_Server
- 현재 브랜치 확인: main...origin/main
- 기존 폴더 확인: src, include, db, sql_processor, tests, docs
- agent.md 작성 시작
- origin main 최신화 완료
- seonho 브랜치 생성 완료
- 기존 `make` 실행 성공
- 기존 SQL 처리기: lexer/parser/executor/runtime 기반 파일 DB 확인
- 기존 B+Tree: include/bptree.h, src/bptree.c 확인
- `db_exec()` 서버용 wrapper 구현
- books 전용 CREATE/INSERT/SELECT/DELETE SQL 처리 구현
- 기존 B+Tree를 id 인덱스로 재사용
- pthread rwlock 기반 read/write lock 적용
- 고정 크기 ring buffer Queue 구현
- Thread Pool과 worker job 처리 구현
- POSIX socket 기반 HTTP/JSON API 서버 구현
- `/health`, `/stats`, `/sql`, `/bench`, `/chart` 구현
- Node.js 기본 모듈 기반 worker별 benchmark 구현
- Canvas chart 구현
- Queue/SQL/BTree 단위 테스트와 API 테스트 추가
- Makefile에 make/run/test/api-test/bench/clean 추가
- README와 docs 문서 작성
- GitHub Actions CI 작성
- commit 생성: 855e71fa6b5733a1b912b4da07e3fcb2a83c5d80
- origin seonho push 성공
- docs/issue.md, docs/pr.md 생성

남은 것:
- 없음

의사결정:
- 질문 없이 과제 목표에 맞게 구현한다.
- 기존 코드가 있으면 삭제하지 않고 wrapper 방식으로 재사용한다.
- 기존 SQL 처리기는 INSERT/SELECT 중심 파일 DB라서 서버 과제의 CREATE/DELETE/books 전용 요구에는 직접 맞지 않는다.
- 서버용 `db_exec()`는 과제 SQL 부분집합을 처리하고, 인덱스는 기존 B+Tree 구현을 직접 재사용한다.
- 기존 CLI `sql_processor`는 유지하고, 새 서버 바이너리 `server`를 별도 타깃으로 만든다.
- DELETE는 기존 B+Tree에 delete 함수가 없어서 row를 tombstone 처리한 뒤 살아있는 row만으로 인덱스를 재빌드한다.
- SELECT는 lock 없이 바로 읽고, CREATE/INSERT/DELETE는 `pthread_rwlock_wrlock`을 사용한다.
- Ubuntu CI에서 POSIX 타입이 숨지 않도록 `_XOPEN_SOURCE=700`, `_DEFAULT_SOURCE`를 CFLAGS에 둔다.

문제:
- 기존 `src/main.c`가 CLI main이라 서버 main을 같은 이름으로 둘 수 없다.
- 기본 sandbox에서는 로컬 포트 bind가 막혀 API 테스트가 실패했다.
- API 테스트가 `wait`만 호출해 서버 프로세스까지 기다리는 문제가 있었다.
- gh CLI가 설치되어 있지 않았다.
- GitHub 앱으로 Issue/PR 생성을 시도했지만 403 Resource not accessible by integration 오류가 났다.

해결:
- `src/server_main.c`를 추가하고 Makefile에서 sql_processor/server 빌드 구성을 분리한다.
- 승인된 escalated 실행으로 로컬 bind 테스트를 수행했다.
- API 테스트에서 curl PID만 모아 기다리도록 수정했다.
- macOS에서 `_SC_NPROCESSORS_ONLN`이 feature macro 조합에 따라 숨을 수 있어, 상수가 없으면 기본 worker를 2로 둔다.
- Issue/PR 본문은 docs/issue.md, docs/pr.md에 저장했다.
- PR 생성 URL은 push 결과로 확인했다: https://github.com/whiskend/Threaded_DB-API_Server/pull/new/seonho
- 사용자 피드백에 따라 SELECT read lock을 제거했다.

테스트 결과:
- 기존 make: 성공
- make: 성공 (`make: Nothing to be done for all` 이후 clean rebuild도 성공)
- make test: 성공
- make api-test: 성공

벤치마크 결과:
- make bench COUNT=30 BENCH_WORKERS=1,2 CONC=4 성공
- bench/result.json 생성 확인
