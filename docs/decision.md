# Decisions

## 1. 왜 HTTP/JSON인가?

HTTP도 TCP 위에서 동작한다.
비교 대상은 raw TCP 직접 프로토콜이다.

HTTP/JSON을 선택한 이유는 브라우저, curl, JS에서 바로 테스트할 수 있고
요청/응답 구조가 명확하기 때문이다.

이번 과제의 핵심은 네트워크 프로토콜 발명이 아니라
DBMS API 서버, Thread Pool, 동시성 처리이므로
통신 방식은 단순하고 검증하기 쉬운 HTTP/JSON을 선택했다.

## 2. 왜 Thread Pool인가?

요청마다 thread를 만들면 생성/해제 비용이 계속 발생하고, 요청 폭주 시 thread 수가 무제한으로 늘어날 수 있습니다.

Thread Pool은 worker 수를 고정해 서버 자원을 예측 가능하게 씁니다. 또한 worker 개수를 바꿔 wait time, work time, QPS를 비교하기 쉽습니다.

## 3. 왜 Queue인가?

main thread는 HTTP 요청을 빠르게 받아 큐에 넣고, worker는 큐에서 작업을 꺼내 처리합니다.

Queue가 있으면 요청 접수 속도와 DB 처리 속도를 분리할 수 있고, worker가 모두 바쁠 때도 짧은 대기 구간을 만들 수 있습니다.

## 4. 왜 Queue max 1024인가?

무제한 큐는 요청이 폭주하면 메모리를 터뜨릴 수 있다.
1024는 과제용 서버에서 충분히 크고 관리하기 쉽다.
큐가 가득 차면 요청을 오래 붙잡지 말고 503으로 거절한다.

## 5. 왜 write lock인가?

모든 요청에 mutex 전역락을 걸면 DB가 사실상 싱글 스레드가 됩니다.

이번 구현에서는 SELECT가 DB 구조를 바꾸지 않는다는 과제 정책에 맞춰 SELECT에는 read lock을 걸지 않습니다.

대신 INSERT/DELETE/CREATE 같은 write 요청만 `pthread_rwlock_wrlock()`으로 단독 실행합니다.

## 6. 왜 WRITE는 배타 lock인가?

INSERT/DELETE/CREATE는 B+Tree와 row 저장소를 바꿉니다.

두 write가 동시에 들어오면 배열 크기 변경, row 추가, index split, delete 후 index rebuild가 섞일 수 있습니다. 그래서 write lock으로 단독 실행합니다.

## 7. 왜 READ는 lock 없이 실행하는가?

SELECT는 DB 구조를 바꾸지 않으므로 여러 thread가 동시에 실행해도 된다.
INSERT/DELETE는 B+ Tree와 row 저장소를 바꾸므로 단독 실행해야 한다.
모든 요청에 mutex 전역락을 걸면 DB가 사실상 싱글 스레드가 된다.
SELECT에 lock을 걸지 않으면 읽기 요청끼리는 worker thread에서 바로 병렬 실행된다.

구현에서는 SELECT가 `exec_select()`로 바로 들어가고, CREATE/INSERT/DELETE만 write lock을 잡는다.

## 8. 왜 worker 수를 1,2,4,8,16,32로 비교하는가?

이 서버는 네트워크 I/O와 DB 처리를 함께 한다.
I/O 작업은 기다리는 시간이 있으므로 CPU 코어 수와 같은 thread만 쓰면 자원이 남을 수 있다.
그래서 CPU 코어 수의 2~3배까지 worker를 늘려보고 실제 wait time, work time, QPS를 비교한다.

1, 2, 4, 8, 16, 32는 적은 worker부터 많은 worker까지 변화 추세를 보기 좋은 배수입니다.

## 9. 왜 B+Tree인가?

books 테이블의 핵심 조회는 `WHERE id = ?` 입니다.

B+Tree는 key를 정렬해 저장하고 root에서 leaf까지 내려가므로 대량 row에서도 안정적인 조회 비용을 제공합니다.

기존 레포에 `include/bptree.h`, `src/bptree.c`가 이미 있으므로 새 인덱스를 만들지 않고 이 구현을 재사용했습니다.

## 10. DELETE 구현 결정

기존 B+Tree에는 delete 함수가 없습니다.

그래서 DELETE는 row를 tombstone 처리하고, 살아있는 row만 다시 B+Tree에 넣어 인덱스를 재빌드합니다. 삭제가 아주 많으면 비싸지만 과제 범위에서는 구현이 단순하고 검색 정확성을 유지합니다.
