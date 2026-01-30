/*
 * vox_http_client.c - HTTP/HTTPS Client 实现
 */

#include "vox_http_client.h"
#include "vox_http_gzip.h"
#include "../vox_handle.h"
#include "../vox_timer.h"
#include <string.h>
#include <stdio.h>

typedef enum {
    VOX_HTTP_CLIENT_SCHEME_HTTP = 0,
    VOX_HTTP_CLIENT_SCHEME_HTTPS
} vox_http_client_scheme_t;

typedef struct {
    vox_http_client_scheme_t scheme;
    char* host;     /* NUL 结尾 */
    uint16_t port;
    char* path;     /* NUL 结尾，必须以 '/' 开头，包含 query */
} vox_http_client_url_t;

struct vox_http_client {
    vox_loop_t* loop;
    vox_mpool_t* mpool;
};

struct vox_http_client_req {
    vox_http_client_t* client;
    vox_loop_t* loop;
    vox_mpool_t* mpool;

    vox_http_client_url_t url;
    vox_http_client_request_t request; /* 复制后的视图（指针指向 mpool 或用户原始） */

    vox_dns_getaddrinfo_t* dns_req;
    bool dns_pending;

    bool is_tls;
    vox_tcp_t* tcp;
    vox_tls_t* tls;

    vox_http_parser_t* parser;
    bool headers_notified;
    bool done;
    bool cancelled;

    vox_string_t* cur_h_name;
    vox_string_t* cur_h_value;

    vox_string_t* out; /* request bytes */

    /* gzip 支持 */
    bool is_gzip_encoded;        /* 响应是否使用 gzip 压缩 */
    vox_string_t* compressed_body; /* 收集压缩的响应体（用于解压缩） */

    bool response_connection_close; /* 响应头 Connection: close 时置为 true，用于决定是否关闭连接 */

    vox_timer_t connect_timer;     /* 连接超时定时器（仅 connection_timeout_ms > 0 时使用） */

    vox_http_client_callbacks_t cbs;
    void* user_data;
};

static const char* method_to_cstr(vox_http_method_t m) {
    switch (m) {
        case VOX_HTTP_METHOD_GET: return "GET";
        case VOX_HTTP_METHOD_HEAD: return "HEAD";
        case VOX_HTTP_METHOD_POST: return "POST";
        case VOX_HTTP_METHOD_PUT: return "PUT";
        case VOX_HTTP_METHOD_DELETE: return "DELETE";
        case VOX_HTTP_METHOD_CONNECT: return "CONNECT";
        case VOX_HTTP_METHOD_OPTIONS: return "OPTIONS";
        case VOX_HTTP_METHOD_TRACE: return "TRACE";
        case VOX_HTTP_METHOD_PATCH: return "PATCH";
        default: return "GET";
    }
}

static inline int ci_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    return strcasecmp(a, b) == 0;
}

/* 判断 value 是否包含 token "close"（忽略首尾 OWS 与 \r\n，大小写不敏感；支持 "close" 或 "close, ..."） */
static int value_is_close(const char* v, size_t vlen) {
    if (!v) return 0;
    while (vlen > 0 && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) { v++; vlen--; }
    while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t' || v[vlen - 1] == '\r' || v[vlen - 1] == '\n')) vlen--;
    if (vlen < 5) return 0;
    /* 匹配 "close" 或 "close" 后跟逗号/空白 */
    if (strncasecmp(v, "close", 5) != 0) return 0;
    if (vlen == 5) return 1;
    return (v[5] == ',' || v[5] == ' ' || v[5] == '\t' || v[5] == '\r' || v[5] == '\n');
}

static void req_fail(vox_http_client_req_t* req, const char* msg);

