#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include <pthread.h>
#include <stddef.h>

/* worker가 처리할 대상 클라이언트 소켓 하나를 담는 큐다. */
typedef struct {
    int client_fd; /* accept()로 받은 client socket fd다. */
} Task;

/* 동적 capacity를 갖는 bounded ring-buffer queue다. */
typedef struct {
    Task *items;            /* ring-buffer 저장소다. */
    size_t capacity;        /* 최대 저장 가능한 task 수다. */
    size_t head;            /* 다음 pop 위치다. */
    size_t tail;            /* 다음 push 위치다. */
    size_t count;           /* 현재 queue에 쌓인 task 수다. */
    int shutdown;           /* shutdown이 시작됐는지 나타낸다. */
    pthread_mutex_t mutex;  /* queue 상태를 보호하는 mutex다. */
    pthread_cond_t not_empty; /* pop 대기용 condition variable이다. */
    pthread_cond_t not_full;  /* push 대기용 condition variable이다. */
} TaskQueue;

/* queue를 동적 capacity로 초기화하고 성공 시 0을 반환한다. */
int task_queue_init(TaskQueue *queue, size_t capacity);
/* queue의 내부 저장소와 동기화 primitive를 정리한다. */
void task_queue_destroy(TaskQueue *queue);
/* queue가 가득 차면 기다리지 않고 1을 반환하는 non-blocking push다. */
int task_queue_try_push(TaskQueue *queue, Task task);
/* queue가 가득 차면 대기하는 blocking push다. */
int task_queue_push(TaskQueue *queue, Task task);
/* queue에서 task 하나를 꺼내고 shutdown 시 더 이상 없으면 -1을 반환한다. */
int task_queue_pop(TaskQueue *queue, Task *out_task);
/* shutdown 플래그를 세우고 대기 중인 thread를 모두 깨운다. */
void task_queue_shutdown(TaskQueue *queue);
/* 현재 queue에 들어 있는 task 수를 반환한다. */
size_t task_queue_size(TaskQueue *queue);

#endif
