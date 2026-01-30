/*
 * vox_http_ws.h - WebSocket (WS/WSS)
 * - 提供 Upgrade 握手
 * - 提供消息级 API（库内处理帧/分片/PingPong/Close）
 */

#ifndef VOX_HTTP_WS_H
#define VOX_HTTP_WS_H

#include "../vox_os.h"
#include "vox_http_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vox_http_ws_conn vox_http_ws_conn_t;

/* WebSocket 连接回调（统一命名风格） */
typedef void (*vox_http_ws_on_connect_cb)(vox_http_ws_conn_t* ws, void* user_data);
typedef void (*vox_http_ws_on_message_cb)(vox_http_ws_conn_t* ws, const void* data, size_t len, bool is_text, void* user_data);
typedef void (*vox_http_ws_on_close_cb)(vox_http_ws_conn_t* ws, int code, const char* reason, void* user_data);
typedef void (*vox_http_ws_on_error_cb)(vox_http_ws_conn_t* ws, const char* message, void* user_data);

typedef struct {
    vox_http_ws_on_connect_cb on_connect;  /* 连接建立回调 */
    vox_http_ws_on_message_cb on_message;  /* 消息接收回调 */
    vox_http_ws_on_close_cb on_close;      /* 连接关闭回调 */
    vox_http_ws_on_error_cb on_error;      /* 错误回调 */
    void* user_data;                       /* 用户数据 */
} vox_http_ws_callbacks_t;

/* 在 HTTP handler 内调用：完成握手并切换连接为 WS 模式 */
int vox_http_ws_upgrade(vox_http_context_t* ctx, const vox_http_ws_callbacks_t* cbs);

/* 发送消息（服务器侧发送不需要 mask） */
int vox_http_ws_send_text(vox_http_ws_conn_t* ws, const char* text, size_t len);
int vox_http_ws_send_binary(vox_http_ws_conn_t* ws, const void* data, size_t len);
int vox_http_ws_close(vox_http_ws_conn_t* ws, int code, const char* reason);

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_WS_H */