static void req_close_transport(vox_http_client_req_t* req) {
    if (!req) return;
    if (req->is_tls) {
        if (req->tls && !vox_handle_is_closing((vox_handle_t*)req->tls)) {
            vox_handle_close((vox_handle_t*)req->tls, NULL);
        }
    } else {
        if (req->tcp && !vox_handle_is_closing((vox_handle_t*)req->tcp)) {
            vox_handle_close((vox_handle_t*)req->tcp, NULL);
        }
    }
}

static int url_copy_part(vox_mpool_t* mpool, const char* src, size_t len, char** out_cstr) {
    if (!mpool || !out_cstr) return -1;
    char* p = (char*)vox_mpool_alloc(mpool, len + 1);
    if (!p) return -1;
    memcpy(p, src, len);
    p[len] = '\0';
    *out_cstr = p;
    return 0;
}

static int parse_url(vox_mpool_t* mpool, const char* url, vox_http_client_url_t* out) {
    if (!mpool || !url || !out) return -1;
    memset(out, 0, sizeof(*out));

    const char* p = url;
    if (strncmp(p, "http://", 7) == 0) {
        out->scheme = VOX_HTTP_CLIENT_SCHEME_HTTP;
        out->port = 80;
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        out->scheme = VOX_HTTP_CLIENT_SCHEME_HTTPS;
        out->port = 443;
        p += 8;
    } else {
        return -1;
    }

    /* host[:port][/path][?query] */
    const char* host_start = p;
    const char* host_end = NULL;

    if (*p == '[') {
        /* IPv6: [::1] */
        const char* rb = strchr(p, ']');
        if (!rb) return -1;
        host_start = p + 1;
        host_end = rb;
        p = rb + 1;
    } else {
        while (*p && *p != ':' && *p != '/' && *p != '?' && *p != '#') p++;
        host_end = p;
    }

    if (!host_end || host_end <= host_start) return -1;
    if (url_copy_part(mpool, host_start, (size_t)(host_end - host_start), &out->host) != 0) return -1;

    /* port */
    if (*p == ':') {
        p++;
        uint32_t port = 0;
        const char* port_start = p;
        while (*p >= '0' && *p <= '9') {
            port = port * 10u + (uint32_t)(*p - '0');
            if (port > 65535u) return -1;
            p++;
        }
        if (p == port_start) return -1;
        out->port = (uint16_t)port;
    }

    /* path + query + fragment 按照 RFC 3986 解析
     * URL 结构: scheme://host:port/path?query#fragment
     * fragment 应该被忽略（不发送给服务器）
     */
    const char* path_start = NULL;
    const char* path_end = NULL;
    
    /* 确定 path 起始位置 */
    if (*p == '/') {
        /* 正常 path */
        path_start = p;
    } else if (*p == '?') {
        /* 没有 path，只有 query，默认路径为 / */
        path_start = p;  /* 稍后处理 */
    } else if (*p == '#') {
        /* 没有 path 和 query，只有 fragment，默认路径为 / */
        if (url_copy_part(mpool, "/", 1, &out->path) != 0) return -1;
        return 0;
    } else if (*p == '\0') {
        /* 没有 path/query/fragment，默认路径为 / */
        if (url_copy_part(mpool, "/", 1, &out->path) != 0) return -1;
        return 0;
    } else {
        /* 非法字符 */
        return -1;
    }

    /* 查找 path + query 的结束位置（fragment 之前） */
    path_end = path_start;
    while (*path_end && *path_end != '#') {
        path_end++;
    }
    
    /* 如果 path_end <= path_start，说明没有实际内容 */
    if (path_end <= path_start) {
        if (url_copy_part(mpool, "/", 1, &out->path) != 0) return -1;
        return 0;
    }

    /* 处理没有 path 只有 query 的情况: "?query" */
    if (*path_start == '?') {
        size_t qlen = (size_t)(path_end - path_start);
        char* tmp = (char*)vox_mpool_alloc(mpool, qlen + 2);
        if (!tmp) return -1;
        tmp[0] = '/';  /* 前置 / */
        memcpy(tmp + 1, path_start, qlen);
        tmp[qlen + 1] = '\0';
        out->path = tmp;
        return 0;
    }

    /* 正常 path[?query] */
    size_t path_len = (size_t)(path_end - path_start);
    if (url_copy_part(mpool, path_start, path_len, &out->path) != 0) return -1;
    
    /* 确保 path 以 / 开头 */
    if (out->path[0] != '/') {
        size_t len = strlen(out->path);
        char* tmp = (char*)vox_mpool_alloc(mpool, len + 2);
        if (!tmp) return -1;
        tmp[0] = '/';
        memcpy(tmp + 1, out->path, len);
        tmp[len + 1] = '\0';
        out->path = tmp;
    }
    
    return 0;
}

