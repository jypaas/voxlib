/*
 * vox_http_parser.c - 高性能 HTTP 解析器实现
 * 使用 vox_scanner 流式解析：内部缓冲累积输入，vox_scanner 驱动零拷贝解析
 */

#include "vox_http_parser.h"
#include "../vox_scanner.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>

/* 默认缓冲区初始容量 */
#define VOX_HTTP_PARSER_BUF_INIT 4096
#define VOX_HTTP_PARSER_BUF_MAX  (1024 * 1024)

/* 解析阶段 */
typedef enum {
    VOX_HTTP_PHASE_INIT = 0,
    VOX_HTTP_PHASE_START_LINE,
    VOX_HTTP_PHASE_HEADER_NAME,
    VOX_HTTP_PHASE_HEADER_VALUE,
    VOX_HTTP_PHASE_HEADERS_DONE,
    VOX_HTTP_PHASE_BODY,
    VOX_HTTP_PHASE_CHUNK_SIZE,
    VOX_HTTP_PHASE_CHUNK_DATA,
    VOX_HTTP_PHASE_CHUNK_END,
    VOX_HTTP_PHASE_MESSAGE_COMPLETE,
    VOX_HTTP_PHASE_ERROR
} vox_http_phase_t;

/* 内部解析器结构 */
struct vox_http_parser {
    vox_mpool_t* mpool;
    vox_http_parser_config_t config;
    vox_http_callbacks_t callbacks;
    void* user_data;

    /* 内部缓冲：有效区间 [buf_off, buf_off+buf_size)，延迟 memmove */
    char* buf;
    size_t buf_off;
    size_t buf_size;
    size_t buf_capacity;

    vox_scanner_stream_t stream;  /* 流式扫描器 */
    vox_scanner_t* sc;             /* 当前可用的 scanner 视图 */

    vox_http_phase_t phase;
    bool message_complete;
    bool has_error;
    char error_msg[128];

    /* 解析结果 */
    vox_http_method_t method;
    int http_major;
    int http_minor;
    int status_code;
    uint64_t content_length;
    uint64_t body_read;
    bool chunked;
    uint64_t chunk_remaining;      /* chunked 剩余字节 */
    bool connection_close;
    bool connection_keepalive;
    bool upgrade;

    /* 当前头部：name/value 可能分多段 */
    size_t header_count;
};

/* 已知方法名 */
static const char* const method_names[] = {
    "GET", "HEAD", "POST", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE", "PATCH"
};
static const size_t method_count = sizeof(method_names) / sizeof(method_names[0]);

static inline vox_http_method_t parse_method(const char* ptr, size_t len) {
    for (size_t i = 0; i < method_count; i++) {
        size_t n = strlen(method_names[i]);
        if (len == n && (len == 0 || strncmp(ptr, method_names[i], n) == 0))
            return (vox_http_method_t)(VOX_HTTP_METHOD_GET + (int)i);
    }
    return VOX_HTTP_METHOD_UNKNOWN;
}

static void set_error(vox_http_parser_t* p, const char* msg) {
    p->has_error = true;
    p->phase = VOX_HTTP_PHASE_ERROR;
    if (msg) {
        size_t n = strlen(msg);
        if (n >= sizeof(p->error_msg)) n = sizeof(p->error_msg) - 1;
        memcpy(p->error_msg, msg, n);
        p->error_msg[n] = '\0';
    } else {
        p->error_msg[0] = '\0';
    }
}

static inline int invoke_message_begin(vox_http_parser_t* p) {
    if (!p->callbacks.on_message_begin) return 0;
    return p->callbacks.on_message_begin(p);
}

static inline int invoke_url(vox_http_parser_t* p, const char* data, size_t len) {
    if (!p->callbacks.on_url || len == 0) return 0;
    return p->callbacks.on_url(p, data, len);
}

static inline int invoke_status(vox_http_parser_t* p, const char* data, size_t len) {
    if (!p->callbacks.on_status || len == 0) return 0;
    return p->callbacks.on_status(p, data, len);
}

/* 去掉首尾 OWS（RFC 7230），解析器内统一处理，回调得到的是已 trim 的 field/value */
static inline void trim_ows(const char** ptr, size_t* len) {
    if (!ptr || !len || !*ptr) return;
    while (*len > 0 && (**ptr == ' ' || **ptr == '\t')) { (*ptr)++; (*len)--; }
    while (*len > 0 && ((*ptr)[*len - 1] == ' ' || (*ptr)[*len - 1] == '\t')) (*len)--;
}

