/*
 * vox_http_server.c - Server 实现
 * 先搭骨架，后续在 http-server-integration / https-support todo 中补齐连接管理与解析/写回。
 */

#include "vox_http_server.h"
#include "vox_http_internal.h"
#include "../vox_log.h"
#include "../vox_handle.h"
#include "../vox_list.h"
#include <string.h>

typedef struct vox_http_conn {
    vox_list_node_t node;
    vox_http_server_t* server;

    bool is_tls;
    vox_tcp_t* tcp;
    vox_tls_t* tls;

    vox_mpool_t* mpool; /* 连接级 mpool：连接关闭时统一释放 */
    /* defer 场景生命周期保护：
     * - defer 后可能有异步回调在连接关闭后仍访问 ctx/mpool
     * - 通过 defer_refs 延迟销毁连接 mpool，直到 finish/cancel 释放 */
    int defer_refs;
    bool closing;
    bool handle_closed;

    vox_http_parser_t* parser;

    /* 解析累积（按请求重置） */
    vox_string_t* url;
    vox_string_t* body;
    vox_string_t* cur_h_name;
    vox_string_t* cur_h_value;
    vox_vector_t* headers; /* element: vox_http_header_t* (name/value 为 mpool 拷贝后的 strview) */

    /* Connection/Upgrade 相关 */
    bool conn_keep_alive;
    bool conn_close;
    bool upgrade_websocket;

    /* 写回相关（简化：每次只允许一个在途响应） */
    bool write_pending;
    bool deferred_pending; /* defer 后阻止继续解析后续请求（pipeline 数据缓存到 pending_in） */
    bool should_close_after_write;
    vox_string_t* out;
    vox_string_t* pending_in; /* 写回期间缓存的后续数据（pipeline） */

    /* WebSocket 模式 */
    bool ws_mode;
    bool ws_upgrade_pending;
    vox_http_ws_conn_t* ws;

    /* 客户端IP缓存（连接建立时获取，避免每次请求都调用getpeername） */
    char cached_ip[64];
    bool ip_cached;

    /* 当前 ctx（复用，避免反复分配） */
    vox_http_context_t ctx;

    /* sendfile：headers 写完后由 write_done 发送文件体并关闭 file */
    vox_file_t* sendfile_file;
    int64_t sendfile_offset;
    size_t sendfile_count;
} vox_http_conn_t;

struct vox_http_server {
    vox_http_engine_t* engine;
    vox_loop_t* loop;
    vox_mpool_t* mpool;

    vox_tcp_t* tcp_server;
    vox_tls_t* tls_server;
    vox_ssl_context_t* ssl_ctx;

    vox_list_t conns;
};

/* vox_http_str_contains_token_ci 已移至 vox_http_internal.h */

static void vox_http_conn_reset_request(vox_http_conn_t* c) {
    if (!c) return;
    if (c->url) vox_string_clear(c->url);
    if (c->body) vox_string_clear(c->body);
    if (c->cur_h_name) vox_string_clear(c->cur_h_name);
    if (c->cur_h_value) vox_string_clear(c->cur_h_value);
    if (c->headers) vox_vector_clear(c->headers);
    c->conn_keep_alive = false;
    c->conn_close = false;
    c->upgrade_websocket = false;
    memset(&c->ctx.req, 0, sizeof(c->ctx.req));
    memset(&c->ctx.res, 0, sizeof(c->ctx.res));
    c->ctx.params = NULL;
    c->ctx.param_count = 0;
    c->ctx.handlers = NULL;
    c->ctx.handler_count = 0;
    c->ctx.index = 0;
    c->ctx.aborted = false;
    c->ctx.deferred = false;
    c->ctx.sendfile_file = NULL;
    c->ctx.sendfile_offset = 0;
    c->ctx.sendfile_count = 0;
    c->ctx.res_has_connection_header = false;
    c->deferred_pending = false;
}

/* forward declarations for send_response() dependencies */
static int vox_http_res_has_header(const vox_vector_t* headers, const char* name);
static bool vox_http_should_keep_alive(const vox_http_request_t* req, const vox_http_conn_t* c);
static void vox_http_conn_close(vox_http_conn_t* c);
static void vox_http_conn_try_destroy(vox_http_conn_t* c);

/* forward declarations for send_response() */
static void vox_http_tcp_write_done(vox_tcp_t* tcp, int status, void* user_data);
static void vox_http_tls_write_done(vox_tls_t* tls, int status, void* user_data);

