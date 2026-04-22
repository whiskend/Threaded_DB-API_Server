#ifndef JSON_WRITER_H
#define JSON_WRITER_H

#include <stddef.h>

/* JSON 문자열을 점진적으로 조립하기 위한 동적 버퍼다. */
typedef struct {
    char *data;    /* heap에 할당된 실제 문자열 버퍼다. */
    size_t len;    /* 현재 사용 중인 문자열 길이다. */
    size_t cap;    /* 할당된 전체 버퍼 크기다. */
} JsonBuffer;

/* JSON 버퍼를 빈 문자열 상태로 초기화하고 성공 시 0을 반환한다. */
int json_buffer_init(JsonBuffer *buf);
/* JSON 버퍼가 가진 메모리를 해제하고 구조체를 0으로 초기화한다. */
void json_buffer_free(JsonBuffer *buf);
/* 일반 문자열 text를 그대로 append한다. */
int json_append(JsonBuffer *buf, const char *text);
/* 문자 하나를 append한다. */
int json_append_char(JsonBuffer *buf, char ch);
/* JSON escape를 적용한 quoted string을 append한다. */
int json_append_escaped_string(JsonBuffer *buf, const char *text);
/* 내부 버퍼 소유권을 caller에게 넘기고 buf를 비운다. */
char *json_buffer_take(JsonBuffer *buf);

#endif