static int build_request_bytes(vox_http_client_req_t* req) {
    if (!req) return -1;
    if (!req->out) req->out = vox_string_create(req->mpool);
    if (!req->out) return -1;
    vox_string_clear(req->out);

    const char* m = method_to_cstr(req->request.method);
    const char* path = (req->url.path && req->url.path[0]) ? req->url.path : "/";

    if (vox_string_append_format(req->out, "%s %s HTTP/1.1\r\n", m, path) < 0) return -1;

    /* 默认头：Host/Connection/User-Agent/Accept/Accept-Encoding */
    bool has_host = false;
    bool has_conn = false;
    bool has_cl = false;
    bool has_accept_encoding = false;

    if (req->request.headers && req->request.header_count > 0) {
        for (size_t i = 0; i < req->request.header_count; i++) {
            const vox_http_client_header_t* h = &req->request.headers[i];
            if (!h->name || !h->value) continue;
            if (ci_eq(h->name, "Host")) has_host = true;
            if (ci_eq(h->name, "Connection")) has_conn = true;
            if (ci_eq(h->name, "Content-Length")) has_cl = true;
            if (ci_eq(h->name, "Accept-Encoding")) has_accept_encoding = true;
        }
    }

    if (!has_host) {
        if (vox_string_append_format(req->out, "Host: %s\r\n", req->url.host) < 0) return -1;
    }
    if (!has_conn) {
        if (vox_string_append(req->out, "Connection: close\r\n") != 0) return -1;
    }
    if (vox_string_append(req->out, "User-Agent: voxlib\r\n") != 0) return -1;
    if (vox_string_append(req->out, "Accept: */*\r\n") != 0) return -1;
#ifdef VOX_USE_ZLIB
    if (!has_accept_encoding) {
        if (vox_string_append(req->out, "Accept-Encoding: gzip\r\n") != 0) return -1;
    }
#endif /* VOX_USE_ZLIB */

    if (req->request.body && req->request.body_len > 0 && !has_cl) {
        if (vox_string_append_format(req->out, "Content-Length: %zu\r\n", req->request.body_len) < 0) return -1;
    }

    /* 用户头 */
    if (req->request.headers && req->request.header_count > 0) {
        for (size_t i = 0; i < req->request.header_count; i++) {
            const vox_http_client_header_t* h = &req->request.headers[i];
            if (!h->name || !h->value) continue;
            if (vox_string_append_format(req->out, "%s: %s\r\n", h->name, h->value) < 0) return -1;
        }
    }

    if (vox_string_append(req->out, "\r\n") != 0) return -1;
    if (req->request.body && req->request.body_len > 0) {
        if (vox_string_append_data(req->out, req->request.body, req->request.body_len) != 0) return -1;
    }
    return 0;
}

/* ===== HTTP parser callbacks (response) ===== */

static int on_message_begin(void* p) {
    vox_http_parser_t* parser = (vox_http_parser_t*)p;
    vox_http_client_req_t* req = (vox_http_client_req_t*)vox_http_parser_get_user_data(parser);
    if (!req || req->done) return -1;
    req->headers_notified = false;
    req->is_gzip_encoded = false;
    req->response_connection_close = false;
    vox_string_clear(req->cur_h_name);
    vox_string_clear(req->cur_h_value);
    if (req->compressed_body) {
        vox_string_clear(req->compressed_body);
    }
    return 0;
}

