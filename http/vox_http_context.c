/*
 * vox_http_context.c - Context 实现
 */

#include "vox_http_context.h"
#include "vox_http_internal.h"
#include "vox_http_gzip.h"
#include <string.h>

static const char* vox_http_reason_phrase(int status) {
    switch (status) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

void vox_http_context_next(vox_http_context_t* ctx) {
    if (!ctx || ctx->aborted) return;
    /* Gin 风格：
     * - Engine/Server 只需调用一次 next()，它会顺序执行剩余 handlers
     * - Middleware 若想在后续 handlers 执行完后继续执行“后置逻辑”，可在自身内部再次调用 next()
     * - abort() 会将 index 跳到末尾，终止后续执行 */
    while (ctx->index < ctx->handler_count && !ctx->aborted) {
        vox_http_handler_cb cb = ctx->handlers[ctx->index++];
        if (!cb) continue;
        cb(ctx);
    }
}

void vox_http_context_abort(vox_http_context_t* ctx) {
    if (!ctx) return;
    ctx->aborted = true;
    ctx->index = ctx->handler_count;
}

bool vox_http_context_is_aborted(const vox_http_context_t* ctx) {
    return ctx ? ctx->aborted : true;
}

void vox_http_context_defer(vox_http_context_t* ctx) {
    if (!ctx) return;
    if (!ctx->deferred) {
        ctx->deferred = true;
        if (ctx->conn) {
            vox_http_conn_defer_acquire(ctx->conn);
        }
    }
    /* defer 语义：停止继续执行后续 handlers（避免写回前修改 ctx） */
    vox_http_context_abort(ctx);
}

bool vox_http_context_is_deferred(const vox_http_context_t* ctx) {
    return ctx ? ctx->deferred : false;
}

int vox_http_context_finish(vox_http_context_t* ctx) {
    if (!ctx || !ctx->conn) return -1;
    /* 仅在 defer 模式下允许 finish；避免重复发送 */
    if (!ctx->deferred) return -1;

    /* HTTP 场景：若连接已关闭/正在关闭，直接视为取消并释放 defer hold，避免 UAF/泄漏 */
    if (vox_http_conn_is_closing_or_closed(ctx->conn)) {
        ctx->deferred = false;
        vox_http_conn_defer_release(ctx->conn);
        return -1;
    }

    /* 只有当真正进入发送流程后才清除 deferred，避免发送失败后无法重试 */
    int rc = vox_http_conn_send_response(ctx->conn);
    if (rc == 0) {
        ctx->deferred = false;
        vox_http_conn_defer_release(ctx->conn);
        return 0;
    }

    /* 发送失败但连接已关闭：同样按取消处理，释放 defer hold */
    if (vox_http_conn_is_closing_or_closed(ctx->conn)) {
        ctx->deferred = false;
        vox_http_conn_defer_release(ctx->conn);
    }
    return rc;
}

const vox_http_request_t* vox_http_context_request(const vox_http_context_t* ctx) {
    if (!ctx) return NULL;
    return (const vox_http_request_t*)&ctx->req;
}

vox_http_response_t* vox_http_context_response(vox_http_context_t* ctx) {
    if (!ctx) return NULL;
    return (vox_http_response_t*)&ctx->res;
}

vox_strview_t vox_http_context_param(const vox_http_context_t* ctx, const char* name) {
    if (!ctx || !name) return (vox_strview_t)VOX_STRVIEW_NULL;
    size_t nlen = strlen(name);
    for (size_t i = 0; i < ctx->param_count; i++) {
        if (vox_http_strieq(ctx->params[i].name.ptr, ctx->params[i].name.len, name, nlen)) {
            return ctx->params[i].value;
        }
    }
    return (vox_strview_t)VOX_STRVIEW_NULL;
}

vox_strview_t vox_http_context_get_header(const vox_http_context_t* ctx, const char* name) {
    if (!ctx || !name) return (vox_strview_t)VOX_STRVIEW_NULL;
    const vox_http_request_t* req = &ctx->req;
    if (!req->headers) return (vox_strview_t)VOX_STRVIEW_NULL;
    
    size_t nlen = strlen(name);
    const vox_vector_t* vec = (const vox_vector_t*)req->headers;
    size_t cnt = vox_vector_size(vec);
    for (size_t i = 0; i < cnt; i++) {
        const vox_http_header_t* kv = (const vox_http_header_t*)vox_vector_get(vec, i);
        if (!kv || !kv->name.ptr || !kv->value.ptr) continue;
        if (vox_http_strieq(kv->name.ptr, kv->name.len, name, nlen)) {
            return kv->value;
        }
    }
    return (vox_strview_t)VOX_STRVIEW_NULL;
}

static vox_strview_t parse_query_param(const char* query, size_t query_len, const char* name) {
    if (!query || query_len == 0 || !name) return (vox_strview_t)VOX_STRVIEW_NULL;
    size_t nlen = strlen(name);
    
    const char* p = query;
    const char* end = query + query_len;
    
    while (p < end) {
        const char* eq = (const char*)memchr(p, '=', end - p);
        if (!eq) break;
        
        size_t key_len = (size_t)(eq - p);
        if (key_len == nlen && strncmp(p, name, nlen) == 0) {
            /* 找到匹配的key */
            const char* val_start = eq + 1;
            const char* val_end = (const char*)memchr(val_start, '&', end - val_start);
            if (!val_end) val_end = end;
            
            size_t val_len = (size_t)(val_end - val_start);
            return (vox_strview_t){ val_start, val_len };
        }
        
        /* 查找下一个参数 */
        const char* next = (const char*)memchr(eq, '&', end - eq);
        if (!next) break;
        p = next + 1;
    }
    
    return (vox_strview_t)VOX_STRVIEW_NULL;
}

vox_strview_t vox_http_context_get_query(const vox_http_context_t* ctx, const char* name) {
    if (!ctx || !name) return (vox_strview_t)VOX_STRVIEW_NULL;
    const vox_http_request_t* req = &ctx->req;
    if (!req->query.ptr || req->query.len == 0) return (vox_strview_t)VOX_STRVIEW_NULL;
    return parse_query_param(req->query.ptr, req->query.len, name);
}

int vox_http_context_status(vox_http_context_t* ctx, int status) {
    if (!ctx) return -1;
    ctx->res.status = status;
    return 0;
}

static int vox_http_kv_push(vox_mpool_t* mpool, vox_vector_t* vec, const char* k, const char* v) {
    if (!vec || !k || !v) return -1;
    vox_http_header_t* kv = (vox_http_header_t*)vox_mpool_alloc(mpool, sizeof(vox_http_header_t));
    if (!kv) return -1;
    kv->name = vox_strview_from_cstr(k);
    kv->value = vox_strview_from_cstr(v);
    if (vox_vector_push(vec, kv) != 0) {
        vox_mpool_free(mpool, kv);
        return -1;
    }
    return 0;
}

int vox_http_context_header(vox_http_context_t* ctx, const char* name, const char* value) {
    if (!ctx || !name || !value) return -1;
    if (!ctx->res.headers) {
        ctx->res.headers = vox_vector_create(ctx->mpool);
        if (!ctx->res.headers) return -1;
    }
    return vox_http_kv_push(ctx->mpool, (vox_vector_t*)ctx->res.headers, name, value);
}

int vox_http_context_write(vox_http_context_t* ctx, const void* data, size_t len) {
    if (!ctx || !data || len == 0) return 0;
    if (!ctx->res.body) {
        ctx->res.body = vox_string_create(ctx->mpool);
        if (!ctx->res.body) return -1;
    }
    return vox_string_append_data(ctx->res.body, data, len);
}

int vox_http_context_write_cstr(vox_http_context_t* ctx, const char* cstr) {
    if (!cstr) return 0;
    return vox_http_context_write(ctx, cstr, strlen(cstr));
}

static void vox_http_append_status_line(vox_string_t* out, int major, int minor, int status) {
    const char* reason = vox_http_reason_phrase(status);
    vox_string_append_format(out, "HTTP/%d.%d %d %s\r\n", major, minor, status, reason);
}

static int vox_http_has_header(const vox_vector_t* headers, const char* name) {
    if (!headers || !name) return 0;
    size_t nlen = strlen(name);
    size_t cnt = vox_vector_size((vox_vector_t*)headers);
    for (size_t i = 0; i < cnt; i++) {
        vox_http_header_t* kv = (vox_http_header_t*)vox_vector_get((vox_vector_t*)headers, i);
        if (!kv) continue;
        if (vox_http_strieq(kv->name.ptr, kv->name.len, name, nlen)) return 1;
    }
    return 0;
}

int vox_http_context_build_response(const vox_http_context_t* ctx, vox_string_t* out) {
    if (!ctx || !out) return -1;

    vox_string_clear(out);

    int status = ctx->res.status ? ctx->res.status : 200;
    int major = ctx->req.http_major ? ctx->req.http_major : 1;
    int minor = ctx->req.http_minor; /* 默认 1.0/1.1 按请求版本 */
    
#ifdef VOX_USE_ZLIB
    /* 用于 gzip 压缩的变量（需要在函数作用域内） */
    bool use_gzip = false;
    vox_string_t* compressed_body = NULL;
#endif /* VOX_USE_ZLIB */

    vox_http_append_status_line(out, major, minor, status);

    /* 101 Switching Protocols：不应附带 Content-Length/Content-Type/body */
    if (status != 101) {
        size_t body_len = ctx->res.body ? vox_string_length(ctx->res.body) : 0;

#ifdef VOX_USE_ZLIB
        /* 检查是否应该使用 gzip 压缩 */
        if (body_len > 0 && !vox_http_has_header((const vox_vector_t*)ctx->res.headers, "Content-Encoding")) {
            /* 检查客户端是否支持 gzip */
            if (vox_http_supports_gzip(ctx->req.headers)) {
                /* 只压缩较大的响应体（通常 > 256 字节才有意义） */
                if (body_len >= 1024) {
                    compressed_body = vox_string_create(ctx->mpool);
                    if (compressed_body) {
                        int compress_result = vox_http_gzip_compress(ctx->mpool, 
                                                    vox_string_data(ctx->res.body), 
                                                    body_len, 
                                                    compressed_body);
                        if (compress_result == 0) {
                            size_t compressed_len = vox_string_length(compressed_body);
                            /* 只有当压缩后确实更小才使用 */
                            if (compressed_len < body_len) {
                                use_gzip = true;
                                body_len = compressed_len;
                            }
                        }
                        /* 注意：compressed_body 由 mpool 管理，不需要手动释放
                         * 如果不使用压缩，compressed_body 会在 mpool 清理时自动回收 */
                    }
                }
            }
        }
#endif /* VOX_USE_ZLIB */

        /* 自动添加 Content-Length（若未显式设置） */
        if (!vox_http_has_header((const vox_vector_t*)ctx->res.headers, "Content-Length")) {
            vox_string_append_format(out, "Content-Length: %zu\r\n", body_len);
        }
        if (!vox_http_has_header((const vox_vector_t*)ctx->res.headers, "Content-Type")) {
            /* 默认 text/plain */
            vox_string_append(out, "Content-Type: text/plain; charset=utf-8\r\n");
        }

#ifdef VOX_USE_ZLIB
        /* 如果使用了 gzip 压缩，添加 Content-Encoding 头 */
        if (use_gzip) {
            vox_string_append(out, "Content-Encoding: gzip\r\n");
        }
#endif /* VOX_USE_ZLIB */
    }

    /* 追加用户 header */
    if (ctx->res.headers) {
        size_t cnt = vox_vector_size((vox_vector_t*)ctx->res.headers);
        for (size_t i = 0; i < cnt; i++) {
            vox_http_header_t* kv = (vox_http_header_t*)vox_vector_get((vox_vector_t*)ctx->res.headers, i);
            if (!kv || !kv->name.ptr || !kv->value.ptr) continue;
            vox_string_append_format(out, "%.*s: %.*s\r\n",
                                     (int)kv->name.len, kv->name.ptr,
                                     (int)kv->value.len, kv->value.ptr);
        }
    }

    vox_string_append(out, "\r\n");
    if (status != 101) {
        size_t body_len = ctx->res.body ? vox_string_length(ctx->res.body) : 0;
        if (body_len > 0) {
#ifdef VOX_USE_ZLIB
            /* 如果使用了压缩，写入压缩后的数据 */
            if (use_gzip && compressed_body) {
                size_t compressed_len = vox_string_length(compressed_body);
                vox_string_append_data(out, vox_string_data(compressed_body), compressed_len);
                /* compressed_body 由 mpool 管理，无需手动释放 */
            } else {
                vox_string_append_data(out, vox_string_data(ctx->res.body), body_len);
            }
#else
            vox_string_append_data(out, vox_string_data(ctx->res.body), body_len);
#endif /* VOX_USE_ZLIB */
        }
    }
    return 0;
}

void* vox_http_context_get_user_data(const vox_http_context_t* ctx) {
    return ctx ? ctx->user_data : NULL;
}

void vox_http_context_set_user_data(vox_http_context_t* ctx, void* user_data) {
    if (!ctx) return;
    ctx->user_data = user_data;
}

vox_loop_t* vox_http_context_get_loop(const vox_http_context_t* ctx) {
    return ctx ? ctx->loop : NULL;
}

vox_mpool_t* vox_http_context_get_mpool(const vox_http_context_t* ctx) {
    return ctx ? ctx->mpool : NULL;
}

