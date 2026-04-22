#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>

/* worker main이 queue에서 task를 꺼내 handler로 넘기는 반복 루프다. */
static void *thread_pool_worker_main(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;

    if (pool == NULL || pool->handler == NULL) {
        return NULL;
    }

    for (;;) {
        Task task;

        if (task_queue_pop(&pool->queue, &task) != 0) {
            break;
        }

        pool->handler(task, pool->user_data);
    }

    return NULL;
}

/* worker 배열과 내부 queue를 초기화한 뒤 thread를 미리 생성한다. */
int thread_pool_init(ThreadPool *pool, size_t thread_count, size_t queue_capacity,
                     TaskHandler handler, void *user_data)
{
    size_t i;

    if (pool == NULL || handler == NULL || thread_count == 0U || queue_capacity == 0U) {
        return -1;
    }

    memset(pool, 0, sizeof(*pool));
    if (task_queue_init(&pool->queue, queue_capacity) != 0) {
        return -1;
    }

    pool->threads = (pthread_t *)calloc(thread_count, sizeof(pthread_t));
    if (pool->threads == NULL) {
        task_queue_destroy(&pool->queue);
        memset(pool, 0, sizeof(*pool));
        return -1;
    }

    pool->thread_count = thread_count;
    pool->handler = handler;
    pool->user_data = user_data;

    for (i = 0U; i < thread_count; ++i) {
        if (pthread_create(&pool->threads[i], NULL, thread_pool_worker_main, pool) != 0) {
            size_t joined;

            task_queue_shutdown(&pool->queue);
            for (joined = 0U; joined < pool->created_count; ++joined) {
                pthread_join(pool->threads[joined], NULL);
            }
            free(pool->threads);
            task_queue_destroy(&pool->queue);
            memset(pool, 0, sizeof(*pool));
            return -1;
        }
        pool->created_count += 1U;
    }

    pool->started = 1;
    return 0;
}

/* queue에 task 하나를 non-blocking 방식으로 제출한다. */
int thread_pool_try_submit(ThreadPool *pool, Task task)
{
    if (pool == NULL || !pool->started) {
        return -1;
    }

    return task_queue_try_push(&pool->queue, task);
}

/* worker가 종료될 수 있게 queue shutdown을 건다. */
void thread_pool_shutdown(ThreadPool *pool)
{
    if (pool == NULL || !pool->started) {
        return;
    }

    task_queue_shutdown(&pool->queue);
}

/* worker join과 queue 정리를 포함한 전체 pool 파괴 루틴이다. */
void thread_pool_destroy(ThreadPool *pool)
{
    size_t i;

    if (pool == NULL) {
        return;
    }

    if (pool->queue.items != NULL) {
        task_queue_shutdown(&pool->queue);
    }

    if (pool->threads != NULL) {
        for (i = 0U; i < pool->created_count; ++i) {
            pthread_join(pool->threads[i], NULL);
        }
        free(pool->threads);
    }

    if (pool->queue.items != NULL) {
        task_queue_destroy(&pool->queue);
    }

    memset(pool, 0, sizeof(*pool));
}
