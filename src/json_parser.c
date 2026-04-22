#include "json_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* errbuf에 사람이 읽을 수 있는 한 줄짜리 에러 메시지를 기록한다. */
static void set_error(char *errbuf, size_t errbuf_size, const char *message)
{
    if (errbuf != NULL && errbuf_size > 0U && message != NULL) {
        snprintf(errbuf, errbuf_size, "%s", message);
    }
}

/* JSON 파싱 중 공백을 모두 건너뛴다. */
static void skip_whitespace(const char **cursor)
{
    while (cursor != NULL && *cursor != NULL &&
           (**cursor == ' ' || **cursor == '\t' || **cursor == '\n' || **cursor == '\r')) {
        *cursor += 1;
    }
}

/* 동적 문자열 버퍼 뒤에 문자 하나를 append한다. */
static int append_char(char **buffer, size_t *length, size_t *capacity, char ch)
{
    char *grown;

    if (*buffer == NULL) {
        *capacity = 32U;
        *buffer = (char *)malloc(*capacity);
        if (*buffer == NULL) {
            return -1;
        }
        *length = 0U;
    }

    if (*length + 1U >= *capacity) {
        *capacity *= 2U;
        grown = (char *)realloc(*buffer, *capacity);
        if (grown == NULL) {
            return -1;
        }
        *buffer = grown;
    }

    (*buffer)[(*length)++] = ch;
    return 0;
}

/* JSON quoted string 하나를 decode해 heap 문자열로 반환한다. */
static JsonFieldStatus parse_json_string(const char **cursor, char **out_text,
                                         char *errbuf, size_t errbuf_size)
{
    char *buffer = NULL;
    size_t length = 0U;
    size_t capacity = 0U;

    if (cursor == NULL || *cursor == NULL || out_text == NULL || **cursor != '"') {
        set_error(errbuf, errbuf_size, "invalid json string");
        return JSON_FIELD_INVALID_JSON;
    }

    *out_text = NULL;
    *cursor += 1;

    while (**cursor != '\0') {
        char ch = **cursor;

        if (ch == '"') {
            if (append_char(&buffer, &length, &capacity, '\0') != 0) {
                free(buffer);
                set_error(errbuf, errbuf_size, "out of memory");
                return JSON_FIELD_INTERNAL_ERROR;
            }
            *out_text = buffer;
            *cursor += 1;
            return JSON_FIELD_OK;
        }

        if ((unsigned char)ch < 0x20U) {
            free(buffer);
            set_error(errbuf, errbuf_size, "invalid control character in string");
            return JSON_FIELD_INVALID_JSON;
        }

        if (ch == '\\') {
            char escaped;

            *cursor += 1;
            escaped = **cursor;
            if (escaped == '\0') {
                free(buffer);
                set_error(errbuf, errbuf_size, "unterminated escape sequence");
                return JSON_FIELD_INVALID_JSON;
            }

            switch (escaped) {
                case '"':
                case '\\':
                    ch = escaped;
                    break;
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                default:
                    free(buffer);
                    set_error(errbuf, errbuf_size, "unsupported escape sequence");
                    return JSON_FIELD_INVALID_JSON;
            }
        }

        if (append_char(&buffer, &length, &capacity, ch) != 0) {
            free(buffer);
            set_error(errbuf, errbuf_size, "out of memory");
            return JSON_FIELD_INTERNAL_ERROR;
        }

        *cursor += 1;
    }

    free(buffer);
    set_error(errbuf, errbuf_size, "unterminated string");
    return JSON_FIELD_INVALID_JSON;
}

/* extra field를 건너뛰기 위해 문자열/숫자/불린/null만 읽어 넘긴다. */
static JsonFieldStatus consume_json_value(const char **cursor, char *errbuf, size_t errbuf_size)
{
    const char *start;

    if (cursor == NULL || *cursor == NULL) {
        set_error(errbuf, errbuf_size, "invalid json value");
        return JSON_FIELD_INVALID_JSON;
    }

    if (**cursor == '"') {
        char *discard = NULL;
        JsonFieldStatus status = parse_json_string(cursor, &discard, errbuf, errbuf_size);
        free(discard);
        return status;
    }

    if (**cursor == '{' || **cursor == '[') {
        set_error(errbuf, errbuf_size, "nested json values are not supported");
        return JSON_FIELD_INVALID_JSON;
    }

    if (strncmp(*cursor, "true", 4U) == 0) {
        *cursor += 4;
        return JSON_FIELD_OK;
    }
    if (strncmp(*cursor, "false", 5U) == 0) {
        *cursor += 5;
        return JSON_FIELD_OK;
    }
    if (strncmp(*cursor, "null", 4U) == 0) {
        *cursor += 4;
        return JSON_FIELD_OK;
    }

    start = *cursor;
    if (**cursor == '-') {
        *cursor += 1;
    }
    if (!isdigit((unsigned char)**cursor)) {
        set_error(errbuf, errbuf_size, "invalid json value");
        return JSON_FIELD_INVALID_JSON;
    }
    while (isdigit((unsigned char)**cursor)) {
        *cursor += 1;
    }
    if (**cursor == '.') {
        *cursor += 1;
        if (!isdigit((unsigned char)**cursor)) {
            set_error(errbuf, errbuf_size, "invalid json number");
            return JSON_FIELD_INVALID_JSON;
        }
        while (isdigit((unsigned char)**cursor)) {
            *cursor += 1;
        }
    }
    if (**cursor == 'e' || **cursor == 'E') {
        *cursor += 1;
        if (**cursor == '+' || **cursor == '-') {
            *cursor += 1;
        }
        if (!isdigit((unsigned char)**cursor)) {
            set_error(errbuf, errbuf_size, "invalid json number");
            return JSON_FIELD_INVALID_JSON;
        }
        while (isdigit((unsigned char)**cursor)) {
            *cursor += 1;
        }
    }
    if (*cursor == start) {
        set_error(errbuf, errbuf_size, "invalid json value");
        return JSON_FIELD_INVALID_JSON;
    }

    return JSON_FIELD_OK;
}