static int commit_header_if_any(vox_http_client_req_t* req) {
    if (!req) return 0;
    size_t nlen = vox_string_length(req->cur_h_name);
    if (nlen == 0) return 0;
    size_t vlen = vox_string_length(req->cur_h_value);

    /* 拷贝到 mpool，保证回调拿到的数据稳定（直到请求结束） */
    const char* nsrc = vox_string_cstr(req->cur_h_name);
    const char* vsrc = vox_string_cstr(req->cur_h_value);
    char* ncopy = (char*)vox_mpool_alloc(req->mpool, nlen + 1);
    char* vcopy = (char*)vox_mpool_alloc(req->mpool, vlen + 1);
    if (!ncopy || !vcopy) return -1;
    memcpy(ncopy, nsrc, nlen);
    ncopy[nlen] = '\0';
    memcpy(vcopy, vsrc, vlen);
    vcopy[vlen] = '\0';

    /* 检查是否是 gzip 编码 */
#ifdef VOX_USE_ZLIB
    if (vox_http_is_gzip_encoded(ncopy, nlen, vcopy, vlen)) {
        req->is_gzip_encoded = true;
        /* 创建缓冲区用于收集压缩的响应体 */
        if (!req->compressed_body) {
            req->compressed_body = vox_string_create(req->mpool);
            if (!req->compressed_body) return -1;
        }
    }
#endif /* VOX_USE_ZLIB */

    /* 根据响应头 Connection: close 决定是否在响应完成后关闭连接 */
    if (ci_eq(ncopy, "Connection") && value_is_close(vcopy, vlen)) {
        req->response_connection_close = true;
    }

    vox_strview_t name = { ncopy, nlen };
    vox_strview_t value = { vcopy, vlen };
    if (req->cbs.on_header) {
        req->cbs.on_header(req, name, value, req->user_data);
    }
    vox_string_clear(req->cur_h_name);
    vox_string_clear(req->cur_h_value);
    return 0;
}

static int on_header_field(void* p, const char* data, size_t len) {
    vox_http_parser_t* parser = (vox_http_parser_t*)p;
    vox_http_client_req_t* req = (vox_http_client_req_t*)vox_http_parser_get_user_data(parser);
    if (!req || req->done) return -1;
    if (len == 0) return 0;

    /* 若已有 value，说明进入了新 header */
    if (vox_string_length(req->cur_h_value) > 0) {
        if (commit_header_if_any(req) != 0) return -1;
    }
    return vox_string_append_data(req->cur_h_name, data, len);
}

static int on_header_value(void* p, const char* data, size_t len) {
    vox_http_parser_t* parser = (vox_http_parser_t*)p;
    vox_http_client_req_t* req = (vox_http_client_req_t*)vox_http_parser_get_user_data(parser);
    if (!req || req->done) return -1;
    if (len == 0) return 0;
    return vox_string_append_data(req->cur_h_value, data, len);
}

static int on_status(void* p, const char* data, size_t len) {
    (void)data;
    (void)len;
    vox_http_parser_t* parser = (vox_http_parser_t*)p;
    vox_http_client_req_t* req = (vox_http_client_req_t*)vox_http_parser_get_user_data(parser);
    if (!req || req->done) return -1;
    int status_code = vox_http_parser_get_status_code(parser);
    if (req->cbs.on_status) {
        req->cbs.on_status(req,
                           status_code,
                           vox_http_parser_get_http_major(parser),
                           vox_http_parser_get_http_minor(parser),
                           req->user_data);
    }
    return 0;
}

static int on_headers_complete(void* p) {
    vox_http_parser_t* parser = (vox_http_parser_t*)p;
    vox_http_client_req_t* req = (vox_http_client_req_t*)vox_http_parser_get_user_data(parser);
    if (!req || req->done) return -1;
    if (commit_header_if_any(req) != 0) return -1;
    req->headers_notified = true;
    if (req->cbs.on_headers_complete) {
        req->cbs.on_headers_complete(req, req->user_data);
    }
    return 0;
}

