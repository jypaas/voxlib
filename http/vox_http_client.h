/*
 * vox_http_client.h - HTTP/HTTPS Client（异步）
 *
 * - 支持 http:// 与 https://
 * - URL -> DNS(getaddrinfo) -> TCP/TLS connect -> send request -> parse response
 * - 解析响应复用 vox_http_parser（VOX_HTTP_PARSER_TYPE_RESPONSE）
 *
 * 说明：
 * - 当前实现面向 HTTP/1.1 单次请求（默认 Connection: close）
 * - HTTPS 依赖 vox_tls（当前后端为 OpenSSL Memory BIO）
 */
 
#ifndef VOX_HTTP_CLIENT_H
#define VOX_HTTP_CLIENT_H

#include "../vox_os.h"
#include "../vox_loop.h"
#include "../vox_mpool.h"
#include "../vox_socket.h"
#include "../vox_tcp.h"
#include "../vox_tls.h"
#include "../vox_dns.h"
#include "../vox_string.h"
#include "../ssl/vox_ssl.h"

#include "vox_http_parser.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vox_http_client vox_http_client_t;
typedef struct vox_http_client_req vox_http_client_req_t;

typedef struct {
    const char* name;
    const char* value;
} vox_http_client_header_t;

typedef struct {
    vox_http_method_t method;          /* 请求方法 */
    const char* url;                   /* 仅支持 http:// 与 https:// */
    const vox_http_client_header_t* headers;
    size_t header_count;
    const void* body;
    size_t body_len;
    vox_ssl_context_t* ssl_ctx;        /* https 可选：NULL 则内部创建默认 client ctx */
    uint32_t connection_timeout_ms;    /* 连接超时（毫秒），覆盖 DNS + TCP/TLS 建立；0 表示不设超时 */
} vox_http_client_request_t;

typedef struct {
    /* 连接/握手完成（此时已可开始发请求） */
    void (*on_connect)(vox_http_client_req_t* req, void* user_data);

    /* 解析到 status line 后（状态码可用） */
    void (*on_status)(vox_http_client_req_t* req, int status_code, int http_major, int http_minor, void* user_data);

    /* 每解析到一个完整 header（name/value 都是稳定视图，生命周期至少到 on_complete/on_error） */
    void (*on_header)(vox_http_client_req_t* req, vox_strview_t name, vox_strview_t value, void* user_data);

    /* headers 完成（之后开始 body 回调） */
    void (*on_headers_complete)(vox_http_client_req_t* req, void* user_data);

    /* body 数据分片（data 指向内部读缓冲，回调内使用或自行拷贝） */
    void (*on_body)(vox_http_client_req_t* req, const void* data, size_t len, void* user_data);

    /* 整个响应完成（HTTP message complete） */
    void (*on_complete)(vox_http_client_req_t* req, int status, void* user_data);

    /* 发生错误（DNS/连接/解析/读写） */
    void (*on_error)(vox_http_client_req_t* req, const char* message, void* user_data);
} vox_http_client_callbacks_t;

vox_http_client_t* vox_http_client_create(vox_loop_t* loop);
void vox_http_client_destroy(vox_http_client_t* client);

/**
 * 发起一个请求
 * @return 成功返回0，失败返回-1
 */
int vox_http_client_request(vox_http_client_t* client,
                            const vox_http_client_request_t* request,
                            const vox_http_client_callbacks_t* cbs,
                            void* user_data,
                            vox_http_client_req_t** out_req);

/**
 * 取消请求（best-effort）
 */
void vox_http_client_cancel(vox_http_client_req_t* req);

/**
 * 关闭当前请求的底层连接（不触发 on_error）。
 * 响应完成后若服务端未送 Connection: close，可调用此接口主动关连接以便 loop 能退出（VOX_RUN_DEFAULT 需无活跃句柄）。
 */
void vox_http_client_close(vox_http_client_req_t* req);

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_CLIENT_H */