int vox_http_conn_send_response(void* conn) {
    vox_http_conn_t* c = (vox_http_conn_t*)conn;
    if (!c) return -1;
    if (!c->server || !c->server->engine) return -1;
    if (c->closing || c->handle_closed) return -1;
    if (c->write_pending) return -1;
    /* 进入写回阶段，解除 defer 阻塞 */
    c->deferred_pending = false;

    vox_http_context_t* ctx = &c->ctx;
    /* TLS 无法使用 sendfile，将文件读入 body */
    if (ctx->sendfile_file && c->is_tls) {
        vox_string_t* body = ctx->res.body ? ctx->res.body : vox_string_create(c->mpool);
        if (body) {
            char buf[65536];
            int64_t n;
            if (ctx->sendfile_offset > 0 && vox_file_seek(ctx->sendfile_file, ctx->sendfile_offset, VOX_FILE_SEEK_SET) != 0) {
                vox_file_close(ctx->sendfile_file);
                ctx->sendfile_file = NULL;
            } else {
                size_t remain = ctx->sendfile_count;
                while (remain > 0 && (n = vox_file_read(ctx->sendfile_file, buf, remain > sizeof(buf) ? sizeof(buf) : remain)) > 0) {
                    vox_string_append_data(body, buf, (size_t)n);
                    remain -= (size_t)n;
                }
                if (!ctx->res.body) ctx->res.body = body;
                vox_file_close(ctx->sendfile_file);
            }
        } else {
            vox_file_close(ctx->sendfile_file);
        }
        ctx->sendfile_file = NULL;
        ctx->sendfile_offset = 0;
        ctx->sendfile_count = 0;
    }

    vox_http_request_t* req = &c->ctx.req;

    /* keep-alive / close 决策 */
    bool keep_alive = vox_http_should_keep_alive(req, c);
    c->should_close_after_write = !keep_alive;
    /* WebSocket upgrade 后连接必须保持打开 */
    if (c->ws_upgrade_pending) {
        c->should_close_after_write = false;
    }

    /* 自动添加 Connection: close（若需要且用户未设置）；优先用缓存避免线性扫描 res.headers */
    if (c->should_close_after_write) {
        if (!c->ctx.res_has_connection_header &&
            (!c->ctx.res.headers || !vox_http_res_has_header((const vox_vector_t*)c->ctx.res.headers, "Connection"))) {
            vox_http_context_header(&c->ctx, "Connection", "close");
        }
    }

    if (!c->out) c->out = vox_string_create(c->mpool);
    if (!c->out) return -1;
    if (vox_http_context_build_response(&c->ctx, c->out) != 0) return -1;

    /* 若使用 sendfile，将 file 交给 conn，headers 已写入 out */
    if (ctx->sendfile_file && !c->is_tls) {
        c->sendfile_file = ctx->sendfile_file;
        c->sendfile_offset = ctx->sendfile_offset;
        c->sendfile_count = ctx->sendfile_count;
        ctx->sendfile_file = NULL;
    } else {
        c->sendfile_file = NULL;
    }

    /* 暂停读取，避免 pipeline 触发并发响应 */
    if (c->is_tls) {
        if (c->tls) vox_tls_read_stop(c->tls);
    } else {
        if (c->tcp) vox_tcp_read_stop(c->tcp);
    }

    c->write_pending = true;
    const void* buf = vox_string_data(c->out);
    size_t blen = vox_string_length(c->out);
    if (c->is_tls) {
        if (!c->tls) return -1;
        if (vox_tls_write(c->tls, buf, blen, vox_http_tls_write_done) != 0) {
            c->write_pending = false;
            vox_http_conn_close(c);
            return -1;
        }
    } else {
        if (!c->tcp) return -1;
        if (vox_tcp_write(c->tcp, buf, blen, vox_http_tcp_write_done) != 0) {
            c->write_pending = false;
            vox_http_conn_close(c);
            return -1;
        }
    }
    return 0;
}

static int vox_http_conn_commit_header(vox_http_conn_t* c) {
    if (!c || !c->headers || !c->cur_h_name || !c->cur_h_value) return 0;
    size_t nlen = vox_string_length(c->cur_h_name);
    if (nlen == 0) return 0;
    size_t vlen = vox_string_length(c->cur_h_value);

    const char* nsrc = vox_string_cstr(c->cur_h_name);
    const char* vsrc = vox_string_cstr(c->cur_h_value);

    vox_http_header_t* kv = (vox_http_header_t*)vox_mpool_alloc(c->mpool, sizeof(vox_http_header_t));
    if (!kv) return -1;

    char* ncopy = (char*)vox_mpool_alloc(c->mpool, nlen + 1);
    char* vcopy = (char*)vox_mpool_alloc(c->mpool, vlen + 1);
    if (!ncopy || !vcopy) {
        vox_mpool_free(c->mpool, kv);
        return -1;
    }
    memcpy(ncopy, nsrc, nlen);
    ncopy[nlen] = '\0';
    memcpy(vcopy, vsrc, vlen);
    vcopy[vlen] = '\0';

    kv->name = (vox_strview_t){ ncopy, nlen };
    kv->value = (vox_strview_t){ vcopy, vlen };
    if (vox_vector_push(c->headers, kv) != 0) {
        vox_mpool_free(c->mpool, kv);
        vox_mpool_free(c->mpool, ncopy);
        vox_mpool_free(c->mpool, vcopy);
        return -1;
    }

    /* Connection/Upgrade/WS 相关快速判断 */
    if (vox_http_strieq(kv->name.ptr, kv->name.len, "Connection", 10)) {
        if (vox_http_str_contains_token_ci(kv->value.ptr, kv->value.len, "close")) c->conn_close = true;
        if (vox_http_str_contains_token_ci(kv->value.ptr, kv->value.len, "keep-alive")) c->conn_keep_alive = true;
        if (vox_http_str_contains_token_ci(kv->value.ptr, kv->value.len, "upgrade")) {
            /* 仅标记，具体 upgrade 类型在 Upgrade 头里看 */
        }
    } else if (vox_http_strieq(kv->name.ptr, kv->name.len, "Upgrade", 7)) {
        if (vox_http_str_contains_token_ci(kv->value.ptr, kv->value.len, "websocket")) c->upgrade_websocket = true;
    }

    vox_string_clear(c->cur_h_name);
    vox_string_clear(c->cur_h_value);
    return 0;
}

static int vox_http_res_has_header(const vox_vector_t* headers, const char* name) {
    if (!headers || !name) return 0;
    size_t nlen = strlen(name);
    size_t cnt = vox_vector_size((vox_vector_t*)headers);
    for (size_t i = 0; i < cnt; i++) {
        const vox_http_header_t* kv = (const vox_http_header_t*)vox_vector_get((vox_vector_t*)headers, i);
        if (!kv) continue;
        if (vox_http_strieq(kv->name.ptr, kv->name.len, name, nlen)) return 1;
    }
    return 0;
}