static int on_body(void* p, const char* data, size_t len) {
    vox_http_parser_t* parser = (vox_http_parser_t*)p;
    vox_http_client_req_t* req = (vox_http_client_req_t*)vox_http_parser_get_user_data(parser);
    if (!req || req->done) return -1;
    
#ifdef VOX_USE_ZLIB
    /* 如果响应是 gzip 压缩的，收集数据稍后解压缩 */
    if (req->is_gzip_encoded && req->compressed_body) {
        if (data && len > 0) {
            if (vox_string_append_data(req->compressed_body, data, len) != 0) {
                return -1;
            }
        }
        /* 不立即调用 on_body，等消息完成后再解压缩 */
        return 0;
    }
#endif /* VOX_USE_ZLIB */
    
    if (req->cbs.on_body && data && len > 0) {
        req->cbs.on_body(req, data, len, req->user_data);
    }
    return 0;
}

static int on_message_complete(void* p) {
    vox_http_parser_t* parser = (vox_http_parser_t*)p;
    vox_http_client_req_t* req = (vox_http_client_req_t*)vox_http_parser_get_user_data(parser);
    if (!req || req->done) return -1;
    
#ifdef VOX_USE_ZLIB
    /* 如果响应是 gzip 压缩的，解压缩并调用 on_body 回调 */
    if (req->is_gzip_encoded && req->compressed_body) {
        size_t compressed_len = vox_string_length(req->compressed_body);
        if (compressed_len > 0) {
            vox_string_t* decompressed = vox_string_create(req->mpool);
            if (decompressed) {
                if (vox_http_gzip_decompress(req->mpool,
                                              vox_string_data(req->compressed_body),
                                              compressed_len,
                                              decompressed) == 0) {
                    /* 解压缩成功，调用 on_body 回调 */
                    size_t decompressed_len = vox_string_length(decompressed);
                    if (decompressed_len > 0 && req->cbs.on_body) {
                        req->cbs.on_body(req, vox_string_data(decompressed), decompressed_len, req->user_data);
                    }
                    vox_string_destroy(decompressed);
                } else {
                    /* 解压缩失败，使用原始压缩数据 */
                    if (req->cbs.on_body) {
                        req->cbs.on_body(req, vox_string_data(req->compressed_body), compressed_len, req->user_data);
                    }
                    vox_string_destroy(decompressed);
                }
            }
        }
    }
#endif /* VOX_USE_ZLIB */
    
    req->done = true;
    if (req->dns_req) {
        if (req->dns_pending) {
            vox_dns_getaddrinfo_cancel(req->dns_req);
            req->dns_pending = false;
        }
        vox_dns_getaddrinfo_destroy(req->dns_req);
        req->dns_req = NULL;
    }
    if (req->cbs.on_complete) {
        req->cbs.on_complete(req, 0, req->user_data);
    }
    /* 仅当响应头 Connection 等于 close 时关闭连接 */
    if (req->response_connection_close) {
        req_close_transport(req);
    }
    return 0;
}

static int on_parse_error(void* p, const char* message) {
    vox_http_parser_t* parser = (vox_http_parser_t*)p;
    vox_http_client_req_t* req = (vox_http_client_req_t*)vox_http_parser_get_user_data(parser);
    if (!req || req->done) return -1;
    req_fail(req, message ? message : "http parse error");
    return 0;
}

static void on_connect_timeout_cb(vox_timer_t* timer, void* user_data) {
    (void)timer;
    vox_http_client_req_t* req = (vox_http_client_req_t*)user_data;
    if (!req || req->done || req->cancelled) return;
    req_fail(req, "connection timeout");
}

