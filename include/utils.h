#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

/* path 파일 전체를 읽어 heap에 올린 NUL 종료 문자열을 반환하고 실패 시 NULL을 반환한다. */
char *read_text_file(const char *path);
/* text의 앞뒤 공백을 제자리에서 제거하고 실제 시작 위치 포인터를 반환한다. */
char *trim_whitespace(char *text);
/* src를 heap에 복제한 새 문자열을 반환한다. */
char *strdup_safe(const char *src);
/* size 바이트를 malloc하고 실패 시 즉시 프로그램을 종료하는 할당 래퍼다. */
void *xmalloc(size_t size);

#endif