static bool vox_http_should_keep_alive(const vox_http_request_t* req, const vox_http_conn_t* c) {
    if (!req || !c) return false;
    /* HTTP/1.1 默认 keep-alive；HTTP/1.0 默认 close */
    bool keep = (req->http_major > 1) || (req->http_major == 1 && req->http_minor == 1);
    if (req->http_major == 1 && req->http_minor == 0) keep = false;

    if (c->conn_close) keep = false;
    if (c->conn_keep_alive) keep = true;
    return keep;
}

static void vox_http_conn_on_handle_closed(vox_handle_t* handle);

static void vox_http_conn_close(vox_http_conn_t* c) {
    if (!c) return;
    /* 如果已经在关闭中，避免重复关闭 */
    if (c->closing) return;
    c->closing = true;
    
    /* 先停止读取，避免在关闭过程中收到新的数据 */
    if (c->is_tls) {
        if (c->tls) {
            vox_tls_read_stop(c->tls);
            if (!vox_handle_is_closing((vox_handle_t*)c->tls)) {
                vox_handle_close((vox_handle_t*)c->tls, vox_http_conn_on_handle_closed);
            }
        }
    } else {
        if (c->tcp) {
            /* 在关闭TCP连接前，确保停止所有读取操作 */
            vox_tcp_read_stop(c->tcp);
            if (!vox_handle_is_closing((vox_handle_t*)c->tcp)) {
                vox_handle_close((vox_handle_t*)c->tcp, vox_http_conn_on_handle_closed);
            }
        }
    }
}

static void vox_http_conn_try_destroy(vox_http_conn_t* c) {
    if (!c) return;
    if (!c->handle_closed) return;
    if (c->defer_refs != 0) return;
    /* conn 本体与其所有资源都来自 conn->mpool */
    if (c->mpool) {
        vox_mpool_destroy(c->mpool);
    }
}

static void vox_http_conn_on_handle_closed(vox_handle_t* handle) {
    if (!handle) return;
    vox_http_conn_t* c = (vox_http_conn_t*)vox_handle_get_data(handle);
    if (!c) return;
    vox_http_server_t* s = c->server;
    if (s) {
        vox_list_remove(&s->conns, &c->node);
    }
    c->handle_closed = true;
    vox_http_conn_try_destroy(c);
}

static int vox_http_on_message_begin(void* parser) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    vox_http_conn_t* c = (vox_http_conn_t*)vox_http_parser_get_user_data(p);
    if (!c) return -1;
    vox_http_conn_reset_request(c);
    return 0;
}

static int vox_http_on_url(void* parser, const char* data, size_t len) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    vox_http_conn_t* c = (vox_http_conn_t*)vox_http_parser_get_user_data(p);
    if (!c || !c->url) return -1;
    if (len > 0) vox_string_append_data(c->url, data, len);
    return 0;
}

static int vox_http_on_header_field(void* parser, const char* data, size_t len) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    vox_http_conn_t* c = (vox_http_conn_t*)vox_http_parser_get_user_data(p);
    if (!c || !c->cur_h_name || !c->cur_h_value) return -1;
    /* 新 header name 开始前：若已有 value，先提交上一条 */
    if (vox_string_length(c->cur_h_value) > 0) {
        if (vox_http_conn_commit_header(c) != 0) return -1;
    }
    if (len > 0) vox_string_append_data(c->cur_h_name, data, len);
    return 0;
}

static int vox_http_on_header_value(void* parser, const char* data, size_t len) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    vox_http_conn_t* c = (vox_http_conn_t*)vox_http_parser_get_user_data(p);
    if (!c || !c->cur_h_value) return -1;
    if (len > 0) vox_string_append_data(c->cur_h_value, data, len);
    return 0;
}

static int vox_http_on_headers_complete(void* parser) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    vox_http_conn_t* c = (vox_http_conn_t*)vox_http_parser_get_user_data(p);
    if (!c) return -1;

    if (vox_http_conn_commit_header(c) != 0) return -1;

    vox_http_request_t* req = &c->ctx.req;
    req->method = vox_http_parser_get_method(p);
    req->http_major = vox_http_parser_get_http_major(p);
    req->http_minor = vox_http_parser_get_http_minor(p);
    req->is_upgrade = vox_http_parser_is_upgrade(p);
    req->headers = c->headers;
    req->body = c->body;

    /* raw_url/path/query */
    const char* u = c->url ? vox_string_cstr(c->url) : NULL;
    size_t ulen = c->url ? vox_string_length(c->url) : 0;
    if (!u || ulen == 0) {
        req->raw_url = (vox_strview_t)VOX_STRVIEW_NULL;
        req->path = (vox_strview_t)VOX_STRVIEW_NULL;
        req->query = (vox_strview_t)VOX_STRVIEW_NULL;
    } else {
        req->raw_url = (vox_strview_t){ u, ulen };
        const char* q = (const char*)memchr(u, '?', ulen);
        if (!q) {
            req->path = req->raw_url;
            req->query = (vox_strview_t)VOX_STRVIEW_NULL;
        } else {
            size_t plen = (size_t)(q - u);
            req->path = (vox_strview_t){ u, plen };
            req->query = (vox_strview_t){ q + 1, ulen - plen - 1 };
        }
        if (req->path.len == 0) {
            req->path = vox_strview_from_cstr("/");
        }
    }
    return 0;
}

static int vox_http_on_body(void* parser, const char* data, size_t len) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    vox_http_conn_t* c = (vox_http_conn_t*)vox_http_parser_get_user_data(p);
    if (!c || !c->body) return -1;
    if (len > 0) vox_string_append_data(c->body, data, len);
    return 0;
}