static void req_fail(vox_http_client_req_t* req, const char* msg) {
    if (!req || req->done) return;
    req->done = true;
    if (vox_timer_is_active(&req->connect_timer)) {
        vox_timer_stop(&req->connect_timer);
    }
    if (req->dns_req) {
        if (req->dns_pending) {
            vox_dns_getaddrinfo_cancel(req->dns_req);
            req->dns_pending = false;
        }
        vox_dns_getaddrinfo_destroy(req->dns_req);
        req->dns_req = NULL;
    }
    if (req->cbs.on_error) {
        req->cbs.on_error(req, msg ? msg : "error", req->user_data);
    }
    req_close_transport(req);
}

/* ===== transport callbacks ===== */

static void tcp_write_cb(vox_tcp_t* tcp, int status, void* user_data);
static void tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data);
static void tcp_connect_cb(vox_tcp_t* tcp, int status, void* user_data);

static void tls_write_cb(vox_tls_t* tls, int status, void* user_data);
static void tls_read_cb(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data);
static void tls_connect_cb(vox_tls_t* tls, int status, void* user_data);

static int feed_parser(vox_http_client_req_t* req, const char* data, size_t len) {
    size_t off = 0;
    while (off < len && !req->done) {
        ssize_t n = vox_http_parser_execute(req->parser, data + off, len - off);
        if (n < 0) {
            req_fail(req, vox_http_parser_get_error(req->parser));
            return -1;
        }
        if (n == 0) break;
        off += (size_t)n;
        if (vox_http_parser_is_complete(req->parser)) {
            break;
        }
    }
    return 0;
}

static void tcp_connect_cb(vox_tcp_t* tcp, int status, void* user_data) {
    (void)tcp;
    vox_http_client_req_t* req = (vox_http_client_req_t*)user_data;
    if (!req || req->done || req->cancelled) return;
    if (status != 0) {
        req_fail(req, "tcp connect failed");
        return;
    }
    if (vox_timer_is_active(&req->connect_timer)) {
        vox_timer_stop(&req->connect_timer);
    }
    if (req->cbs.on_connect) req->cbs.on_connect(req, req->user_data);

    const void* buf = vox_string_data(req->out);
    size_t blen = vox_string_length(req->out);
    if (vox_tcp_write(req->tcp, buf, blen, tcp_write_cb) != 0) {
        req_fail(req, "tcp write failed");
        return;
    }
}

static void tcp_write_cb(vox_tcp_t* tcp, int status, void* user_data) {
    (void)tcp;
    vox_http_client_req_t* req = (vox_http_client_req_t*)user_data;
    if (!req || req->done || req->cancelled) return;
    if (status != 0) {
        req_fail(req, "tcp write callback error");
        return;
    }
    if (vox_tcp_read_start(req->tcp, NULL, tcp_read_cb) != 0) {
        req_fail(req, "tcp read_start failed");
        return;
    }
}

static void tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    (void)tcp;
    vox_http_client_req_t* req = (vox_http_client_req_t*)user_data;
    if (!req || req->done || req->cancelled) return;

    if (nread < 0) {
        req_fail(req, "tcp read error");
        return;
    }
    if (nread == 0) {
        /* EOF：用于响应体 identity-eof */
        (void)vox_http_parser_execute(req->parser, NULL, 0);
        if (!req->done) {
            req_fail(req, "connection closed");
        }
        return;
    }
    feed_parser(req, (const char*)buf, (size_t)nread);
}

static void tls_connect_cb(vox_tls_t* tls, int status, void* user_data) {
    (void)tls;
    vox_http_client_req_t* req = (vox_http_client_req_t*)user_data;
    if (!req || req->done || req->cancelled) return;
    if (status != 0) {
        req_fail(req, "tls connect/handshake failed");
        return;
    }
    if (vox_timer_is_active(&req->connect_timer)) {
        vox_timer_stop(&req->connect_timer);
    }
    if (req->cbs.on_connect) req->cbs.on_connect(req, req->user_data);

    const void* buf = vox_string_data(req->out);
    size_t blen = vox_string_length(req->out);
    if (vox_tls_write(req->tls, buf, blen, tls_write_cb) != 0) {
        req_fail(req, "tls write failed");
        return;
    }
}