/* top-level object에서 지정된 문자열 field를 찾아 valid JSON인지 함께 검증한다. */
JsonFieldStatus json_extract_string_field(const char *json, const char *field_name,
                                          char **out_value,
                                          char *errbuf, size_t errbuf_size)
{
    const char *cursor = json;
    char *found_value = NULL;

    if (out_value != NULL) {
        *out_value = NULL;
    }

    if (json == NULL || field_name == NULL || out_value == NULL) {
        set_error(errbuf, errbuf_size, "invalid json parser arguments");
        return JSON_FIELD_INTERNAL_ERROR;
    }

    skip_whitespace(&cursor);
    if (*cursor != '{') {
        set_error(errbuf, errbuf_size, "top-level json value must be an object");
        return JSON_FIELD_INVALID_JSON;
    }
    cursor += 1;

    skip_whitespace(&cursor);
    if (*cursor == '}') {
        set_error(errbuf, errbuf_size, "missing sql field");
        return JSON_FIELD_MISSING_FIELD;
    }

    for (;;) {
        char *key = NULL;
        JsonFieldStatus status;

        skip_whitespace(&cursor);
        if (*cursor != '"') {
            free(found_value);
            set_error(errbuf, errbuf_size, "expected object key");
            return JSON_FIELD_INVALID_JSON;
        }

        status = parse_json_string(&cursor, &key, errbuf, errbuf_size);
        if (status != JSON_FIELD_OK) {
            free(found_value);
            return status;
        }

        skip_whitespace(&cursor);
        if (*cursor != ':') {
            free(key);
            free(found_value);
            set_error(errbuf, errbuf_size, "expected ':' after object key");
            return JSON_FIELD_INVALID_JSON;
        }
        cursor += 1;
        skip_whitespace(&cursor);

        if (strcmp(key, field_name) == 0) {
            if (*cursor != '"') {
                const char *value_cursor = cursor;

                status = consume_json_value(&value_cursor, errbuf, errbuf_size);
                free(key);
                if (status == JSON_FIELD_OK) {
                    free(found_value);
                    set_error(errbuf, errbuf_size, "sql field must be a string");
                    return JSON_FIELD_MISSING_FIELD;
                }
                free(found_value);
                return status;
            }

            if (found_value == NULL) {
                status = parse_json_string(&cursor, &found_value, errbuf, errbuf_size);
                if (status != JSON_FIELD_OK) {
                    free(key);
                    return status;
                }
            } else {
                char *discard = NULL;
                status = parse_json_string(&cursor, &discard, errbuf, errbuf_size);
                free(discard);
                if (status != JSON_FIELD_OK) {
                    free(key);
                    free(found_value);
                    return status;
                }
            }
        } else {
            status = consume_json_value(&cursor, errbuf, errbuf_size);
            if (status != JSON_FIELD_OK) {
                free(key);
                free(found_value);
                return status;
            }
        }

        free(key);
        skip_whitespace(&cursor);

        if (*cursor == ',') {
            cursor += 1;
            continue;
        }

        if (*cursor == '}') {
            cursor += 1;
            break;
        }

        free(found_value);
        set_error(errbuf, errbuf_size, "expected ',' or '}' after object entry");
        return JSON_FIELD_INVALID_JSON;
    }

    skip_whitespace(&cursor);
    if (*cursor != '\0') {
        free(found_value);
        set_error(errbuf, errbuf_size, "unexpected trailing characters after json object");
        return JSON_FIELD_INVALID_JSON;
    }

    if (found_value == NULL) {
        set_error(errbuf, errbuf_size, "missing sql field");
        return JSON_FIELD_MISSING_FIELD;
    }

    *out_value = found_value;
    return JSON_FIELD_OK;
}
