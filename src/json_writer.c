#include "json_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 최소 required_len까지 들어갈 수 있도록 버퍼 크기를 지수적으로 늘린다. */
static int json_ensure_capacity(JsonBuffer *buf, size_t required_len)
{
    size_t new_cap;
    char *grown;

    if (buf == NULL) {
        return -1;
    }

    if (required_len + 1U <= buf->cap) {
        return 0;
    }

    new_cap = buf->cap == 0U ? 256U : buf->cap;
    while (required_len + 1U > new_cap) {
        new_cap *= 2U;
    }

    grown = (char *)realloc(buf->data, new_cap);
    if (grown == NULL) {
        return -1;
    }

    buf->data = grown;
    buf->cap = new_cap;
    return 0;
}

/* JSON 버퍼를 빈 문자열 상태로 초기화한다. */
int json_buffer_init(JsonBuffer *buf)
{
    if (buf == NULL) {
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    buf->data = (char *)malloc(256U);
    if (buf->data == NULL) {
        return -1;
    }

    buf->cap = 256U;
    buf->len = 0U;
    buf->data[0] = '\0';
    return 0;
}

/* JSON 버퍼 메모리를 해제하고 구조체를 빈 상태로 돌린다. */
void json_buffer_free(JsonBuffer *buf)
{
    if (buf == NULL) {
        return;
    }

    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

/* 일반 문자열 text를 그대로 append한다. */
int json_append(JsonBuffer *buf, const char *text)
{
    size_t text_len;

    if (buf == NULL || text == NULL) {
        return -1;
    }

    text_len = strlen(text);
    if (json_ensure_capacity(buf, buf->len + text_len) != 0) {
        return -1;
    }

    memcpy(buf->data + buf->len, text, text_len + 1U);
    buf->len += text_len;
    return 0;
}

/* 문자 하나를 append한다. */
int json_append_char(JsonBuffer *buf, char ch)
{
    if (buf == NULL) {
        return -1;
    }

    if (json_ensure_capacity(buf, buf->len + 1U) != 0) {
        return -1;
    }

    buf->data[buf->len++] = ch;
    buf->data[buf->len] = '\0';
    return 0;
}

/* JSON 문자열 규칙에 맞게 escape 처리한 quoted string을 append한다. */
int json_append_escaped_string(JsonBuffer *buf, const char *text)
{
    size_t i;

    if (json_append_char(buf, '"') != 0) {
        return -1;
    }

    if (text == NULL) {
        text = "";
    }

    for (i = 0U; text[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)text[i];
        char control_escape[7];

        switch (ch) {
            case '"':
                if (json_append(buf, "\\\"") != 0) {
                    return -1;
                }
                break;
            case '\\':
                if (json_append(buf, "\\\\") != 0) {
                    return -1;
                }
                break;
            case '\n':
                if (json_append(buf, "\\n") != 0) {
                    return -1;
                }
                break;
            case '\r':
                if (json_append(buf, "\\r") != 0) {
                    return -1;
                }
                break;
            case '\t':
                if (json_append(buf, "\\t") != 0) {
                    return -1;
                }
                break;
            default:
                if (ch < 0x20U) {
                    snprintf(control_escape, sizeof(control_escape), "\\u%04x", (unsigned int)ch);
                    if (json_append(buf, control_escape) != 0) {
                        return -1;
                    }
                } else if (json_append_char(buf, (char)ch) != 0) {
                    return -1;
                }
                break;
        }
    }

    return json_append_char(buf, '"');
}

/* 내부 문자열 버퍼의 소유권을 caller에게 넘기고 buf를 비운다. */
char *json_buffer_take(JsonBuffer *buf)
{
    char *taken;

    if (buf == NULL) {
        return NULL;
    }

    taken = buf->data;
    buf->data = NULL;
    buf->len = 0U;
    buf->cap = 0U;
    return taken;
}
