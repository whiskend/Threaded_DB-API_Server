#include "utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* size 바이트 메모리를 할당하고 실패 시 즉시 stderr 출력 후 프로세스를 종료한다. */
void *xmalloc(size_t size) {
    void *ptr = malloc(size == 0 ? 1 : size);
    if (ptr == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

/* src 문자열을 새 heap 버퍼에 복제해 반환하고, src가 NULL이면 NULL을 반환한다. */
char *strdup_safe(const char *src) {
    size_t len;
    char *copy;

    if (src == NULL) {
        return NULL;
    }

    len = strlen(src);
    copy = (char *)xmalloc(len + 1);
    memcpy(copy, src, len + 1);
    return copy;
}

/* text 앞뒤 공백을 제자리에서 제거하고 실제 문자열 시작 위치 포인터를 반환한다. */
char *trim_whitespace(char *text) {
    char *end;

    if (text == NULL) {
        return NULL;
    }

    while (*text != '\0' && isspace((unsigned char)*text)) {
        ++text;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        --end;
    }

    return text;
}

/* path 파일 전체를 읽어 NUL 종료 문자열로 반환하고, 실패 시 NULL을 반환한다. */
char *read_text_file(const char *path) {
    FILE *file;
    long size;
    size_t bytes_read;
    char *buffer;

    if (path == NULL) {
        return NULL;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = (char *)malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    bytes_read = fread(buffer, 1, (size_t)size, file);
    fclose(file);

    if (bytes_read != (size_t)size) {
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';
    return buffer;
}
