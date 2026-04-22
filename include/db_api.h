#ifndef DB_API_H
#define DB_API_H

#include <pthread.h>

#include "runtime.h"

/* HTTP 서버와 기존 SQL 엔진 사이의 동시성/JSON adapter다. */
typedef struct {
    ExecutionContext ctx;     /* 공유 runtime cache를 담는 실행 컨텍스트다. */
    pthread_rwlock_t db_rwlock; /* SELECT/INSERT 동기화를 위한 rwlock이다. */
} DbApi;

/* DB API adapter를 초기화하고 성공 시 0을 반환한다. */
int db_api_init(DbApi *api, const char *db_dir);
/* SQL 1문장을 실행하고 heap JSON 응답과 HTTP status를 반환한다. */
int db_api_execute_sql(DbApi *api, const char *sql, char **out_json, int *out_http_status);
/* DB API adapter가 가진 runtime cache와 rwlock을 정리한다. */
void db_api_destroy(DbApi *api);

#endif