static void vox_http_tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data);
static void vox_http_tcp_write_done(vox_tcp_t* tcp, int status, void* user_data);
static void vox_http_tls_read_cb(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data);
static void vox_http_tls_write_done(vox_tls_t* tls, int status, void* user_data);
static void vox_http_tcp_ws_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data);
static void vox_http_tls_ws_read_cb(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data);

static int vox_http_on_message_complete(void* parser) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    vox_http_conn_t* c = (vox_http_conn_t*)vox_http_parser_get_user_data(p);
    if (!c || !c->server || !c->server->engine) return -1;
    if (c->write_pending || c->deferred_pending) {
        /* 简化：不允许并发响应（pipeline 数据会缓存到 pending_in） */
        return 0;
    }

    vox_http_engine_t* engine = c->server->engine;
    vox_http_router_t* router = vox_http_engine_get_router(engine);
    if (!router) return -1;

    vox_http_request_t* req = &c->ctx.req;

    vox_http_route_match_t match;
    memset(&match, 0, sizeof(match));

    int match_rc = -1;
    if (req->path.ptr && req->path.len > 0) {
        match_rc = vox_http_router_match(router, req->method, req->path.ptr, req->path.len, c->mpool, &match);
    }

    /* 初始化 ctx 基本字段 */
    c->ctx.mpool = c->mpool;
    c->ctx.loop = c->server->loop;
    c->ctx.engine = engine;
    c->ctx.conn = c;
    c->ctx.user_data = NULL;
    c->ctx.res.status = 0;
    c->ctx.res.headers = NULL;
    c->ctx.res.body = NULL;
    c->ctx.index = 0;
    c->ctx.aborted = false;
    c->ctx.deferred = false;

    if (match_rc != 0) {
        /* 404 */
        c->ctx.handlers = NULL;
        c->ctx.handler_count = 0;
        c->ctx.params = NULL;
        c->ctx.param_count = 0;
        vox_http_context_status(&c->ctx, 404);
        vox_http_context_write_cstr(&c->ctx, "404 Not Found");
    } else {
        c->ctx.handlers = match.handlers;
        c->ctx.handler_count = match.handler_count;
        c->ctx.params = match.params;
        c->ctx.param_count = match.param_count;
        /* 执行 middleware chain */
        vox_http_context_next(&c->ctx);
        if (!c->ctx.deferred) {
            if (!c->ctx.res.status) {
                /* 若 handler 未设置状态，默认 200 */
                c->ctx.res.status = 200;
            }
        }
    }

    /* defer：handler 返回后不立即发送，等待 finish() */
    if (c->ctx.deferred) {
        c->deferred_pending = true;
        /* 暂停读取，避免 pipeline 覆盖 ctx */
        if (c->is_tls) {
            if (c->tls) vox_tls_read_stop(c->tls);
        } else {
            if (c->tcp) vox_tcp_read_stop(c->tcp);
        }
        return 0;
    }

    return vox_http_conn_send_response(c);
}

static int vox_http_on_error(void* parser, const char* message) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    vox_http_conn_t* c = (vox_http_conn_t*)vox_http_parser_get_user_data(p);
    VOX_LOG_ERROR("http parser error: %s", message ? message : "(null)");
    if (c) vox_http_conn_close(c);
    return 0;
}

static void vox_http_drain_pending(vox_http_conn_t* c) {
    if (!c || !c->pending_in || c->write_pending || c->deferred_pending) return;
    size_t plen = vox_string_length(c->pending_in);
    if (plen == 0) return;

    const char* p = (const char*)vox_string_data(c->pending_in);
    size_t left = plen;
    while (left > 0 && !c->write_pending && !c->deferred_pending) {
        ssize_t n = vox_http_parser_execute(c->parser, p, left);
        if (n < 0) {
            vox_http_conn_close(c);
            break;
        }
        if (n == 0) break;
        p += (size_t)n;
        left -= (size_t)n;
        if (c->write_pending || c->deferred_pending) break;
    }
    /* 移除已消费的数据 */
    if (left == 0) {
        vox_string_clear(c->pending_in);
    } else {
        size_t consumed = plen - left;
        if (consumed > 0) {
            vox_string_remove(c->pending_in, 0, consumed);
        }
    }
}

static void vox_http_tcp_write_done(vox_tcp_t* tcp, int status, void* user_data) {
    vox_http_conn_t* c = (vox_http_conn_t*)user_data;
    VOX_UNUSED(tcp);
    if (!c) return;
    c->write_pending = false;
    if (status != 0) {
        if (c->sendfile_file) {
            vox_file_close(c->sendfile_file);
            c->sendfile_file = NULL;
        }
        vox_http_conn_close(c);
        return;
    }
    /* sendfile：headers 已发送，再发送文件体 */
    if (c->sendfile_file && c->tcp) {
        intptr_t fd = vox_file_get_fd(c->sendfile_file);
        size_t sent = 0;
        int r = vox_socket_sendfile(&c->tcp->socket, fd, c->sendfile_offset, c->sendfile_count, &sent);
        vox_file_close(c->sendfile_file);
        c->sendfile_file = NULL;
        if (r != 0 || (sent > 0 && sent < c->sendfile_count)) {
            vox_http_conn_close(c);
            return;
        }
    }
    if (c->should_close_after_write) {
        vox_http_conn_close(c);
        return;
    }

    /* WebSocket upgrade：切换到 WS 读循环，不再解析 HTTP */
    if (c->ws_upgrade_pending && c->ws) {
        c->ws_upgrade_pending = false;
        c->ws_mode = true;
        /* 若握手期间已经缓存了后续数据（极少见），先喂给 ws 解析器 */
        if (c->pending_in && vox_string_length(c->pending_in) > 0) {
            (void)vox_http_ws_internal_feed(c->ws, vox_string_data(c->pending_in), vox_string_length(c->pending_in));
            vox_string_clear(c->pending_in);
        }
        if (c->tcp) {
            vox_tcp_read_start(c->tcp, NULL, vox_http_tcp_ws_read_cb);
        }
        vox_http_ws_internal_on_open(c->ws);
        return;
    }

    /* 准备解析下一个请求 */
    vox_http_parser_reset(c->parser);
    vox_http_conn_reset_request(c);

    /* 先消费 pipeline 缓存 */
    vox_http_drain_pending(c);

    /* 恢复读取 */
    if (c->tcp) {
        vox_tcp_read_start(c->tcp, NULL, vox_http_tcp_read_cb);
    }
}

