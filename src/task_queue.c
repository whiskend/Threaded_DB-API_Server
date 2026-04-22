#include "task_queue.h"

#include <stdlib.h>
#include <string.h>

/* queue 저장소와 동기화 primitive를 초기화해 사용 가능한 상태로 만든다. */
int task_queue_init(TaskQueue *queue, size_t capacity)
{
    if (queue == NULL || capacity == 0U) {
        return -1;
    }

    memset(queue, 0, sizeof(*queue));
    queue->items = (Task *)calloc(capacity, sizeof(Task));
    if (queue->items == NULL) {
        return -1;
    }
    queue->capacity = capacity;

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue->items);
        memset(queue, 0, sizeof(*queue));
        return -1;
    }

    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        memset(queue, 0, sizeof(*queue));
        return -1;
    }

    if (pthread_cond_init(&queue->not_full, NULL) != 0) {
        pthread_cond_destroy(&queue->not_empty);
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        memset(queue, 0, sizeof(*queue));
        return -1;
    }

    return 0;
}

/* queue가 보유한 메모리와 동기화 primitive를 모두 해제한다. */
void task_queue_destroy(TaskQueue *queue)
{
    if (queue == NULL || queue->items == NULL) {
        return;
    }

    pthread_cond_destroy(&queue->not_full);
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

/* queue가 full이면 기다리지 않고 즉시 반환하는 push 구현이다. */
int task_queue_try_push(TaskQueue *queue, Task task)
{
    int result = 0;

    if (queue == NULL || queue->items == NULL) {
        return -1;
    }

    if (pthread_mutex_lock(&queue->mutex) != 0) {
        return -1;
    }

    if (queue->shutdown) {
        result = -1;
    } else if (queue->count == queue->capacity) {
        result = 1;
    } else {
        queue->items[queue->tail] = task;
        queue->tail = (queue->tail + 1U) % queue->capacity;
        queue->count += 1U;
        pthread_cond_signal(&queue->not_empty);
    }

    pthread_mutex_unlock(&queue->mutex);
    return result;
}

/* queue가 full이면 not_full을 기다리는 blocking push 구현이다. */
int task_queue_push(TaskQueue *queue, Task task)
{
    if (queue == NULL || queue->items == NULL) {
        return -1;
    }

    if (pthread_mutex_lock(&queue->mutex) != 0) {
        return -1;
    }

    while (!queue->shutdown && queue->count == queue->capacity) {
        if (pthread_cond_wait(&queue->not_full, &queue->mutex) != 0) {
            pthread_mutex_unlock(&queue->mutex);
            return -1;
        }
    }

    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    queue->items[queue->tail] = task;
    queue->tail = (queue->tail + 1U) % queue->capacity;
    queue->count += 1U;
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
    return 0;
}

/* queue가 빌 때까지 기다렸다가 task 하나를 꺼내고 shutdown 종료를 감지한다. */
int task_queue_pop(TaskQueue *queue, Task *out_task)
{
    if (queue == NULL || queue->items == NULL || out_task == NULL) {
        return -1;
    }

    if (pthread_mutex_lock(&queue->mutex) != 0) {
        return -1;
    }

    while (queue->count == 0U && !queue->shutdown) {
        if (pthread_cond_wait(&queue->not_empty, &queue->mutex) != 0) {
            pthread_mutex_unlock(&queue->mutex);
            return -1;
        }
    }

    if (queue->shutdown && queue->count == 0U) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    *out_task = queue->items[queue->head];
    queue->head = (queue->head + 1U) % queue->capacity;
    queue->count -= 1U;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    return 0;
}

/* shutdown 플래그를 세우고 대기 중인 producer/consumer를 모두 깨운다. */
void task_queue_shutdown(TaskQueue *queue)
{
    if (queue == NULL || queue->items == NULL) {
        return;
    }

    if (pthread_mutex_lock(&queue->mutex) != 0) {
        return;
    }

    queue->shutdown = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}

/* 현재 queue 길이를 mutex로 보호해서 읽어 반환한다. */
size_t task_queue_size(TaskQueue *queue)
{
    size_t count = 0U;

    if (queue == NULL || queue->items == NULL) {
        return 0U;
    }

    if (pthread_mutex_lock(&queue->mutex) != 0) {
        return 0U;
    }

    count = queue->count;
    pthread_mutex_unlock(&queue->mutex);
    return count;
}
