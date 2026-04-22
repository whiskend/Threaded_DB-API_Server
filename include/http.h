#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_MAX_HEADER_SIZE 8192
#define HTTP_MAX_BODY_SIZE 8192
#define HTTP_MAX_METHOD_SIZE 8
#define HTTP_MAX_PATH_SIZE 256

/* 최소 HTTP 요청에서 worker가 실제로 필요한 정보만 담는 구조체다. */
typedef struct {
    char method[HTTP_MAX_METHOD_SIZE]; /* "GET", "POST" 같은 method 문자열이다. */
    char path[HTTP_MAX_PATH_SIZE];     /* "/health", "/query" 같은 path 문자열이다. */
    char *body;                        /* heap에 할당된 body 문자열이다. */
    size_t body_len;                   /* body 길이다. */
} HttpRequest;

/* 요청 읽기 실패 원인을 400/413/500으로 구분하기 위한 상태 enum이다. */
typedef enum {
    HTTP_READ_OK = 0,          /* 요청 전체를 정상 파싱했다. */
    HTTP_READ_BAD_REQUEST,     /* 잘못된 request line/header/body였다. */
    HTTP_READ_PAYLOAD_TOO_LARGE, /* body 제한을 초과했다. */
    HTTP_READ_INTERNAL_ERROR   /* 메모리 부족 같은 내부 오류가 발생했다. */
} HttpReadStatus;

/* 소켓에서 요청 1개를 읽어 최소 HTTP request 구조체로 파싱한다. */
HttpReadStatus http_read_request(int fd, HttpRequest *out_req,
                                 char *errbuf, size_t errbuf_size);
/* HttpRequest가 들고 있는 body 메모리를 정리한다. */
void http_request_free(HttpRequest *req);
/* JSON body를 담은 최소 HTTP response를 전송한다. */
int http_send_json_response(int fd, int status_code, const char *json_body);
/* 표준 에러 JSON 형식으로 HTTP response를 전송한다. */
int http_send_error_response(int fd, int status_code,
                             const char *error_code, const char *message);
/* status code에 대응하는 reason phrase를 반환한다. */
const char *http_status_text(int status_code);

#endif