static void vox_http_tls_write_done(vox_tls_t* tls, int status, void* user_data) {
    vox_http_conn_t* c = (vox_http_conn_t*)user_data;
    VOX_UNUSED(tls);
    if (!c) return;
    c->write_pending = false;
    if (status != 0) {
        vox_http_conn_close(c);
        return;
    }
    if (c->should_close_after_write) {
        vox_http_conn_close(c);
        return;
    }

    /* WebSocket upgrade：切换到 WS 读循环，不再解析 HTTP */
    if (c->ws_upgrade_pending && c->ws) {
        c->ws_upgrade_pending = false;
        c->ws_mode = true;
        /* 若握手期间已经缓存了后续数据（极少见），先喂给 ws 解析器 */
        if (c->pending_in && vox_string_length(c->pending_in) > 0) {
            (void)vox_http_ws_internal_feed(c->ws, vox_string_data(c->pending_in), vox_string_length(c->pending_in));
            vox_string_clear(c->pending_in);
        }
        if (c->tls) {
            vox_tls_read_start(c->tls, NULL, vox_http_tls_ws_read_cb);
        }
        vox_http_ws_internal_on_open(c->ws);
        return;
    }

    /* 准备解析下一个请求 */
    vox_http_parser_reset(c->parser);
    vox_http_conn_reset_request(c);

    /* 先消费 pipeline 缓存 */
    vox_http_drain_pending(c);

    /* 恢复读取 */
    if (c->tls) {
        vox_tls_read_start(c->tls, NULL, vox_http_tls_read_cb);
    }
}

static void vox_http_tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    vox_http_conn_t* c = (vox_http_conn_t*)user_data;
    VOX_UNUSED(tcp);
    if (!c) return;
    if (nread <= 0) {
        vox_http_conn_close(c);
        return;
    }
    if (!buf) return;

    const char* p = (const char*)buf;
    size_t left = (size_t)nread;

    if (c->write_pending || c->deferred_pending) {
        if (!c->pending_in) c->pending_in = vox_string_create(c->mpool);
        if (c->pending_in) vox_string_append_data(c->pending_in, p, left);
        return;
    }

    while (left > 0 && !c->write_pending && !c->deferred_pending) {
        ssize_t n = vox_http_parser_execute(c->parser, p, left);
        if (n < 0) {
            vox_http_conn_close(c);
            return;
        }
        if (n == 0) break;
        p += (size_t)n;
        left -= (size_t)n;
        if (c->write_pending || c->deferred_pending) {
            /* 缓存剩余数据 */
            if (left > 0) {
                if (!c->pending_in) c->pending_in = vox_string_create(c->mpool);
                if (c->pending_in) vox_string_append_data(c->pending_in, p, left);
            }
            return;
        }
    }
}

static void vox_http_tls_read_cb(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data) {
    vox_http_conn_t* c = (vox_http_conn_t*)user_data;
    VOX_UNUSED(tls);
    if (!c) return;
    if (nread <= 0) {
        vox_http_conn_close(c);
        return;
    }
    if (!buf) return;

    const char* p = (const char*)buf;
    size_t left = (size_t)nread;

    if (c->write_pending || c->deferred_pending) {
        if (!c->pending_in) c->pending_in = vox_string_create(c->mpool);
        if (c->pending_in) vox_string_append_data(c->pending_in, p, left);
        return;
    }

    while (left > 0 && !c->write_pending && !c->deferred_pending) {
        ssize_t n = vox_http_parser_execute(c->parser, p, left);
        if (n < 0) {
            vox_http_conn_close(c);
            return;
        }
        if (n == 0) break;
        p += (size_t)n;
        left -= (size_t)n;
        if (c->write_pending || c->deferred_pending) {
            /* 缓存剩余数据 */
            if (left > 0) {
                if (!c->pending_in) c->pending_in = vox_string_create(c->mpool);
                if (c->pending_in) vox_string_append_data(c->pending_in, p, left);
            }
            return;
        }
    }
}

static void vox_http_tcp_ws_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    vox_http_conn_t* c = (vox_http_conn_t*)user_data;
    VOX_UNUSED(tcp);
    if (!c) return;
    /* 如果连接已经在关闭中，忽略后续的读取事件 */
    if (c->closing) return;
    if (nread <= 0) {
        vox_http_conn_close(c);
        return;
    }
    if (!buf || !c->ws) return;
    if (vox_http_ws_internal_feed(c->ws, buf, (size_t)nread) != 0) {
        vox_http_conn_close(c);
    }
}

static void vox_http_tls_ws_read_cb(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data) {
    vox_http_conn_t* c = (vox_http_conn_t*)user_data;
    VOX_UNUSED(tls);
    if (!c) return;
    /* 如果连接已经在关闭中，忽略后续的读取事件 */
    if (c->closing) return;
    if (nread <= 0) {
        vox_http_conn_close(c);
        return;
    }
    if (!buf || !c->ws) return;
    if (vox_http_ws_internal_feed(c->ws, buf, (size_t)nread) != 0) {
        vox_http_conn_close(c);
    }
}

