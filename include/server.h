#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>

#include "db_api.h"
#include "thread_pool.h"

/* mini_db_server 실행 설정을 담는 구조체다. */
typedef struct {
    const char *db_dir;      /* DB 디렉터리 경로다. */
    int port;                /* listen port다. */
    size_t thread_count;     /* worker thread 수다. */
    size_t queue_capacity;   /* task queue 용량이다. */
} ServerConfig;

/* listen socket, thread pool, db api를 묶어 관리하는 서버 구조체다. */
typedef struct {
    ServerConfig config; /* 서버 시작 시 확정된 설정값이다. */
    int listen_fd;       /* listen socket fd다. */
    ThreadPool pool;     /* worker thread pool이다. */
    DbApi db_api;        /* SQL 엔진 adapter다. */
} Server;

/* 서버 실행에 필요한 리소스를 초기화하고 성공 시 0을 반환한다. */
int server_init(Server *server, const ServerConfig *config,
                char *errbuf, size_t errbuf_size);
/* accept loop를 돌며 worker queue에 client socket을 전달한다. */
int server_run(Server *server, char *errbuf, size_t errbuf_size);
/* 서버가 소유한 listen socket, thread pool, db api를 정리한다. */
void server_destroy(Server *server);

#endif