static inline int invoke_header_field(vox_http_parser_t* p, const char* data, size_t len) {
    if (!p->callbacks.on_header_field || len == 0) return 0;
    return p->callbacks.on_header_field(p, data, len);
}

static inline int invoke_header_value(vox_http_parser_t* p, const char* data, size_t len) {
    if (!p->callbacks.on_header_value || len == 0) return 0;
    return p->callbacks.on_header_value(p, data, len);
}

static inline int invoke_headers_complete(vox_http_parser_t* p) {
    if (!p->callbacks.on_headers_complete) return 0;
    return p->callbacks.on_headers_complete(p);
}

static inline int invoke_body(vox_http_parser_t* p, const char* data, size_t len) {
    if (!p->callbacks.on_body || len == 0) return 0;
    return p->callbacks.on_body(p, data, len);
}

static inline int invoke_message_complete(vox_http_parser_t* p) {
    if (!p->callbacks.on_message_complete) return 0;
    return p->callbacks.on_message_complete(p);
}

static inline int invoke_error(vox_http_parser_t* p, const char* msg) {
    if (!p->callbacks.on_error) return 0;
    return p->callbacks.on_error(p, msg);
}

/* 压缩缓冲：将 [buf_off, buf_off+buf_size) 移到 [0, buf_size)，buf_off=0 */
static void compact_buf(vox_http_parser_t* p) {
    if (p->buf_off == 0 || p->buf_size == 0) return;
    memmove(p->buf, p->buf + p->buf_off, p->buf_size);
    p->buf_off = 0;
}

/* 确保内部缓冲区至少有 need 字节可用（含结尾 \0） */
static int ensure_buf(vox_http_parser_t* p, size_t need) {
    size_t want = p->buf_off + p->buf_size + need + 1;
    if (want <= p->buf_capacity)
        return 0;
    if (p->buf_off > 0) {
        compact_buf(p);
        if (p->buf_size + need + 1 <= p->buf_capacity)
            return 0;
    }
    size_t new_cap = p->buf_capacity ? p->buf_capacity : VOX_HTTP_PARSER_BUF_INIT;
    while (new_cap < p->buf_size + need + 1) {
        size_t next = new_cap + (new_cap >> 1);
        if (next <= new_cap || next > VOX_HTTP_PARSER_BUF_MAX)
            return -1;
        new_cap = next;
    }
    char* new_buf = (char*)vox_mpool_alloc(p->mpool, new_cap);
    if (!new_buf) return -1;
    if (p->buf && p->buf_size)
        memcpy(new_buf, p->buf + p->buf_off, p->buf_size);
    if (p->buf)
        vox_mpool_free(p->mpool, p->buf);
    p->buf = new_buf;
    p->buf_off = 0;
    p->buf_capacity = new_cap;
    return 0;
}

/* 检查 Transfer-Encoding: chunked（大小写不敏感，允许前后空白） */
static inline bool header_value_is_chunked(const char* ptr, size_t len) {
    while (len && (*ptr == ' ' || *ptr == '\t')) { ptr++; len--; }
    while (len && (ptr[len-1] == ' ' || ptr[len-1] == '\t')) len--;
    if (len != 7) return false;
    return strncasecmp(ptr, "chunked", 7) == 0;
}

static inline bool header_name_is(const char* ptr, size_t len, const char* name) {
    size_t n = strlen(name);
    if (len != n) return false;
    return strncasecmp(ptr, name, n) == 0;
}

/* 解析 Content-Length 数值 */
static int parse_content_length(const char* ptr, size_t len, uint64_t* out) {
    *out = 0;
    while (len && (*ptr == ' ' || *ptr == '\t')) { ptr++; len--; }
    if (len == 0) return -1;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)ptr[i])) return -1;
        uint64_t prev = *out;
        *out = *out * 10 + (uint64_t)(ptr[i] - '0');
        if (*out < prev) return -1; /* overflow */
    }
    return 0;
}