static void tls_write_cb(vox_tls_t* tls, int status, void* user_data) {
    (void)tls;
    vox_http_client_req_t* req = (vox_http_client_req_t*)user_data;
    if (!req || req->done || req->cancelled) return;
    if (status != 0) {
        req_fail(req, "tls write callback error");
        return;
    }
    if (vox_tls_read_start(req->tls, NULL, tls_read_cb) != 0) {
        req_fail(req, "tls read_start failed");
        return;
    }
}

static void tls_read_cb(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data) {
    (void)tls;
    vox_http_client_req_t* req = (vox_http_client_req_t*)user_data;
    if (!req || req->done || req->cancelled) return;

    if (nread < 0) {
        req_fail(req, "tls read error");
        return;
    }
    if (nread == 0) {
        (void)vox_http_parser_execute(req->parser, NULL, 0);
        if (!req->done) {
            req_fail(req, "connection closed");
        }
        return;
    }
    feed_parser(req, (const char*)buf, (size_t)nread);
}

/* ===== DNS callback ===== */

static void dns_cb(vox_dns_getaddrinfo_t* dns, int status, const vox_dns_addrinfo_t* addrinfo, void* user_data) {
    (void)dns;
    vox_http_client_req_t* req = (vox_http_client_req_t*)user_data;
    if (!req || req->done || req->cancelled) return;
    req->dns_pending = false;
    if (status != 0 || !addrinfo || addrinfo->count == 0) {
        req_fail(req, "dns resolve failed");
        return;
    }

    /* 先拷贝地址再销毁 dns_req（destroy 会释放 addrinfo->addrs） */
    vox_socket_addr_t addr = addrinfo->addrs[0];
    if (req->dns_req) {
        vox_dns_getaddrinfo_destroy(req->dns_req);
        req->dns_req = NULL;
    }

    if (req->is_tls) {
        if (vox_tls_connect(req->tls, &addr, tls_connect_cb) != 0) {
            req_fail(req, "tls connect start failed");
            return;
        }
    } else {
        if (vox_tcp_connect(req->tcp, &addr, tcp_connect_cb) != 0) {
            req_fail(req, "tcp connect start failed");
            return;
        }
    }
}

