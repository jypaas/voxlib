/*
 * ws_echo_example.c - WebSocket Echo 示例（WS）
 * - GET /ws 触发 websocket upgrade
 * - 收到 text/binary 原样回显
 */

#include "../vox_loop.h"
#include "../vox_socket.h"
#include "../vox_log.h"

#include "../http/vox_http_engine.h"
#include "../http/vox_http_server.h"
#include "../http/vox_http_context.h"
#include "../http/vox_http_ws.h"

#include <stdio.h>

static void ws_on_connect(vox_http_ws_conn_t* ws, void* user_data) {
    (void)user_data;
    const char* msg = "welcome\n";
    vox_http_ws_send_text(ws, msg, 8);
}

static void ws_on_message(vox_http_ws_conn_t* ws, const void* data, size_t len, bool is_text, void* user_data) {
    (void)user_data;
    if (is_text) {
        vox_http_ws_send_text(ws, (const char*)data, len);
    } else {
        vox_http_ws_send_binary(ws, data, len);
    }
}

static void ws_on_close(vox_http_ws_conn_t* ws, int code, const char* reason, void* user_data) {
    (void)ws;
    (void)user_data;
    VOX_LOG_INFO("[ws] closed code=%d reason=%s", code, reason ? reason : "");
}

static void ws_on_error(vox_http_ws_conn_t* ws, const char* message, void* user_data) {
    (void)ws;
    (void)user_data;
    VOX_LOG_ERROR("[ws] error: %s", message ? message : "");
}

static void ws_upgrade_handler(vox_http_context_t* ctx) {
    vox_http_ws_callbacks_t cbs;
    cbs.on_connect = ws_on_connect;
    cbs.on_message = ws_on_message;
    cbs.on_close = ws_on_close;
    cbs.on_error = ws_on_error;
    cbs.user_data = NULL;

    if (vox_http_ws_upgrade(ctx, &cbs) != 0) {
        VOX_LOG_ERROR("[ws] upgrade failed");
        vox_http_context_status(ctx, 400);
        vox_http_context_write_cstr(ctx, "bad websocket upgrade\n");
    }
}

int main(void) {
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }

    vox_log_set_level(VOX_LOG_INFO);

    vox_loop_t* loop = vox_loop_create();
    if (!loop) return 1;

    vox_http_engine_t* engine = vox_http_engine_create(loop);
    if (!engine) return 1;

    vox_http_handler_cb hs[] = { ws_upgrade_handler };
    vox_http_engine_get(engine, "/ws", hs, sizeof(hs) / sizeof(hs[0]));

    vox_http_server_t* server = vox_http_server_create(engine);
    if (!server) return 1;

    vox_socket_addr_t addr;
    if (vox_socket_parse_address("0.0.0.0", 8081, &addr) != 0) {
        fprintf(stderr, "vox_socket_parse_address failed\n");
        return 1;
    }

    if (vox_http_server_listen_tcp(server, &addr, 128) != 0) {
        fprintf(stderr, "listen tcp failed\n");
        return 1;
    }

    VOX_LOG_INFO("WS echo listening on 0.0.0.0:8081 (GET /ws)");
    return vox_loop_run(loop, VOX_RUN_DEFAULT);
}

