#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <stddef.h>

#include "task_queue.h"

/* worker가 task 하나를 꺼냈을 때 호출하는 callback 타입이다. */
typedef void (*TaskHandler)(Task task, void *user_data);

/* worker thread 집합과 내부 task queue를 함께 관리하는 구조체다. */
typedef struct {
    pthread_t *threads;      /* worker thread handle 배열이다. */
    size_t thread_count;     /* 설정된 worker 수다. */
    size_t created_count;    /* 실제 생성 완료된 worker 수다. */
    TaskQueue queue;         /* worker가 공유하는 bounded queue다. */
    TaskHandler handler;     /* 각 task를 처리할 callback이다. */
    void *user_data;         /* callback에 함께 넘길 context다. */
    int started;             /* 초기화가 성공적으로 끝났는지 나타낸다. */
} ThreadPool;

/* queue와 worker thread를 초기화하고 성공 시 0을 반환한다. */
int thread_pool_init(ThreadPool *pool, size_t thread_count, size_t queue_capacity,
                     TaskHandler handler, void *user_data);
/* queue가 가득 차면 기다리지 않고 1을 반환하는 non-blocking submit이다. */
int thread_pool_try_submit(ThreadPool *pool, Task task);
/* queue shutdown을 걸어 worker들이 종료될 수 있게 만든다. */
void thread_pool_shutdown(ThreadPool *pool);
/* worker join과 queue 정리를 포함한 전체 pool 해제를 수행한다. */
void thread_pool_destroy(ThreadPool *pool);

#endif