vox_http_client_t* vox_http_client_create(vox_loop_t* loop) {
    if (!loop) return NULL;
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_http_client_t* c = (vox_http_client_t*)vox_mpool_alloc(mpool, sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->loop = loop;
    c->mpool = mpool;
    return c;
}

void vox_http_client_destroy(vox_http_client_t* client) {
    (void)client;
    /* client 自身来自 loop mpool：无需显式 free；调用者通常随 loop 一起回收 */
}

static vox_http_client_req_t* req_create(vox_http_client_t* client) {
    vox_http_client_req_t* req = (vox_http_client_req_t*)vox_mpool_alloc(client->mpool, sizeof(*req));
    if (!req) return NULL;
    memset(req, 0, sizeof(*req));
    req->client = client;
    req->loop = client->loop;
    req->mpool = client->mpool;

    req->cur_h_name = vox_string_create(req->mpool);
    req->cur_h_value = vox_string_create(req->mpool);
    req->out = vox_string_create(req->mpool);
    req->compressed_body = vox_string_create(req->mpool);
    if (!req->cur_h_name || !req->cur_h_value || !req->out || !req->compressed_body) return NULL;
    req->is_gzip_encoded = false;

    vox_http_parser_config_t cfg = {0};
    cfg.type = VOX_HTTP_PARSER_TYPE_RESPONSE;
    cfg.max_header_size = 0;
    cfg.max_headers = 0;
    cfg.max_url_size = 0;
    cfg.strict_mode = false;

    vox_http_callbacks_t pcbs = {0};
    pcbs.on_message_begin = on_message_begin;
    pcbs.on_status = on_status;
    pcbs.on_header_field = on_header_field;
    pcbs.on_header_value = on_header_value;
    pcbs.on_headers_complete = on_headers_complete;
    pcbs.on_body = on_body;
    pcbs.on_message_complete = on_message_complete;
    pcbs.on_error = on_parse_error;
    pcbs.user_data = req;

    req->parser = vox_http_parser_create(req->mpool, &cfg, &pcbs);
    if (!req->parser) return NULL;

    if (vox_timer_init(&req->connect_timer, req->loop) != 0) {
        return NULL;
    }
    return req;
}

int vox_http_client_request(vox_http_client_t* client,
                            const vox_http_client_request_t* request,
                            const vox_http_client_callbacks_t* cbs,
                            void* user_data,
                            vox_http_client_req_t** out_req) {
    if (!client || !request || !request->url) return -1;

    vox_http_client_req_t* req = req_create(client);
    if (!req) return -1;
    if (cbs) memcpy(&req->cbs, cbs, sizeof(req->cbs));
    req->user_data = user_data;

    /* copy request struct */
    req->request = *request;

    if (parse_url(req->mpool, request->url, &req->url) != 0) {
        req_fail(req, "invalid url");
        return -1;
    }
    req->is_tls = (req->url.scheme == VOX_HTTP_CLIENT_SCHEME_HTTPS);

    if (build_request_bytes(req) != 0) {
        req_fail(req, "build request failed");
        return -1;
    }

    /* init transport */
    if (req->is_tls) {
        req->tls = vox_tls_create(req->loop, request->ssl_ctx);
        if (!req->tls) {
            req_fail(req, "tls create failed");
            return -1;
        }
        vox_handle_set_data((vox_handle_t*)req->tls, req);
    } else {
        req->tcp = vox_tcp_create(req->loop);
        if (!req->tcp) {
            req_fail(req, "tcp create failed");
            return -1;
        }
        vox_handle_set_data((vox_handle_t*)req->tcp, req);
    }

    /* dns resolve */
    req->dns_req = vox_dns_getaddrinfo_create(req->loop);
    if (!req->dns_req) {
        req_fail(req, "dns req create failed");
        return -1;
    }

    char port_str[16];
    (void)snprintf(port_str, sizeof(port_str), "%u", (unsigned)req->url.port);
    req->dns_pending = true;
    if (vox_dns_getaddrinfo(req->dns_req, req->url.host, port_str, 0, dns_cb, req, 5000) != 0) {
        req->dns_pending = false;
        req_fail(req, "dns getaddrinfo start failed");
        return -1;
    }

    /* 连接超时：覆盖 DNS + TCP/TLS 建立 */
    if (request->connection_timeout_ms > 0) {
        if (vox_timer_start(&req->connect_timer, (uint64_t)request->connection_timeout_ms, 0, on_connect_timeout_cb, req) != 0) {
            req_fail(req, "connection timeout timer start failed");
            return -1;
        }
    }

    if (out_req) *out_req = req;
    return 0;
}

void vox_http_client_cancel(vox_http_client_req_t* req) {
    if (!req || req->done) return;
    req->cancelled = true;
    req->done = true;

    if (vox_timer_is_active(&req->connect_timer)) {
        vox_timer_stop(&req->connect_timer);
    }
    if (req->dns_req) {
        if (req->dns_pending) {
            vox_dns_getaddrinfo_cancel(req->dns_req);
            req->dns_pending = false;
        }
        vox_dns_getaddrinfo_destroy(req->dns_req);
        req->dns_req = NULL;
    }

    if (req->cbs.on_error) {
        req->cbs.on_error(req, "cancelled", req->user_data);
    }

    req_close_transport(req);
}

void vox_http_client_close(vox_http_client_req_t* req) {
    if (!req) return;
    req_close_transport(req);
}

