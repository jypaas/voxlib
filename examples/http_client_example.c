/*
 * http_client_example.c - HTTP/HTTPS Client 示例
 *
 * 用法：
 *   http_client_example [url]
 *
 * 默认 URL：
 *   http://127.0.0.1:8080/hello
 */

#include "../vox_socket.h"
#include "../vox_loop.h"
#include "../vox_log.h"

#include "../http/vox_http_client.h"

#include <stdio.h>
#include <string.h>

/* 完成标志 + loop，便于 VOX_RUN_ONCE 循环在收到响应后退出 */
typedef struct {
    vox_loop_t* loop;
    int done;
} example_ctx_t;

static void on_connect(vox_http_client_req_t* req, void* user_data) {
    (void)req;
    (void)user_data;
    VOX_LOG_INFO("[client] connected");
}

static void on_status(vox_http_client_req_t* req, int status_code, int http_major, int http_minor, void* user_data) {
    (void)req;
    (void)user_data;
    VOX_LOG_INFO("[client] status: %d (HTTP/%d.%d)", status_code, http_major, http_minor);
}

static void on_header(vox_http_client_req_t* req, vox_strview_t name, vox_strview_t value, void* user_data) {
    (void)req;
    (void)user_data;
    fprintf(stdout, "%.*s: %.*s\n", (int)name.len, name.ptr, (int)value.len, value.ptr);
}

static void on_headers_complete(vox_http_client_req_t* req, void* user_data) {
    (void)req;
    (void)user_data;
    fprintf(stdout, "\n");
}

static void on_body(vox_http_client_req_t* req, const void* data, size_t len, void* user_data) {
    (void)req;
    (void)user_data;
    if (data && len) {
        fwrite(data, 1, len, stdout);
    }
}

static void on_complete(vox_http_client_req_t* req, int status, void* user_data) {
    (void)req;
    example_ctx_t* ctx = (example_ctx_t*)user_data;
    VOX_LOG_INFO("[client] complete: %d", status);
    ctx->done = 1;
    vox_loop_stop(ctx->loop);
}

static void on_error(vox_http_client_req_t* req, const char* message, void* user_data) {
    (void)req;
    example_ctx_t* ctx = (example_ctx_t*)user_data;
    VOX_LOG_ERROR("[client] error: %s", message ? message : "(null)");
    ctx->done = 1;
    vox_loop_stop(ctx->loop);
}

int main(int argc, char** argv) {
    const char* url = "http://127.0.0.1:8080/hello";
    if (argc >= 2 && argv[1] && argv[1][0]) {
        url = argv[1];
    }

    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }

    vox_log_set_level(VOX_LOG_INFO);

    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "vox_loop_create failed\n");
        return 1;
    }

    vox_http_client_t* client = vox_http_client_create(loop);
    if (!client) {
        fprintf(stderr, "vox_http_client_create failed\n");
        return 1;
    }

    vox_http_client_request_t req = {0};
    req.method = VOX_HTTP_METHOD_GET;
    req.url = url;
    req.headers = NULL;
    req.header_count = 0;
    req.body = NULL;
    req.body_len = 0;
    req.ssl_ctx = NULL;
    req.connection_timeout_ms = 3000;

    vox_http_client_callbacks_t cbs = {0};
    cbs.on_connect = on_connect;
    cbs.on_status = on_status;
    cbs.on_header = on_header;
    cbs.on_headers_complete = on_headers_complete;
    cbs.on_body = on_body;
    cbs.on_complete = on_complete;
    cbs.on_error = on_error;

    example_ctx_t ctx = { .loop = loop, .done = 0 };
    if (vox_http_client_request(client, &req, &cbs, &ctx, NULL) != 0) {
        fprintf(stderr, "vox_http_client_request failed\n");
        return 1;
    }

    /* VOX_RUN_ONCE 每轮后返回，由 ctx.done 决定是否退出，避免 VOX_RUN_DEFAULT 下不退出 */
    while (!ctx.done) {
        vox_loop_run(loop, VOX_RUN_ONCE);
    }
    return 0;
}