static void vox_http_tcp_connection_cb(vox_tcp_t* server, int status, void* user_data) {
    vox_http_server_t* s = (vox_http_server_t*)user_data;
    if (!s || status != 0) return;

    vox_tcp_t* client = vox_tcp_create(s->loop);
    if (!client) return;
    if (vox_tcp_accept(server, client) != 0) {
        vox_handle_close((vox_handle_t*)client, NULL);
        return;
    }

    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        vox_handle_close((vox_handle_t*)client, NULL);
        return;
    }

    vox_http_conn_t* c = (vox_http_conn_t*)vox_mpool_alloc(mpool, sizeof(vox_http_conn_t));
    if (!c) {
        vox_mpool_destroy(mpool);
        vox_handle_close((vox_handle_t*)client, NULL);
        return;
    }
    memset(c, 0, sizeof(*c));
    vox_list_node_init(&c->node);
    c->server = s;
    c->is_tls = false;
    c->tcp = client;
    c->mpool = mpool;
    c->defer_refs = 0;
    c->closing = false;
    c->handle_closed = false;
    c->ip_cached = false;
    c->url = vox_string_create(mpool);
    c->body = vox_string_create(mpool);
    c->cur_h_name = vox_string_create(mpool);
    c->cur_h_value = vox_string_create(mpool);
    c->headers = vox_vector_create(mpool);
    c->out = vox_string_create(mpool);
    c->pending_in = vox_string_create(mpool);
    
    /* 缓存客户端IP地址 */
    vox_socket_addr_t peer_addr;
    if (vox_tcp_getpeername(client, &peer_addr) == 0) {
        if (vox_socket_address_to_string(&peer_addr, c->cached_ip, sizeof(c->cached_ip)) > 0) {
            c->ip_cached = true;
        }
    }

    if (!c->url || !c->body || !c->cur_h_name || !c->cur_h_value || !c->headers || !c->out || !c->pending_in) {
        vox_mpool_destroy(mpool);
        vox_handle_close((vox_handle_t*)client, NULL);
        return;
    }

    vox_http_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_message_begin = vox_http_on_message_begin;
    cb.on_url = vox_http_on_url;
    cb.on_header_field = vox_http_on_header_field;
    cb.on_header_value = vox_http_on_header_value;
    cb.on_headers_complete = vox_http_on_headers_complete;
    cb.on_body = vox_http_on_body;
    cb.on_message_complete = vox_http_on_message_complete;
    cb.on_error = vox_http_on_error;
    cb.user_data = c;

    vox_http_parser_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.type = VOX_HTTP_PARSER_TYPE_REQUEST;
    c->parser = vox_http_parser_create(mpool, &cfg, &cb);
    if (!c->parser) {
        vox_mpool_destroy(mpool);
        vox_handle_close((vox_handle_t*)client, NULL);
        return;
    }
    vox_http_parser_set_user_data(c->parser, c);

    /* 初始化 ctx 固定字段 */
    memset(&c->ctx, 0, sizeof(c->ctx));
    c->ctx.mpool = mpool;
    c->ctx.loop = s->loop;
    c->ctx.engine = s->engine;
    c->ctx.conn = c;

    vox_handle_set_data((vox_handle_t*)client, c);
    vox_list_push_back(&s->conns, &c->node);

    /* 开始读 */
    if (vox_tcp_read_start(client, NULL, vox_http_tcp_read_cb) != 0) {
        vox_handle_close((vox_handle_t*)client, vox_http_conn_on_handle_closed);
        return;
    }
}

static void vox_http_tls_handshake_cb(vox_tls_t* tls, int status, void* user_data) {
    vox_http_conn_t* c = (vox_http_conn_t*)user_data;
    VOX_UNUSED(tls);
    if (!c) return;
    if (status != 0) {
        vox_http_conn_close(c);
        return;
    }
    
    /* TLS握手完成后，缓存客户端IP地址 */
    if (!c->ip_cached && c->tls) {
        vox_socket_addr_t peer_addr;
        if (vox_tls_getpeername(c->tls, &peer_addr) == 0) {
            if (vox_socket_address_to_string(&peer_addr, c->cached_ip, sizeof(c->cached_ip)) > 0) {
                c->ip_cached = true;
            }
        }
    }
    
    if (c->tls) {
        if (vox_tls_read_start(c->tls, NULL, vox_http_tls_read_cb) != 0) {
            vox_http_conn_close(c);
        }
    }
}

