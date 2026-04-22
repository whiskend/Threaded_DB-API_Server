#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "json_writer.h"

/* errbuf에 최소 HTTP 파서 오류 메시지를 한 줄로 기록한다. */
static void set_error(char *errbuf, size_t errbuf_size, const char *message)
{
    if (errbuf != NULL && errbuf_size > 0U && message != NULL) {
        snprintf(errbuf, errbuf_size, "%s", message);
    }
}

/* len 바이트 안에서 "\r\n\r\n"가 끝나는 첫 위치를 찾아 index를 반환한다. */
static ssize_t find_header_end(const char *buffer, size_t len)
{
    size_t i;

    if (buffer == NULL || len < 4U) {
        return -1;
    }

    for (i = 0U; i + 3U < len; ++i) {
        if (buffer[i] == '\r' && buffer[i + 1U] == '\n' &&
            buffer[i + 2U] == '\r' && buffer[i + 3U] == '\n') {
            return (ssize_t)i;
        }
    }

    return -1;
}

/* line에서 request line 3요소를 추출해 method/path를 복사한다. */
static int parse_request_line(char *line, HttpRequest *out_req,
                              char *errbuf, size_t errbuf_size)
{
    char *first_space;
    char *second_space;
    size_t method_len;
    size_t path_len;
    const char *version;

    if (line == NULL || out_req == NULL) {
        set_error(errbuf, errbuf_size, "malformed request line");
        return -1;
    }

    first_space = strchr(line, ' ');
    if (first_space == NULL) {
        set_error(errbuf, errbuf_size, "malformed request line");
        return -1;
    }

    second_space = strchr(first_space + 1, ' ');
    if (second_space == NULL) {
        set_error(errbuf, errbuf_size, "malformed request line");
        return -1;
    }

    method_len = (size_t)(first_space - line);
    path_len = (size_t)(second_space - (first_space + 1));
    version = second_space + 1;

    if (method_len == 0U || method_len >= sizeof(out_req->method) ||
        path_len == 0U || path_len >= sizeof(out_req->path) ||
        version[0] == '\0' || strncmp(version, "HTTP/", 5U) != 0) {
        set_error(errbuf, errbuf_size, "malformed request line");
        return -1;
    }

    memcpy(out_req->method, line, method_len);
    out_req->method[method_len] = '\0';
    memcpy(out_req->path, first_space + 1, path_len);
    out_req->path[path_len] = '\0';
    return 0;
}

/* Content-Length 헤더를 찾아 decimal body 길이를 파싱한다. */
static int parse_content_length(char *headers, size_t *out_length,
                                char *errbuf, size_t errbuf_size)
{
    char *line;
    int found = 0;

    if (out_length == NULL) {
        set_error(errbuf, errbuf_size, "invalid Content-Length state");
        return -1;
    }

    *out_length = 0U;
    line = headers;
    while (line != NULL && *line != '\0') {
        char *line_end = strstr(line, "\r\n");
        char *value;
        char *endptr = NULL;
        unsigned long long parsed;

        if (line_end == NULL) {
            set_error(errbuf, errbuf_size, "malformed request headers");
            return -1;
        }

        *line_end = '\0';
        if (*line == '\0') {
            break;
        }

        if (strncasecmp(line, "Content-Length:", 15U) == 0) {
            if (found) {
                set_error(errbuf, errbuf_size, "duplicate Content-Length");
                return -1;
            }

            value = line + 15;
            while (*value == ' ' || *value == '\t') {
                ++value;
            }
            if (*value == '\0') {
                set_error(errbuf, errbuf_size, "missing Content-Length");
                return -1;
            }

            parsed = strtoull(value, &endptr, 10);
            if (endptr == NULL || *endptr != '\0') {
                set_error(errbuf, errbuf_size, "invalid Content-Length");
                return -1;
            }
            *out_length = (size_t)parsed;
            found = 1;
        }

        *line_end = '\r';
        line = line_end + 2;
    }

    return found ? 0 : 1;
}

/* send()가 partial write를 돌려줘도 끝까지 보내는 공용 헬퍼다. */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0U;

    while (sent < len) {
        ssize_t written = send(fd, buf + sent, len - sent, 0);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }

        sent += (size_t)written;
    }

    return 0;
}

