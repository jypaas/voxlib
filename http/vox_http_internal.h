/*
 * vox_http_internal.h - HTTP 模块内部共享定义（非公开 API）
 * 注意：仅供 http/ 目录下模块互相引用，避免对外暴露实现细节。
 */

#ifndef VOX_HTTP_INTERNAL_H
#define VOX_HTTP_INTERNAL_H

#include "../vox_os.h"
#include "../vox_mpool.h"
#include "../vox_string.h"
#include "../vox_vector.h"
#include "../vox_loop.h"
#include "../vox_file.h"
#include "../vox_socket.h"
#include "vox_http_parser.h"
#include "vox_http_middleware.h"
#include "vox_http_context.h"
#include "vox_http_ws.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vox_http_engine;

/* 注意：
 * - vox_http_context_t/vox_http_ws_conn_t/vox_http_engine_t 等 typedef 已在公开头文件中定义。
 * - 这里仅“补全”struct vox_http_context 的内部字段，避免重复 typedef（Linux -Wpedantic）。 */
struct vox_http_context {
    vox_mpool_t* mpool;
    vox_loop_t* loop;
    struct vox_http_engine* engine;

    /* 当前请求/响应 */
    vox_http_request_t req;
    vox_http_response_t res;

    /* 路由匹配结果 */
    vox_http_param_t* params;
    size_t param_count;

    /* handler chain */
    vox_http_handler_cb* handlers;
    size_t handler_count;
    size_t index;
    bool aborted;
    bool deferred; /* handler 返回后延迟写响应 */

    /* 由 server 注入：用于写回/升级等 */
    void* conn;            /* vox_http_conn_t*（在 server.c 内定义） */
    void* user_data;

    /* sendfile：非 NULL 时响应体由 sendfile 发送，调用方不得关闭 file */
    vox_file_t* sendfile_file;
    int64_t sendfile_offset;
    size_t sendfile_count;

    /* 快速路径：handler 已通过 vox_http_context_header 设置过 Connection 头则置 true，避免 send_response 时线性扫描 res.headers */
    bool res_has_connection_header;
};

/* ===== ws/transport 内部胶水（仅供 http/ 模块使用） ===== */
int vox_http_conn_mark_ws_upgrade(void* conn, vox_http_ws_conn_t* ws);
int vox_http_conn_ws_write(void* conn, const void* data, size_t len);
void vox_http_conn_ws_close(void* conn);

/* defer response：由 context_finish 调用（仅供 http/ 模块使用） */
int vox_http_conn_send_response(void* conn);
/* defer 生命周期保护：HTTP 场景下避免“客户端提前断开 + 异步回调”导致 ctx/mpool UAF */
void vox_http_conn_defer_acquire(void* conn);
void vox_http_conn_defer_release(void* conn);
bool vox_http_conn_is_closing_or_closed(void* conn);

/* ws 模块内部：供 server 将网络数据喂给 ws 解析器 */
vox_http_ws_conn_t* vox_http_ws_internal_create(vox_mpool_t* mpool, void* conn, const vox_http_ws_callbacks_t* cbs);
int vox_http_ws_internal_feed(vox_http_ws_conn_t* ws, const void* data, size_t len);
void vox_http_ws_internal_on_open(vox_http_ws_conn_t* ws);

/* 获取客户端IP地址（仅供 http/ 模块使用） */
int vox_http_conn_get_client_ip(void* conn, char* ip_buf, size_t ip_buf_size);

/* ===== 小工具：大小写不敏感比较 ===== */
static VOX_UNUSED_FUNC int vox_http_strieq(const char* a, size_t alen, const char* b, size_t blen) {
    if (alen != blen) return 0;
    return strncasecmp(a, b, alen) == 0;
}

/* 检查字符串中是否包含指定的token（大小写不敏感，支持逗号分隔的列表） */
static VOX_UNUSED_FUNC int vox_http_str_contains_token_ci(const char* s, size_t slen, const char* tok) {
    if (!s || slen == 0 || !tok || !*tok) return 0;
    size_t tlen = strlen(tok);
    if (tlen == 0 || tlen > slen) return 0;
    for (size_t i = 0; i + tlen <= slen; i++) {
        if (strncasecmp(s + i, tok, tlen) == 0) {
            /* 检查token边界（前后是空白、逗号或字符串边界） */
            if ((i == 0 || s[i-1] == ' ' || s[i-1] == '\t' || s[i-1] == ',') &&
                (i + tlen >= slen || s[i+tlen] == ' ' || s[i+tlen] == '\t' || s[i+tlen] == ',')) {
                return 1;
            }
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_INTERNAL_H */

