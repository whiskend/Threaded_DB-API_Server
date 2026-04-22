#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <stddef.h>

/* JSON field 추출 결과를 caller가 정확히 분기할 수 있게 하는 상태 enum이다. */
typedef enum {
    JSON_FIELD_OK = 0,         /* field를 정상적으로 찾아 문자열 값을 추출했다. */
    JSON_FIELD_MISSING_FIELD,  /* field가 없거나 문자열 타입이 아니었다. */
    JSON_FIELD_INVALID_JSON,   /* JSON 문법 자체가 잘못됐다. */
    JSON_FIELD_INTERNAL_ERROR  /* 메모리 부족 같은 내부 오류가 발생했다. */
} JsonFieldStatus;

/* top-level JSON object에서 field_name 문자열 필드를 찾아 heap 문자열로 반환한다. */
JsonFieldStatus json_extract_string_field(const char *json, const char *field_name,
                                          char **out_value,
                                          char *errbuf, size_t errbuf_size);

#endif
