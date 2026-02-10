/*
 * http_server_example.c - 基本 HTTP Server 示例
 * - GET /hello
 * - GET /api/user/:id （展示 group + :param + middleware）
 */

#include "../vox_loop.h"
#include "../vox_socket.h"
#include "../vox_log.h"

#include "../http/vox_http_engine.h"
#include "../http/vox_http_server.h"
#include "../http/vox_http_context.h"

#include <stdio.h>
#include <string.h>

static void mw_logger(vox_http_context_t* ctx) {
    const vox_http_request_t* req = vox_http_context_request(ctx);
    if (req && req->path.ptr) {
        VOX_LOG_INFO("[http] %.*s", (int)req->path.len, req->path.ptr);
    }
    vox_http_context_next(ctx);
}

static void hello_handler(vox_http_context_t* ctx) {
    vox_http_context_status(ctx, 200);
    vox_http_context_write_cstr(ctx, "hello");  /* 5 字节，Content-Length: 5 */
}

static void user_handler(vox_http_context_t* ctx) {
    vox_strview_t id = vox_http_context_param(ctx, "id");
    vox_http_context_status(ctx, 200);
    vox_http_context_write_cstr(ctx, "user id=");
    if (id.ptr && id.len > 0) {
        vox_http_context_write(ctx, id.ptr, id.len);
    }
    vox_http_context_write_cstr(ctx, "\n");
}

int main(void) {
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }

    vox_log_set_level(VOX_LOG_INFO);

    /* 配置 backend */
    vox_backend_config_t backend_config = {0};
    backend_config.type = VOX_BACKEND_TYPE_AUTO;
    backend_config.mpool = NULL;  /* 使用 loop 内部创建的内存池 */
    backend_config.max_events = 1024 * 100;  /* 使用默认值 */
    
    /* 配置 loop */
    vox_loop_config_t loop_config = {0};
    loop_config.backend_config = &backend_config;

    vox_loop_t* loop = vox_loop_create_with_config(&loop_config);
    if (!loop) {
        fprintf(stderr, "vox_loop_create failed\n");
        return 1;
    }

    vox_http_engine_t* engine = vox_http_engine_create(loop);
    if (!engine) {
        fprintf(stderr, "vox_http_engine_create failed\n");
        return 1;
    }

    /* 全局中间件 */
    vox_http_engine_use(engine, mw_logger);

    /* 路由 */
    {
        vox_http_handler_cb hs[] = { hello_handler };
        vox_http_engine_get(engine, "/hello", hs, sizeof(hs) / sizeof(hs[0]));
    }

    /* group + :param */
    vox_http_group_t* api = vox_http_engine_group(engine, "/api");
    if (api) {
        vox_http_group_use(api, mw_logger);
        vox_http_handler_cb hs[] = { user_handler };
        vox_http_group_get(api, "/user/:id", hs, sizeof(hs) / sizeof(hs[0]));
    }

    vox_http_server_t* server = vox_http_server_create(engine);
    if (!server) {
        fprintf(stderr, "vox_http_server_create failed\n");
        return 1;
    }

    vox_socket_addr_t addr;
    if (vox_socket_parse_address("0.0.0.0", 8080, &addr) != 0) {
        fprintf(stderr, "vox_socket_parse_address failed\n");
        return 1;
    }

    if (vox_http_server_listen_tcp(server, &addr, 128) != 0) {
        fprintf(stderr, "listen tcp failed\n");
        return 1;
    }

    VOX_LOG_INFO("HTTP server listening on 0.0.0.0:8080");
    return vox_loop_run(loop, VOX_RUN_DEFAULT);
}