static void vox_http_tls_connection_cb(vox_tls_t* server, int status, void* user_data) {
    vox_http_server_t* s = (vox_http_server_t*)user_data;
    if (!s || status != 0) return;

    vox_tls_t* client = vox_tls_create(s->loop, s->ssl_ctx);
    if (!client) return;
    if (vox_tls_accept(server, client) != 0) {
        vox_handle_close((vox_handle_t*)client, NULL);
        return;
    }

    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        vox_handle_close((vox_handle_t*)client, NULL);
        return;
    }

    vox_http_conn_t* c = (vox_http_conn_t*)vox_mpool_alloc(mpool, sizeof(vox_http_conn_t));
    if (!c) {
        vox_mpool_destroy(mpool);
        vox_handle_close((vox_handle_t*)client, NULL);
        return;
    }
    memset(c, 0, sizeof(*c));
    vox_list_node_init(&c->node);
    c->server = s;
    c->is_tls = true;
    c->tcp = NULL;
    c->tls = client;
    c->mpool = mpool;
    c->defer_refs = 0;
    c->closing = false;
    c->handle_closed = false;
    c->ip_cached = false;
    c->url = vox_string_create(mpool);
    c->body = vox_string_create(mpool);
    c->cur_h_name = vox_string_create(mpool);
    c->cur_h_value = vox_string_create(mpool);
    c->headers = vox_vector_create(mpool);
    c->out = vox_string_create(mpool);
    c->pending_in = vox_string_create(mpool);
    
    /* 缓存客户端IP地址（TLS握手完成后获取） */
    /* 注意：TLS连接在握手完成前可能无法获取peer地址，所以在handshake_cb中获取 */

    if (!c->url || !c->body || !c->cur_h_name || !c->cur_h_value || !c->headers || !c->out || !c->pending_in) {
        vox_mpool_destroy(mpool);
        vox_handle_close((vox_handle_t*)client, NULL);
        return;
    }

    vox_http_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_message_begin = vox_http_on_message_begin;
    cb.on_url = vox_http_on_url;
    cb.on_header_field = vox_http_on_header_field;
    cb.on_header_value = vox_http_on_header_value;
    cb.on_headers_complete = vox_http_on_headers_complete;
    cb.on_body = vox_http_on_body;
    cb.on_message_complete = vox_http_on_message_complete;
    cb.on_error = vox_http_on_error;
    cb.user_data = c;

    vox_http_parser_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.type = VOX_HTTP_PARSER_TYPE_REQUEST;
    c->parser = vox_http_parser_create(mpool, &cfg, &cb);
    if (!c->parser) {
        vox_mpool_destroy(mpool);
        vox_handle_close((vox_handle_t*)client, NULL);
        return;
    }
    vox_http_parser_set_user_data(c->parser, c);

    /* 初始化 ctx 固定字段 */
    memset(&c->ctx, 0, sizeof(c->ctx));
    c->ctx.mpool = mpool;
    c->ctx.loop = s->loop;
    c->ctx.engine = s->engine;
    c->ctx.conn = c;

    vox_handle_set_data((vox_handle_t*)client, c);
    vox_list_push_back(&s->conns, &c->node);

    /* 开始 TLS 握手，成功后再开始读 */
    if (vox_tls_handshake(client, vox_http_tls_handshake_cb) != 0) {
        vox_handle_close((vox_handle_t*)client, vox_http_conn_on_handle_closed);
        return;
    }
}