/* 解析 chunk 大小（十六进制）；溢出或非十六进制返回 -1 */
static int parse_chunk_size(const char* ptr, size_t len, uint64_t* out) {
    *out = 0;
    while (len && (*ptr == ' ' || *ptr == '\t')) { ptr++; len--; }
    if (len == 0) return -1;
    for (size_t i = 0; i < len; i++) {
        char c = ptr[i];
        uint64_t digit;
        if (c >= '0' && c <= '9') digit = (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (uint64_t)(c - 'A' + 10);
        else return -1;
        /* 左移前检查，避免 uint64_t 溢出导致高位丢失 */
        if (*out > (UINT64_MAX >> 4)) return -1;
        uint64_t prev = *out;
        *out = (*out << 4) + digit;
        if (*out < prev) return -1;
    }
    return 0;
}

/* 应用头部：更新 content_length, chunked, connection, upgrade；无效 Content-Length 时返回 -1 并 set_error */
static int apply_header(vox_http_parser_t* p, const char* name_ptr, size_t name_len,
                         const char* value_ptr, size_t value_len) {
    if (header_name_is(name_ptr, name_len, "Content-Length")) {
        const char* v = value_ptr;
        size_t vl = value_len;
        while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
        if (vl == 0) return 0;
        if (*v == '-') {
            set_error(p, "invalid Content-Length");
            return -1;
        }
        for (size_t i = 0; i < vl; i++) {
            if (!isdigit((unsigned char)v[i])) {
                set_error(p, "invalid Content-Length");
                return -1;
            }
        }
        uint64_t cl = 0;
        if (parse_content_length(value_ptr, value_len, &cl) != 0) {
            set_error(p, "invalid Content-Length");
            return -1;
        }
        p->content_length = cl;
    } else if (header_name_is(name_ptr, name_len, "Transfer-Encoding")) {
        if (header_value_is_chunked(value_ptr, value_len))
            p->chunked = true;
    } else if (header_name_is(name_ptr, name_len, "Connection")) {
        while (value_len) {
            while (value_len && (*value_ptr == ' ' || *value_ptr == '\t' || *value_ptr == ',')) {
                value_ptr++; value_len--;
            }
            if (value_len >= 5 && strncasecmp(value_ptr, "close", 5) == 0) {
                p->connection_close = true;
                value_ptr += 5; value_len -= 5;
            } else if (value_len >= 10 && strncasecmp(value_ptr, "keep-alive", 10) == 0) {
                p->connection_keepalive = true;
                value_ptr += 10; value_len -= 10;
            } else
                break;
        }
    } else if (header_name_is(name_ptr, name_len, "Upgrade")) {
        p->upgrade = true;
    }
    return 0;
}

/* 检测是请求还是响应：首行以 "HTTP/" 开头为响应 */
static inline bool looks_like_response(const char* ptr, size_t len) {
    return len >= 5 && ptr[0] == 'H' && ptr[1] == 'T' && ptr[2] == 'T' && ptr[3] == 'P' && ptr[4] == '/';
}

/* 流式下可能 \r\n 被截断，用 peek_until_str 看是否有 \r\n，没有则返回 -2 */
static inline int peek_line(vox_scanner_t* sc, vox_strview_t* out) {
    if (vox_scanner_peek_until_str(sc, "\r\n", false, out) != 0)
        return -1;
    size_t rem = vox_scanner_remaining(sc);
    if (out->len + 2 > rem) return -2; /* 不完整 */
    return 0;
}

static int parse_start_line(vox_http_parser_t* p, vox_scanner_t* sc) {
    vox_strview_t line;
    if (peek_line(sc, &line) != 0)
        return -2; /* 需要更多数据或错误 */
    if (line.len == 0) {
        set_error(p, "empty start line");
        return -1;
    }
    if (p->config.type == VOX_HTTP_PARSER_TYPE_RESPONSE || looks_like_response(line.ptr, line.len)) {
        /* 响应: HTTP/1.x SP status SP reason */
        if (line.len < 12) {
            set_error(p, "invalid response line");
            return -1;
        }
        if (line.ptr[6] != '.' || line.len < 9) {
            set_error(p, "invalid HTTP version");
            return -1;
        }
        p->http_major = line.ptr[5] - '0';
        p->http_minor = line.ptr[7] - '0';
        if (p->http_major != 1 || (p->http_minor != 0 && p->http_minor != 1)) {
            set_error(p, "unsupported HTTP version");
            return -1;
        }
        const char* rest = line.ptr + 9;
        size_t rest_len = line.len - 9;
        while (rest_len && *rest == ' ') { rest++; rest_len--; }
        if (rest_len < 3) {
            set_error(p, "missing status code");
            return -1;
        }
        int code = 0;
        size_t i = 0;
        for (; i < rest_len && i < 3 && isdigit((unsigned char)rest[i]); i++)
            code = code * 10 + (rest[i] - '0');
        if (i == 0) {
            set_error(p, "invalid status code");
            return -1;
        }
        p->status_code = code;
        while (i < rest_len && rest[i] == ' ') i++;
        if (i < rest_len && p->callbacks.on_status)
            invoke_status(p, rest + i, rest_len - i);
    } else {
        /* 请求: METHOD SP URL SP HTTP/1.x */
        const char* cur = line.ptr;
        size_t left = line.len;
        size_t i = 0;
        while (i < left && cur[i] != ' ') i++;
        if (i == 0 || i >= left) {
            set_error(p, "invalid request line");
            return -1;
        }
        p->method = parse_method(cur, i);
        cur += i; left -= i;
        while (left && *cur == ' ') { cur++; left--; }
        if (left == 0) {
            set_error(p, "missing URL");
            return -1;
        }
        i = 0;
        while (i < left && cur[i] != ' ') i++;
        if (i == 0) {
            set_error(p, "missing URL");
            return -1;
        }
        if (p->config.max_url_size && i > p->config.max_url_size) {
            set_error(p, "URL too long");
            return -1;
        }
        invoke_url(p, cur, i);
        cur += i; left -= i;
        while (left && *cur == ' ') { cur++; left--; }
        if (left < 8 || cur[0] != 'H' || cur[1] != 'T' || cur[2] != 'T' || cur[3] != 'P' || cur[4] != '/') {
            set_error(p, "invalid HTTP version");
            return -1;
        }
        if (cur[6] != '.') {
            set_error(p, "invalid HTTP version");
            return -1;
        }
        p->http_major = cur[5] - '0';
        p->http_minor = cur[7] - '0';
        if (p->http_major != 1 || (p->http_minor != 0 && p->http_minor != 1)) {
            set_error(p, "unsupported HTTP version");
            return -1;
        }
    }
    /* 消费整行含 \r\n */
    vox_scanner_get_until_str(sc, "\r\n", true, &line);
    return 0;
}

/* 解析头部区：逐行 name: value，直到空行 */
static int parse_headers(vox_http_parser_t* p, vox_scanner_t* sc) {
    vox_strview_t line;
    const char* field_start = NULL;
    size_t field_len = 0;
    const char* value_start = NULL;
    size_t value_len = 0;

    for (;;) {
        if (peek_line(sc, &line) != 0)
            return -2;
        if (line.len == 0) {
            /* 空行：头部结束，先提交最后一条头部 */
            vox_scanner_skip(sc, 2); /* \r\n */
            if (field_start) {
                if (p->callbacks.on_header_field) invoke_header_field(p, field_start, field_len);
                if (p->callbacks.on_header_value && value_start) invoke_header_value(p, value_start, value_len);
                if (apply_header(p, field_start, field_len, value_start, value_len) != 0)
                    return -1;
            }
            return 0;
        }
        /* 行首为 LWS 表示续行（折叠到上一个 value），去掉续行尾部的 OWS 再回调 */
        if ((line.ptr[0] == ' ' || line.ptr[0] == '\t') && value_start) {
            const char* cont_ptr = line.ptr;
            size_t cont_len = line.len;
            while (cont_len > 0 && (cont_ptr[cont_len - 1] == ' ' || cont_ptr[cont_len - 1] == '\t'))
                cont_len--;
            if (p->callbacks.on_header_value && cont_len > 0)
                invoke_header_value(p, cont_ptr, cont_len);
            value_len += 1 + line.len; /* 含前导空白 */
            vox_scanner_get_until_str(sc, "\r\n", true, &line);
            continue;
        }
        /* 新头部：先提交上一条 */
        if (field_start) {
            if (p->callbacks.on_header_field) invoke_header_field(p, field_start, field_len);
            if (p->callbacks.on_header_value) invoke_header_value(p, value_start, value_len);
            if (apply_header(p, field_start, field_len, value_start, value_len) != 0)
                return -1;
            if (p->config.max_headers && p->header_count >= p->config.max_headers) {
                set_error(p, "too many headers");
                return -1;
            }
            p->header_count++;
        }
        /* 找 ': ' */
        size_t colon = 0;
        while (colon < line.len && line.ptr[colon] != ':') colon++;
        if (colon == 0 || colon >= line.len) {
            set_error(p, "invalid header line");
            return -1;
        }
        field_start = line.ptr;
        field_len = colon;
        trim_ows(&field_start, &field_len);
        value_start = line.ptr + colon + 1;
        value_len = line.len - (colon + 1);
        while (value_len && (*value_start == ' ' || *value_start == '\t')) {
            value_start++; value_len--;
        }
        size_t trim = value_len;
        while (trim && (value_start[trim-1] == ' ' || value_start[trim-1] == '\t')) trim--;
        value_len = trim;
        vox_scanner_get_until_str(sc, "\r\n", true, &line);
    }
}

/* 执行解析：从当前 scanner 位置解析，更新 phase 与 consumed；返回 0 正常，-1 错误，-2 需要更多数据 */
static int do_parse(vox_http_parser_t* p, size_t* consumed) {
    vox_scanner_t* sc = p->sc;
    if (!sc || vox_scanner_eof(sc)) {
        *consumed = 0;
        return -2;
    }
    size_t start_offset = vox_scanner_offset(sc);

    for (;;) {
        if (p->phase == VOX_HTTP_PHASE_INIT) {
            if (invoke_message_begin(p) != 0) { set_error(p, "callback error"); return -1; }
            p->phase = VOX_HTTP_PHASE_START_LINE;
        }
        if (p->phase == VOX_HTTP_PHASE_START_LINE) {
            int r = parse_start_line(p, sc);
            if (r == -2) { *consumed = vox_scanner_offset(sc) - start_offset; return -2; }
            if (r != 0) return -1;
            p->phase = VOX_HTTP_PHASE_HEADER_NAME;
            p->header_count = 0;
            continue;
        }
        if (p->phase == VOX_HTTP_PHASE_HEADER_NAME || p->phase == VOX_HTTP_PHASE_HEADER_VALUE) {
            int r = parse_headers(p, sc);
            if (r == -2) { *consumed = vox_scanner_offset(sc) - start_offset; return -2; }
            if (r != 0) return -1;
            if (invoke_headers_complete(p) != 0) { set_error(p, "callback error"); return -1; }
            p->phase = VOX_HTTP_PHASE_HEADERS_DONE;
            if (p->chunked)
                p->phase = VOX_HTTP_PHASE_CHUNK_SIZE;
            else if (p->content_length > 0)
                p->phase = VOX_HTTP_PHASE_BODY;
            else {
                p->phase = VOX_HTTP_PHASE_MESSAGE_COMPLETE;
                p->message_complete = true;
                if (invoke_message_complete(p) != 0) { set_error(p, "callback error"); return -1; }
                *consumed = vox_scanner_offset(sc) - start_offset;
                return 0;
            }
            continue;
        }
        if (p->phase == VOX_HTTP_PHASE_BODY) {
            size_t rem = vox_scanner_remaining(sc);
            uint64_t need = p->content_length - p->body_read;
            if (need == 0) {
                p->phase = VOX_HTTP_PHASE_MESSAGE_COMPLETE;
                p->message_complete = true;
                if (invoke_message_complete(p) != 0) { set_error(p, "callback error"); return -1; }
                *consumed = vox_scanner_offset(sc) - start_offset;
                return 0;
            }
            if (rem == 0) { *consumed = vox_scanner_offset(sc) - start_offset; return -2; }
            size_t take = (size_t)(need < (uint64_t)rem ? need : (uint64_t)rem);
            vox_strview_t seg;
            if (vox_scanner_get(sc, take, &seg) != 0) { *consumed = vox_scanner_offset(sc) - start_offset; return -2; }
            if (invoke_body(p, seg.ptr, seg.len) != 0) { set_error(p, "callback error"); return -1; }
            p->body_read += seg.len;
            if (p->body_read >= p->content_length) {
                p->phase = VOX_HTTP_PHASE_MESSAGE_COMPLETE;
                p->message_complete = true;
                if (invoke_message_complete(p) != 0) { set_error(p, "callback error"); return -1; }
                *consumed = vox_scanner_offset(sc) - start_offset;
                return 0;
            }
            continue;
        }
        if (p->phase == VOX_HTTP_PHASE_CHUNK_SIZE) {
            vox_strview_t line;
            if (peek_line(sc, &line) != 0) {
                *consumed = vox_scanner_offset(sc) - start_offset;
                return -2;
            }
            uint64_t chunk_size = 0;
            if (parse_chunk_size(line.ptr, line.len, &chunk_size) != 0) {
                set_error(p, "invalid chunk size");
                return -1;
            }
            vox_scanner_get_until_str(sc, "\r\n", true, &line);
            p->chunk_remaining = chunk_size;
            if (chunk_size == 0) {
                /* 最后一个 chunk 0，后面可能有 trailer，跳过直到空行 */
                while (peek_line(sc, &line) == 0 && line.len > 0)
                    vox_scanner_get_until_str(sc, "\r\n", true, &line);
                if (vox_scanner_remaining(sc) >= 2)
                    vox_scanner_skip(sc, 2);
                p->phase = VOX_HTTP_PHASE_MESSAGE_COMPLETE;
                p->message_complete = true;
                if (invoke_message_complete(p) != 0) { set_error(p, "callback error"); return -1; }
                *consumed = vox_scanner_offset(sc) - start_offset;
                return 0;
            }
            p->phase = VOX_HTTP_PHASE_CHUNK_DATA;
            continue;
        }
        if (p->phase == VOX_HTTP_PHASE_CHUNK_DATA) {
            size_t rem = vox_scanner_remaining(sc);
            if (p->chunk_remaining == 0) {
                p->phase = VOX_HTTP_PHASE_CHUNK_END;
                continue;
            }
            if (rem == 0) { *consumed = vox_scanner_offset(sc) - start_offset; return -2; }
            size_t take = (size_t)(p->chunk_remaining < (uint64_t)rem ? p->chunk_remaining : (uint64_t)rem);
            vox_strview_t seg;
            if (vox_scanner_get(sc, take, &seg) != 0) { *consumed = vox_scanner_offset(sc) - start_offset; return -2; }
            if (invoke_body(p, seg.ptr, seg.len) != 0) { set_error(p, "callback error"); return -1; }
            p->chunk_remaining -= take;
            if (p->chunk_remaining == 0)
                p->phase = VOX_HTTP_PHASE_CHUNK_END;
            continue;
        }
        if (p->phase == VOX_HTTP_PHASE_CHUNK_END) {
            if (vox_scanner_remaining(sc) < 2) { *consumed = vox_scanner_offset(sc) - start_offset; return -2; }
            vox_scanner_skip(sc, 2); /* \r\n */
            p->phase = VOX_HTTP_PHASE_CHUNK_SIZE;
            continue;
        }
        break;
    }
    *consumed = vox_scanner_offset(sc) - start_offset;
    return 0;
}

/* ========== 公开 API ========== */

vox_http_parser_t* vox_http_parser_create(vox_mpool_t* mpool,
                                          const vox_http_parser_config_t* config,
                                          const vox_http_callbacks_t* callbacks) {
    if (!mpool) return NULL;
    vox_http_parser_t* p = (vox_http_parser_t*)vox_mpool_alloc(mpool, sizeof(struct vox_http_parser));
    if (!p) return NULL;
    memset(p, 0, sizeof(struct vox_http_parser));
    p->mpool = mpool;
    if (config)
        p->config = *config;
    else {
        p->config.type = VOX_HTTP_PARSER_TYPE_BOTH;
        p->config.max_header_size = 0;
        p->config.max_headers = 0;
        p->config.max_url_size = 0;
        p->config.strict_mode = false;
    }
    if (callbacks) {
        p->callbacks = *callbacks;
        p->user_data = callbacks->user_data;
    }
    if (vox_scanner_stream_init(&p->stream, mpool, 0) != 0) {
        vox_mpool_free(mpool, p);
        return NULL;
    }
    p->sc = vox_scanner_stream_get_scanner(&p->stream);
    return p;
}

void vox_http_parser_destroy(vox_http_parser_t* parser) {
    if (!parser) return;
    vox_scanner_stream_destroy(&parser->stream);
    if (parser->buf)
        vox_mpool_free(parser->mpool, parser->buf);
    vox_mpool_free(parser->mpool, parser);
}

ssize_t vox_http_parser_execute(vox_http_parser_t* parser, const char* data, size_t len) {
    if (!parser) return -1;
    if (parser->has_error || parser->phase == VOX_HTTP_PHASE_ERROR) return -1;
    if (len == 0) return 0;

    if (ensure_buf(parser, len) != 0) {
        set_error(parser, "buffer alloc failed");
        invoke_error(parser, parser->error_msg);
        return -1;
    }
    memcpy(parser->buf + parser->buf_off + parser->buf_size, data, len);
    parser->buf_size += len;
    parser->buf[parser->buf_off + parser->buf_size] = '\0';

    vox_scanner_stream_reset(&parser->stream);
    if (vox_scanner_stream_feed(&parser->stream, parser->buf + parser->buf_off, parser->buf_size) != 0) {
        set_error(parser, "stream feed failed");
        return -1;
    }
    parser->sc = vox_scanner_stream_get_scanner(&parser->stream);

    size_t consumed = 0;
    int r = do_parse(parser, &consumed);
    if (r == -1) {
        invoke_error(parser, parser->error_msg);
        return -1;
    }
    if (r == -2 && consumed == 0 && parser->buf_size > 0) {
        if (parser->config.max_header_size && parser->buf_size >= parser->config.max_header_size) {
            set_error(parser, "header too large");
            invoke_error(parser, parser->error_msg);
            return -1;
        }
        /* 需要更多数据且未消费任何字节：下次 execute 会 append 后整缓冲 re-feed，
         * scanner 会从缓冲开头开始，故必须从 INIT 重新解析，否则会把首行当 header 解析失败 */
        parser->phase = VOX_HTTP_PHASE_INIT;
    }

    if (consumed > 0) {
        parser->buf_off += consumed;
        parser->buf_size -= consumed;
        if (parser->buf_off >= 4096u || (parser->buf_capacity > 0 && parser->buf_off > (parser->buf_capacity >> 1)))
            compact_buf(parser);
    }
    return (ssize_t)consumed;
}

void vox_http_parser_reset(vox_http_parser_t* parser) {
    if (!parser) return;
    parser->phase = VOX_HTTP_PHASE_INIT;
    parser->message_complete = false;
    parser->has_error = false;
    parser->error_msg[0] = '\0';
    parser->buf_off = 0;
    parser->buf_size = 0;
    parser->method = VOX_HTTP_METHOD_UNKNOWN;
    parser->http_major = parser->http_minor = 0;
    parser->status_code = 0;
    parser->content_length = 0;
    parser->body_read = 0;
    parser->chunked = false;
    parser->chunk_remaining = 0;
    parser->connection_close = false;
    parser->connection_keepalive = false;
    parser->upgrade = false;
    parser->header_count = 0;
    vox_scanner_stream_reset(&parser->stream);
}

bool vox_http_parser_is_complete(const vox_http_parser_t* parser) {
    return parser && parser->message_complete;
}

bool vox_http_parser_has_error(const vox_http_parser_t* parser) {
    return parser && parser->has_error;
}

const char* vox_http_parser_get_error(const vox_http_parser_t* parser) {
    if (!parser || !parser->has_error) return NULL;
    return parser->error_msg;
}

vox_http_method_t vox_http_parser_get_method(const vox_http_parser_t* parser) {
    return parser ? parser->method : VOX_HTTP_METHOD_UNKNOWN;
}

int vox_http_parser_get_http_major(const vox_http_parser_t* parser) {
    return parser ? parser->http_major : 0;
}

int vox_http_parser_get_http_minor(const vox_http_parser_t* parser) {
    return parser ? parser->http_minor : 0;
}

int vox_http_parser_get_status_code(const vox_http_parser_t* parser) {
    return parser ? parser->status_code : 0;
}

uint64_t vox_http_parser_get_content_length(const vox_http_parser_t* parser) {
    return parser ? parser->content_length : 0;
}

bool vox_http_parser_is_chunked(const vox_http_parser_t* parser) {
    return parser && parser->chunked;
}

bool vox_http_parser_is_connection_close(const vox_http_parser_t* parser) {
    return parser && parser->connection_close;
}

bool vox_http_parser_is_connection_keep_alive(const vox_http_parser_t* parser) {
    return parser && parser->connection_keepalive;
}

bool vox_http_parser_is_upgrade(const vox_http_parser_t* parser) {
    return parser && parser->upgrade;
}

void* vox_http_parser_get_user_data(const vox_http_parser_t* parser) {
    return parser ? parser->user_data : NULL;
}

void vox_http_parser_set_user_data(vox_http_parser_t* parser, void* user_data) {
    if (parser) parser->user_data = user_data;
}
