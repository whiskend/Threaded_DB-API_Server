#include "server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "http.h"
#include "json_parser.h"

/* errbuf에 사람이 읽을 수 있는 서버 오류 메시지를 한 줄로 기록한다. */
static void set_error(char *errbuf, size_t errbuf_size, const char *message)
{
    if (errbuf != NULL && errbuf_size > 0U && message != NULL) {
        snprintf(errbuf, errbuf_size, "%s", message);
    }
}

/* errno를 포함한 시스템 호출 실패 메시지를 errbuf에 기록한다. */
static void set_errno_error(char *errbuf, size_t errbuf_size, const char *message)
{
    if (errbuf != NULL && errbuf_size > 0U && message != NULL) {
        snprintf(errbuf, errbuf_size, "%s: %s", message, strerror(errno));
    }
}

/* worker가 큐에서 client fd를 꺼냈을 때 실제 HTTP 처리 전체를 수행한다. */
static void server_handle_client_task(Task task, void *user_data)
{
    Server *server = (Server *)user_data;
    HttpRequest req = {0};
    char errbuf[256] = {0};
    char *sql = NULL;
    char *json = NULL;
    int http_status = 500;

    if (server == NULL) {
        close(task.client_fd);
        return;
    }

    switch (http_read_request(task.client_fd, &req, errbuf, sizeof(errbuf))) {
        case HTTP_READ_OK:
            break;
        case HTTP_READ_PAYLOAD_TOO_LARGE:
            http_send_error_response(task.client_fd, 413, "PAYLOAD_TOO_LARGE",
                                     errbuf[0] != '\0' ? errbuf : "request body too large");
            close(task.client_fd);
            return;
        case HTTP_READ_INTERNAL_ERROR:
            http_send_error_response(task.client_fd, 500, "INTERNAL_ERROR",
                                     errbuf[0] != '\0' ? errbuf : "internal server error");
            close(task.client_fd);
            return;
        case HTTP_READ_BAD_REQUEST:
        default:
            http_send_error_response(task.client_fd, 400, "BAD_REQUEST",
                                     errbuf[0] != '\0' ? errbuf : "bad request");
            close(task.client_fd);
            return;
    }

    if (strcmp(req.path, "/health") == 0) {
        if (strcmp(req.method, "GET") != 0) {
            http_send_error_response(task.client_fd, 405, "METHOD_NOT_ALLOWED",
                                     "only GET is allowed for /health");
        } else {
            http_send_json_response(task.client_fd, 200,
                                    "{\"success\":true,\"service\":\"mini_db_server\"}");
        }
        goto cleanup;
    }

    if (strcmp(req.path, "/query") == 0) {
        JsonFieldStatus json_status;

        if (strcmp(req.method, "POST") != 0) {
            http_send_error_response(task.client_fd, 405, "METHOD_NOT_ALLOWED",
                                     "only POST is allowed for /query");
            goto cleanup;
        }

        json_status = json_extract_string_field(req.body, "sql", &sql, errbuf, sizeof(errbuf));
        if (json_status == JSON_FIELD_INVALID_JSON) {
            http_send_error_response(task.client_fd, 400, "INVALID_JSON",
                                     errbuf[0] != '\0' ? errbuf : "invalid json");
            goto cleanup;
        }
        if (json_status == JSON_FIELD_MISSING_FIELD) {
            http_send_error_response(task.client_fd, 400, "MISSING_SQL_FIELD",
                                     errbuf[0] != '\0' ? errbuf : "missing sql field");
            goto cleanup;
        }
        if (json_status == JSON_FIELD_INTERNAL_ERROR) {
            http_send_error_response(task.client_fd, 500, "INTERNAL_ERROR",
                                     errbuf[0] != '\0' ? errbuf : "internal server error");
            goto cleanup;
        }

        db_api_execute_sql(&server->db_api, sql, &json, &http_status);
        if (json == NULL) {
            http_send_error_response(task.client_fd, 500, "INTERNAL_ERROR",
                                     "failed to build sql response");
            goto cleanup;
        }

        http_send_json_response(task.client_fd, http_status, json);
        goto cleanup;
    }

    http_send_error_response(task.client_fd, 404, "NOT_FOUND", "route not found");

cleanup:
    free(sql);
    free(json);
    http_request_free(&req);
    close(task.client_fd);
}

/* server 구조체와 내부 리소스를 초기화하고 listen socket까지 준비한다. */
int server_init(Server *server, const ServerConfig *config,
                char *errbuf, size_t errbuf_size)
{
    int opt_value = 1;
    struct sockaddr_in address;

    if (server == NULL || config == NULL || config->db_dir == NULL ||
        config->thread_count == 0U || config->queue_capacity == 0U ||
        config->port <= 0 || config->port > 65535) {
        set_error(errbuf, errbuf_size, "invalid server configuration");
        return -1;
    }

    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    server->config = *config;

    signal(SIGPIPE, SIG_IGN);

    if (db_api_init(&server->db_api, config->db_dir) != 0) {
        set_error(errbuf, errbuf_size, "failed to initialize database adapter");
        return -1;
    }

    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        set_errno_error(errbuf, errbuf_size, "failed to create listen socket");
        server_destroy(server);
        return -1;
    }

    if (setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt_value, sizeof(opt_value)) != 0) {
        set_errno_error(errbuf, errbuf_size, "failed to set SO_REUSEADDR");
        server_destroy(server);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)config->port);

    if (bind(server->listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        set_errno_error(errbuf, errbuf_size, "failed to bind listen socket");
        server_destroy(server);
        return -1;
    }

    if (listen(server->listen_fd, SOMAXCONN) != 0) {
        set_errno_error(errbuf, errbuf_size, "failed to listen on socket");
        server_destroy(server);
        return -1;
    }

    if (thread_pool_init(&server->pool, config->thread_count, config->queue_capacity,
                         server_handle_client_task, server) != 0) {
        set_error(errbuf, errbuf_size, "failed to initialize thread pool");
        server_destroy(server);
        return -1;
    }

    return 0;
}

/* accept loop를 돌며 client socket을 bounded queue에 non-blocking으로 넣는다. */
int server_run(Server *server, char *errbuf, size_t errbuf_size)
{
    if (server == NULL || server->listen_fd < 0) {
        set_error(errbuf, errbuf_size, "server is not initialized");
        return -1;
    }

    for (;;) {
        int client_fd = accept(server->listen_fd, NULL, NULL);
        Task task;
        int submit_result;

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }

            set_errno_error(errbuf, errbuf_size, "accept failed");
            return -1;
        }

        task.client_fd = client_fd;
        submit_result = thread_pool_try_submit(&server->pool, task);
        if (submit_result == 0) {
            continue;
        }

        if (submit_result == 1) {
            shutdown(client_fd, SHUT_RD);
            http_send_error_response(client_fd, 503, "QUEUE_FULL",
                                     "server task queue is full");
            close(client_fd);
            continue;
        }

        http_send_error_response(client_fd, 500, "INTERNAL_ERROR",
                                 "failed to submit request to worker queue");
        close(client_fd);
        set_error(errbuf, errbuf_size, "failed to submit request to worker queue");
        return -1;
    }
}

/* 서버가 소유한 socket, thread pool, db api를 역순으로 정리한다. */
void server_destroy(Server *server)
{
    if (server == NULL) {
        return;
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    thread_pool_destroy(&server->pool);
    db_api_destroy(&server->db_api);
}