vox_http_server_t* vox_http_server_create(vox_http_engine_t* engine) {
    if (!engine) return NULL;
    vox_mpool_t* mpool = vox_http_engine_get_mpool(engine);
    vox_loop_t* loop = vox_http_engine_get_loop(engine);
    if (!mpool || !loop) return NULL;

    vox_http_server_t* s = (vox_http_server_t*)vox_mpool_alloc(mpool, sizeof(vox_http_server_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->engine = engine;
    s->loop = loop;
    s->mpool = mpool;
    vox_list_init(&s->conns);
    return s;
}

void vox_http_server_destroy(vox_http_server_t* server) {
    VOX_UNUSED(server);
    /* mpool 分配，不做深度释放 */
}

int vox_http_server_listen_tcp(vox_http_server_t* server, const vox_socket_addr_t* addr, int backlog) {
    if (!server || !addr) return -1;
    if (server->tcp_server) return -1;

    vox_tcp_t* tcp = vox_tcp_create(server->loop);
    if (!tcp) return -1;
    vox_handle_set_data((vox_handle_t*)tcp, server);

    /* 常用 socket 选项 */
    vox_tcp_reuseaddr(tcp, true);
    vox_tcp_nodelay(tcp, true);
    vox_tcp_keepalive(tcp, true);

    /* Unix 多进程 worker 需 SO_REUSEPORT 才能同端口 bind，避免 respawn 死循环 */
    unsigned int bind_flags = 0;
#ifndef VOX_OS_WINDOWS
    bind_flags = VOX_PORT_REUSE_FLAG;
#endif
    if (vox_tcp_bind(tcp, addr, bind_flags) != 0) {
        vox_handle_close((vox_handle_t*)tcp, NULL);
        return -1;
    }
    if (vox_tcp_listen(tcp, backlog, vox_http_tcp_connection_cb) != 0) {
        vox_handle_close((vox_handle_t*)tcp, NULL);
        return -1;
    }

    server->tcp_server = tcp;
    return 0;
}

int vox_http_server_listen_tls(vox_http_server_t* server, vox_ssl_context_t* ssl_ctx, const vox_socket_addr_t* addr, int backlog) {
    if (!server || !ssl_ctx || !addr) return -1;
    if (server->tls_server) return -1;

    vox_tls_t* tls = vox_tls_create(server->loop, ssl_ctx);
    if (!tls) return -1;
    vox_handle_set_data((vox_handle_t*)tls, server);

    /* 常用 socket 选项 */
    vox_tls_reuseaddr(tls, true);
    vox_tls_nodelay(tls, true);
    vox_tls_keepalive(tls, true);

    /* Unix 多进程 worker 需 SO_REUSEPORT 才能同端口 bind */
    unsigned int bind_flags = 0;
#ifndef VOX_OS_WINDOWS
    bind_flags = VOX_PORT_REUSE_FLAG;
#endif
    if (vox_tls_bind(tls, addr, bind_flags) != 0) {
        vox_handle_close((vox_handle_t*)tls, NULL);
        return -1;
    }
    if (vox_tls_listen(tls, backlog, vox_http_tls_connection_cb) != 0) {
        vox_handle_close((vox_handle_t*)tls, NULL);
        return -1;
    }

    server->ssl_ctx = ssl_ctx;
    server->tls_server = tls;
    return 0;
}

void vox_http_server_close(vox_http_server_t* server) {
    if (!server) return;

    /* 关闭 listener */
    if (server->tcp_server) {
        vox_handle_close((vox_handle_t*)server->tcp_server, NULL);
        server->tcp_server = NULL;
    }
    if (server->tls_server) {
        vox_handle_close((vox_handle_t*)server->tls_server, NULL);
        server->tls_server = NULL;
    }

    /* 关闭所有连接 */
    vox_list_node_t* pos;
    vox_list_node_t* n;
    vox_list_for_each_safe(pos, n, &server->conns) {
        vox_http_conn_t* c = vox_container_of(pos, vox_http_conn_t, node);
        if (!c) continue;
        if (c->is_tls) {
            if (c->tls) vox_handle_close((vox_handle_t*)c->tls, vox_http_conn_on_handle_closed);
        } else {
            if (c->tcp) vox_handle_close((vox_handle_t*)c->tcp, vox_http_conn_on_handle_closed);
        }
    }
}

/* ===== ws/transport 内部胶水（供 vox_http_ws.c 调用） ===== */
void vox_http_conn_defer_acquire(void* conn) {
    vox_http_conn_t* c = (vox_http_conn_t*)conn;
    if (!c) return;
    if (c->defer_refs < 0) c->defer_refs = 0;
    c->defer_refs++;
}

void vox_http_conn_defer_release(void* conn) {
    vox_http_conn_t* c = (vox_http_conn_t*)conn;
    if (!c) return;
    if (c->defer_refs > 0) c->defer_refs--;
    vox_http_conn_try_destroy(c);
}

bool vox_http_conn_is_closing_or_closed(void* conn) {
    vox_http_conn_t* c = (vox_http_conn_t*)conn;
    return (!c) || c->closing || c->handle_closed;
}

int vox_http_conn_mark_ws_upgrade(void* conn, vox_http_ws_conn_t* ws) {
    vox_http_conn_t* c = (vox_http_conn_t*)conn;
    if (!c || !ws) return -1;
    c->ws = ws;
    c->ws_upgrade_pending = true;
    c->ws_mode = false;
    /* upgrade 成功后连接必须保持 */
    c->should_close_after_write = false;
    return 0;
}

int vox_http_conn_ws_write(void* conn, const void* data, size_t len) {
    vox_http_conn_t* c = (vox_http_conn_t*)conn;
    if (!c || !data || len == 0) return -1;
    if (c->is_tls) {
        if (!c->tls) return -1;
        return vox_tls_write(c->tls, data, len, NULL);
    }
    if (!c->tcp) return -1;
    return vox_tcp_write(c->tcp, data, len, NULL);
}

void vox_http_conn_ws_close(void* conn) {
    vox_http_conn_t* c = (vox_http_conn_t*)conn;
    if (!c) return;
    vox_http_conn_close(c);
}

int vox_http_conn_get_client_ip(void* conn, char* ip_buf, size_t ip_buf_size) {
    if (!conn || !ip_buf || ip_buf_size == 0) return -1;
    vox_http_conn_t* c = (vox_http_conn_t*)conn;
    
    /* 优先检查 X-Forwarded-For 或 X-Real-IP 头（如果存在代理） */
    if (c->ctx.req.headers) {
        const vox_vector_t* vec = (const vox_vector_t*)c->ctx.req.headers;
        size_t cnt = vox_vector_size(vec);
        for (size_t i = 0; i < cnt; i++) {
            const vox_http_header_t* kv = (const vox_http_header_t*)vox_vector_get(vec, i);
            if (!kv || !kv->name.ptr || !kv->value.ptr) continue;
            
            /* 检查 X-Forwarded-For */
            if (vox_http_strieq(kv->name.ptr, kv->name.len, "X-Forwarded-For", 15)) {
                /* 取第一个IP（如果有多个，用逗号分隔） */
                const char* val = kv->value.ptr;
                size_t val_len = kv->value.len;
                const char* comma = (const char*)memchr(val, ',', val_len);
                size_t ip_len = comma ? (size_t)(comma - val) : val_len;
                
                /* 跳过前导空格 */
                while (ip_len > 0 && (*val == ' ' || *val == '\t')) {
                    val++;
                    ip_len--;
                }
                
                if (ip_len > 0 && ip_len < ip_buf_size) {
                    memcpy(ip_buf, val, ip_len);
                    ip_buf[ip_len] = '\0';
                    return 0;
                }
            }
            
            /* 检查 X-Real-IP */
            if (vox_http_strieq(kv->name.ptr, kv->name.len, "X-Real-IP", 9)) {
                if (kv->value.len > 0 && kv->value.len < ip_buf_size) {
                    memcpy(ip_buf, kv->value.ptr, kv->value.len);
                    ip_buf[kv->value.len] = '\0';
                    return 0;
                }
            }
        }
    }
    
    /* 使用缓存的IP地址（避免系统调用） */
    if (c->ip_cached && c->cached_ip[0] != '\0') {
        size_t ip_len = strlen(c->cached_ip);
        if (ip_len > 0 && ip_len < ip_buf_size) {
            memcpy(ip_buf, c->cached_ip, ip_len);
            ip_buf[ip_len] = '\0';
            return 0;
        }
    }
    
    /* 如果缓存未命中，回退到系统调用（不应该经常发生） */
    vox_socket_addr_t peer_addr;
    int ret = -1;
    
    if (c->is_tls) {
        if (c->tls) {
            ret = vox_tls_getpeername(c->tls, &peer_addr);
        }
    } else {
        if (c->tcp) {
            ret = vox_tcp_getpeername(c->tcp, &peer_addr);
        }
    }
    
    if (ret == 0) {
        if (vox_socket_address_to_string(&peer_addr, ip_buf, ip_buf_size) > 0) {
            /* 更新缓存（如果可能） */
            if (ip_buf_size >= sizeof(c->cached_ip)) {
                strncpy(c->cached_ip, ip_buf, sizeof(c->cached_ip) - 1);
                c->cached_ip[sizeof(c->cached_ip) - 1] = '\0';
                c->ip_cached = true;
            }
            return 0;
        }
    }
    
    return -1;
}