/* 소켓에서 request 1개를 읽고 method/path/body를 최소 구조체에 담는다. */
HttpReadStatus http_read_request(int fd, HttpRequest *out_req,
                                 char *errbuf, size_t errbuf_size)
{
    char buffer[HTTP_MAX_HEADER_SIZE + HTTP_MAX_BODY_SIZE + 1];
    size_t total_read = 0U;
    ssize_t header_end_index = -1;
    size_t body_length = 0U;
    size_t header_length;
    char *header_line_end;
    char *body_storage = NULL;
    size_t initial_body_bytes;
    size_t copied_bytes;

    if (out_req == NULL) {
        set_error(errbuf, errbuf_size, "invalid http request output buffer");
        return HTTP_READ_INTERNAL_ERROR;
    }

    memset(out_req, 0, sizeof(*out_req));
    memset(buffer, 0, sizeof(buffer));

    while (header_end_index < 0) {
        ssize_t received;

        if (total_read >= HTTP_MAX_HEADER_SIZE) {
            set_error(errbuf, errbuf_size, "request header too large");
            return HTTP_READ_BAD_REQUEST;
        }

        received = recv(fd, buffer + total_read, sizeof(buffer) - 1U - total_read, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(errbuf, errbuf_size, "failed to read request");
            return HTTP_READ_BAD_REQUEST;
        }
        if (received == 0) {
            set_error(errbuf, errbuf_size, "unexpected EOF while reading request");
            return HTTP_READ_BAD_REQUEST;
        }

        total_read += (size_t)received;
        buffer[total_read] = '\0';
        header_end_index = find_header_end(buffer, total_read);
    }

    header_length = (size_t)header_end_index + 4U;
    if (header_length > HTTP_MAX_HEADER_SIZE) {
        set_error(errbuf, errbuf_size, "request header too large");
        return HTTP_READ_BAD_REQUEST;
    }

    header_line_end = strstr(buffer, "\r\n");
    if (header_line_end == NULL) {
        set_error(errbuf, errbuf_size, "malformed request line");
        return HTTP_READ_BAD_REQUEST;
    }
    *header_line_end = '\0';
    if (parse_request_line(buffer, out_req, errbuf, errbuf_size) != 0) {
        return HTTP_READ_BAD_REQUEST;
    }
    *header_line_end = '\r';

    if (strcmp(out_req->method, "POST") == 0) {
        int content_length_status = parse_content_length(header_line_end + 2, &body_length,
                                                         errbuf, errbuf_size);

        if (content_length_status < 0) {
            return HTTP_READ_BAD_REQUEST;
        }
        if (content_length_status > 0) {
            set_error(errbuf, errbuf_size, "missing Content-Length");
            return HTTP_READ_BAD_REQUEST;
        }
        if (body_length > HTTP_MAX_BODY_SIZE) {
            set_error(errbuf, errbuf_size, "request body too large");
            return HTTP_READ_PAYLOAD_TOO_LARGE;
        }
    }

    body_storage = (char *)malloc(body_length + 1U);
    if (body_storage == NULL) {
        set_error(errbuf, errbuf_size, "out of memory");
        return HTTP_READ_INTERNAL_ERROR;
    }
    body_storage[0] = '\0';

    initial_body_bytes = total_read - header_length;
    copied_bytes = initial_body_bytes < body_length ? initial_body_bytes : body_length;
    if (copied_bytes > 0U) {
        memcpy(body_storage, buffer + header_length, copied_bytes);
    }

    while (copied_bytes < body_length) {
        ssize_t received = recv(fd, body_storage + copied_bytes, body_length - copied_bytes, 0);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(body_storage);
            set_error(errbuf, errbuf_size, "failed to read request body");
            return HTTP_READ_BAD_REQUEST;
        }
        if (received == 0) {
            free(body_storage);
            set_error(errbuf, errbuf_size, "unexpected EOF while reading request body");
            return HTTP_READ_BAD_REQUEST;
        }

        copied_bytes += (size_t)received;
    }

    body_storage[body_length] = '\0';
    out_req->body = body_storage;
    out_req->body_len = body_length;
    return HTTP_READ_OK;
}

/* HttpRequest가 보유한 body 메모리를 해제한다. */
void http_request_free(HttpRequest *req)
{
    if (req == NULL) {
        return;
    }

    free(req->body);
    memset(req, 0, sizeof(*req));
}

/* status code를 reason phrase와 매핑한다. */
const char *http_status_text(int status_code)
{
    switch (status_code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "Internal Server Error";
    }
}

/* JSON body를 HTTP/1.1 response 헤더로 감싸 한 번에 전송한다. */
int http_send_json_response(int fd, int status_code, const char *json_body)
{
    char header[256];
    size_t body_len;
    int header_len;

    if (json_body == NULL) {
        json_body = "";
    }

    body_len = strlen(json_body);
    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          status_code, http_status_text(status_code), body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return -1;
    }

    if (write_all(fd, header, (size_t)header_len) != 0) {
        return -1;
    }

    if (body_len > 0U && write_all(fd, json_body, body_len) != 0) {
        return -1;
    }

    return 0;
}

/* 표준 에러 JSON 객체를 만들어 HTTP response로 전송한다. */
int http_send_error_response(int fd, int status_code,
                             const char *error_code, const char *message)
{
    JsonBuffer buf;
    char *json_body;
    int result;

    if (json_buffer_init(&buf) != 0) {
        return -1;
    }

    if (json_append(&buf, "{\"success\":false,\"error_code\":") != 0 ||
        json_append_escaped_string(&buf, error_code == NULL ? "INTERNAL_ERROR" : error_code) != 0 ||
        json_append(&buf, ",\"message\":") != 0 ||
        json_append_escaped_string(&buf, message == NULL ? "" : message) != 0 ||
        json_append_char(&buf, '}') != 0) {
        json_buffer_free(&buf);
        return -1;
    }

    json_body = json_buffer_take(&buf);
    json_buffer_free(&buf);
    if (json_body == NULL) {
        return -1;
    }

    result = http_send_json_response(fd, status_code, json_body);
    free(json_body);
    return result;
}
