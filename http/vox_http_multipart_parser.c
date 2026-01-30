/*
 * vox_http_multipart_parser.c - 高性能 HTTP Multipart 解析器实现
 * 使用 vox_scanner 流式解析：内部缓冲累积输入，vox_scanner 驱动零拷贝解析
 * 支持 multipart/form-data 和 multipart/mixed (RFC 2046, 7578)
 */

#include "vox_http_multipart_parser.h"
#include "../vox_scanner.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* 默认缓冲区容量 */
#define VOX_MULTIPART_BUF_INIT  4096
#define VOX_MULTIPART_BUF_MAX   (1024 * 1024)

/* 解析阶段 */
typedef enum {
    VOX_MP_PHASE_INIT = 0,
    VOX_MP_PHASE_FIRST_BOUNDARY,
    VOX_MP_PHASE_PART_HEADERS,
    VOX_MP_PHASE_PART_BODY,
    VOX_MP_PHASE_COMPLETE,
    VOX_MP_PHASE_ERROR
} vox_mp_phase_t;

/* 内部解析器结构 */
struct vox_multipart_parser {
    vox_mpool_t* mpool;
    vox_multipart_parser_config_t config;
    vox_multipart_callbacks_t callbacks;
    void* user_data;

    /* 内部缓冲：buf 有效区间 [buf_off, buf_off + buf_size)，延迟 memmove */
    char* buf;
    size_t buf_off;   /* 逻辑起始偏移，实际数据从 buf + buf_off 开始 */
    size_t buf_size;
    size_t buf_capacity;

    vox_scanner_stream_t stream;
    vox_scanner_t* sc;

    vox_mp_phase_t phase;
    bool complete;
    bool has_error;
    char error_msg[128];

    /* boundary：创建时复制，不含前导 "--" */
    char* boundary;
    size_t boundary_len;
    /* 预计算的边界串（指向 parser 内或静态区，不含 NUL 的裸串用 len 控制） */
    char* first_delim;   /* "--" + boundary + "\r\n" */
    size_t first_delim_len;
    char* next_delim;    /* "\r\n--" + boundary + "\r\n" */
    size_t next_delim_len;
    char* end_delim;     /* "\r\n--" + boundary + "--\r\n" */
    size_t end_delim_len;
    char* first_end_delim; /* "--" + boundary + "--\r\n" 用于开头即结束（无 part） */
    size_t first_end_delim_len;
    /* part body 中允许 \n 或 \r\n 前导（RFC 兼容） */
    char* next_delim_ln;   /* "\n--" + boundary + "\r\n" */
    size_t next_delim_ln_len;
    char* end_delim_ln;   /* "\n--" + boundary + "--\r\n" */
    size_t end_delim_ln_len;
    size_t max_delim_len; /* 四种边界最大长度，用于 safe 计算 */

    size_t header_count;
    size_t header_size;
};

static void set_error(struct vox_multipart_parser* p, const char* msg) {
    p->has_error = true;
    p->phase = VOX_MP_PHASE_ERROR;
    if (msg) {
        size_t n = strlen(msg);
        if (n >= sizeof(p->error_msg)) n = sizeof(p->error_msg) - 1;
        memcpy(p->error_msg, msg, n);
        p->error_msg[n] = '\0';
    } else {
        p->error_msg[0] = '\0';
    }
}

/* 压缩缓冲：将 [buf_off, buf_off+buf_size) 移到 [0, buf_size)，buf_off=0 */
static void compact_buf(struct vox_multipart_parser* p) {
    if (p->buf_off == 0 || p->buf_size == 0) return;
    memmove(p->buf, p->buf + p->buf_off, p->buf_size);
    p->buf_off = 0;
}

static int ensure_buf(struct vox_multipart_parser* p, size_t need) {
    size_t want = p->buf_off + p->buf_size + need + 1;
    if (want <= p->buf_capacity)
        return 0;
    /* 先压缩再检查，避免频繁扩容 */
    if (p->buf_off > 0) {
        compact_buf(p);
        if (p->buf_size + need + 1 <= p->buf_capacity)
            return 0;
    }
    size_t new_cap = p->buf_capacity ? p->buf_capacity : VOX_MULTIPART_BUF_INIT;
    while (new_cap < p->buf_size + need + 1) {
        size_t next = new_cap + (new_cap >> 1);
        if (next <= new_cap || next > VOX_MULTIPART_BUF_MAX)
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

/* 从 Content-Disposition 的 value 中解析 name= 和 filename=，并触发 on_name/on_filename */
static void parse_content_disposition(struct vox_multipart_parser* p,
                                       const char* value_ptr, size_t value_len) {
    const vox_multipart_callbacks_t* cb = &p->callbacks;
    const char* end = value_ptr + value_len;
    while (value_ptr < end) {
        while (value_ptr < end && (*value_ptr == ' ' || *value_ptr == '\t' || *value_ptr == ';'))
            value_ptr++;
        if (value_ptr >= end) break;
        const char* token = value_ptr;
        while (value_ptr < end && *value_ptr != '=' && *value_ptr != ';')
            value_ptr++;
        if (value_ptr >= end || *value_ptr != '=') continue;
        size_t key_len = (size_t)(value_ptr - token);
        value_ptr++;
        while (value_ptr < end && (*value_ptr == ' ' || *value_ptr == '\t'))
            value_ptr++;
        if (value_ptr >= end) break;
        const char* val_start = value_ptr;
        size_t val_len = 0;
        if (*value_ptr == '"') {
            value_ptr++;
            val_start = value_ptr;
            while (value_ptr < end && *value_ptr != '"') {
                if (*value_ptr == '\\' && value_ptr + 1 < end)
                    value_ptr++;
                value_ptr++;
            }
            val_len = (size_t)(value_ptr - val_start);
            if (value_ptr < end) value_ptr++;
        } else {
            while (value_ptr < end && *value_ptr != ';' && *value_ptr != ' ' && *value_ptr != '\t')
                value_ptr++;
            val_len = (size_t)(value_ptr - val_start);
        }
        if (key_len == 4 && strncasecmp(token, "name", 4) == 0 && cb->on_name && val_len > 0) {
            if (!p->config.max_field_name_size || val_len <= p->config.max_field_name_size)
                cb->on_name((void*)p, val_start, val_len);
        } else if (key_len == 8 && strncasecmp(token, "filename", 8) == 0 && cb->on_filename && val_len > 0) {
            if (!p->config.max_filename_size || val_len <= p->config.max_filename_size)
                cb->on_filename((void*)p, val_start, val_len);
        }
    }
}

/* 流式下可能 \r\n 被截断；peek 到 \r\n 为止，若不足则返回 -2 */
static int peek_line(vox_scanner_t* sc, vox_strview_t* out) {
    if (vox_scanner_peek_until_str(sc, "\r\n", false, out) != 0)
        return -1;
    size_t rem = vox_scanner_remaining(sc);
    if (out->len + 2 > rem)
        return -2;
    return 0;
}

/* 解析 part 头部：逐行 name: value，直到空行；返回 0 成功，-1 错误，-2 需更多数据 */
static int parse_part_headers(struct vox_multipart_parser* p, vox_scanner_t* sc, size_t* consumed) {
    vox_strview_t line;
    size_t start_off = vox_scanner_offset(sc);

    for (;;) {
        int pr = peek_line(sc, &line);
        if (pr == -2) {
            *consumed = vox_scanner_offset(sc) - start_off;
            return -2;
        }
        if (pr != 0) return -1;

        if (line.len == 0) {
            vox_scanner_skip(sc, 2);
            if (p->callbacks.on_headers_complete)
                if (p->callbacks.on_headers_complete((void*)p) != 0) {
                    set_error(p, "callback error");
                    return -1;
                }
            *consumed = vox_scanner_offset(sc) - start_off;
            return 0;
        }

        if (p->config.max_header_size && (p->header_size + line.len + 2 > p->config.max_header_size)) {
            set_error(p, "header too large");
            return -1;
        }
        if (p->config.max_headers && p->header_count >= p->config.max_headers) {
            set_error(p, "too many headers");
            return -1;
        }

        size_t colon = 0;
        while (colon < line.len && line.ptr[colon] != ':') colon++;
        if (colon == 0 || colon >= line.len) {
            if (p->config.strict_mode) {
                set_error(p, "invalid header line");
                return -1;
            }
            vox_scanner_get_until_str(sc, "\r\n", true, &line);
            p->header_size += line.len + 2;
            continue;
        }

        const char* field_start = line.ptr;
        size_t field_len = colon;
        const char* value_start = line.ptr + colon + 1;
        size_t value_len = line.len - (colon + 1);
        while (value_len && (*value_start == ' ' || *value_start == '\t')) {
            value_start++;
            value_len--;
        }
        size_t trim = value_len;
        while (trim && (value_start[trim - 1] == ' ' || value_start[trim - 1] == '\t'))
            trim--;
        value_len = trim;

        if (p->callbacks.on_header_field && field_len > 0)
            if (p->callbacks.on_header_field((void*)p, field_start, field_len) != 0) {
                set_error(p, "callback error");
                return -1;
            }
        if (p->callbacks.on_header_value && value_len > 0)
            if (p->callbacks.on_header_value((void*)p, value_start, value_len) != 0) {
                set_error(p, "callback error");
                return -1;
            }
        if (field_len >= 19 && strncasecmp(field_start, "Content-Disposition", 19) == 0)
            parse_content_disposition(p, value_start, value_len);

        p->header_count++;
        p->header_size += line.len + 2;
        vox_scanner_get_until_str(sc, "\r\n", true, &line);
    }
}

/**
 * 单遍查找 part body 中的边界：\r\n--boundary\r\n | \r\n--boundary--\r\n | \n--boundary\r\n | \n--boundary--\r\n
 * 仅在 \n 后检查 "--" + boundary，减少无换行 body 的 memcmp 次数。
 * 返回 0 找到：*body_off = body 字节数，*skip_len = 边界总字节数，*is_end = 是否结束边界；-2 需更多数据
 */
static int find_boundary_single_pass(struct vox_multipart_parser* p, vox_scanner_t* sc,
                                    size_t* body_off, size_t* skip_len, bool* is_end) {
    size_t rem = vox_scanner_remaining(sc);
    const char* cur = vox_scanner_curptr(sc);
    const char* boundary = p->boundary;
    size_t blen = p->boundary_len;
    /* 至少需要: 前导 \n(1) + "--"(2) + boundary + 后缀 \r\n(2) */
    size_t min_len = 1 + 2 + blen + 2;
    if (rem < min_len)
        return -2;
    /* 只在前导 \n 处检查 "--" + boundary，避免对每个字节做 memcmp */
    for (size_t pos = 0; pos + min_len <= rem; pos++) {
        if (cur[pos] != '\n')
            continue;
        size_t i = pos + 1;
        if (cur[i] != '-' || cur[i + 1] != '-')
            continue;
        if (memcmp(cur + i + 2, boundary, blen) != 0)
            continue;
        size_t prefix_len = (i >= 2 && cur[i - 2] == '\r') ? 2u : 1u;
        size_t tail = i + 2 + blen;
        if (tail + 2 > rem)
            return -2;
        if (cur[tail] == '\r' && cur[tail + 1] == '\n') {
            *body_off = i - prefix_len;
            *skip_len = prefix_len + (2 + blen) + 2;
            *is_end = false;
            return 0;
        }
        if (cur[tail] == '-' && cur[tail + 1] == '-' && tail + 4 <= rem &&
            cur[tail + 2] == '\r' && cur[tail + 3] == '\n') {
            *body_off = i - prefix_len;
            *skip_len = prefix_len + (2 + blen) + 4;
            *is_end = true;
            return 0;
        }
    }
    return -2;
}

/* 执行解析：从当前 scanner 解析，更新 phase 与 consumed；0 正常，-1 错误，-2 需更多数据 */
static int do_parse(struct vox_multipart_parser* p, size_t* consumed) {
    vox_scanner_t* sc = p->sc;
    if (!sc || vox_scanner_eof(sc)) {
        *consumed = 0;
        return -2;
    }
    size_t start_offset = vox_scanner_offset(sc);

    for (;;) {
        if (p->phase == VOX_MP_PHASE_INIT)
            p->phase = VOX_MP_PHASE_FIRST_BOUNDARY;

        if (p->phase == VOX_MP_PHASE_FIRST_BOUNDARY) {
            size_t rem = vox_scanner_remaining(sc);
            if (rem >= 2 && sc->curptr[0] == '\r' && sc->curptr[1] == '\n') {
                vox_scanner_skip(sc, 2);
                start_offset = vox_scanner_offset(sc);
            }
            rem = vox_scanner_remaining(sc);
            /* 开头即结束：--boundary--\r\n（无 part） */
            if (rem >= p->first_end_delim_len &&
                memcmp(vox_scanner_curptr(sc), p->first_end_delim, p->first_end_delim_len) == 0) {
                vox_scanner_skip(sc, p->first_end_delim_len);
                if (p->callbacks.on_complete)
                    if (p->callbacks.on_complete((void*)p) != 0) {
                        set_error(p, "callback error");
                        return -1;
                    }
                p->phase = VOX_MP_PHASE_COMPLETE;
                p->complete = true;
                *consumed = vox_scanner_offset(sc) - start_offset;
                return 0;
            }
            if (rem < p->first_delim_len) {
                if (rem > 0) {
                    size_t cmp_len = rem < p->first_delim_len ? rem : p->first_delim_len;
                    if (memcmp(vox_scanner_curptr(sc), p->first_delim, cmp_len) != 0) {
                        set_error(p, "invalid first boundary");
                        return -1;
                    }
                }
                *consumed = vox_scanner_offset(sc) - start_offset;
                return -2;
            }
            if (memcmp(vox_scanner_curptr(sc), p->first_delim, p->first_delim_len) != 0) {
                set_error(p, "invalid first boundary");
                return -1;
            }
            vox_scanner_skip(sc, p->first_delim_len);
            if (p->callbacks.on_part_begin && p->callbacks.on_part_begin((void*)p) != 0) {
                set_error(p, "callback error");
                return -1;
            }
            p->phase = VOX_MP_PHASE_PART_HEADERS;
            p->header_count = 0;
            p->header_size = 0;
            continue;
        }

        if (p->phase == VOX_MP_PHASE_PART_HEADERS) {
            int r = parse_part_headers(p, sc, consumed);
            if (r == -2) return -2;
            if (r != 0) return -1;
            p->phase = VOX_MP_PHASE_PART_BODY;
            continue;
        }

        if (p->phase == VOX_MP_PHASE_PART_BODY) {
            size_t body_off = 0, skip_len = 0;
            bool is_end = false;
            int br = find_boundary_single_pass(p, sc, &body_off, &skip_len, &is_end);

            if (br == -2) {
                size_t rem = vox_scanner_remaining(sc);
                if (rem == 0) {
                    *consumed = vox_scanner_offset(sc) - start_offset;
                    return -2;
                }
                /* 只发射安全字节，避免把边界前缀当 body 发出 */
                size_t safe = (rem > p->max_delim_len - 1) ? (rem - (p->max_delim_len - 1)) : 0;
                if (safe > 0) {
                    vox_strview_t seg;
                    vox_scanner_peek(sc, safe, &seg);
                    if (p->callbacks.on_part_data)
                        if (p->callbacks.on_part_data((void*)p, seg.ptr, seg.len) != 0) {
                            set_error(p, "callback error");
                            return -1;
                        }
                    vox_scanner_skip(sc, safe);
                }
                *consumed = vox_scanner_offset(sc) - start_offset;
                return -2;
            }

            if (body_off > 0 && p->callbacks.on_part_data) {
                vox_strview_t seg;
                vox_scanner_peek(sc, body_off, &seg);
                if (p->callbacks.on_part_data((void*)p, seg.ptr, seg.len) != 0) {
                    set_error(p, "callback error");
                    return -1;
                }
            }
            vox_scanner_skip(sc, body_off);
            if (is_end) {
                vox_scanner_skip(sc, skip_len);
                if (p->callbacks.on_part_complete)
                    if (p->callbacks.on_part_complete((void*)p) != 0) {
                        set_error(p, "callback error");
                        return -1;
                    }
                if (p->callbacks.on_complete)
                    if (p->callbacks.on_complete((void*)p) != 0) {
                        set_error(p, "callback error");
                        return -1;
                    }
                p->phase = VOX_MP_PHASE_COMPLETE;
                p->complete = true;
                *consumed = vox_scanner_offset(sc) - start_offset;
                return 0;
            }
            vox_scanner_skip(sc, skip_len);
            if (p->callbacks.on_part_complete)
                if (p->callbacks.on_part_complete((void*)p) != 0) {
                    set_error(p, "callback error");
                    return -1;
                }
            if (p->callbacks.on_part_begin)
                if (p->callbacks.on_part_begin((void*)p) != 0) {
                    set_error(p, "callback error");
                    return -1;
                }
            p->phase = VOX_MP_PHASE_PART_HEADERS;
            p->header_count = 0;
            p->header_size = 0;
            continue;
        }

        if (p->phase == VOX_MP_PHASE_COMPLETE || p->phase == VOX_MP_PHASE_ERROR) {
            *consumed = vox_scanner_offset(sc) - start_offset;
            return 0;
        }

        *consumed = vox_scanner_offset(sc) - start_offset;
        return -2;
    }
}

/* ===== 公共 API ===== */

vox_multipart_parser_t* vox_multipart_parser_create(vox_mpool_t* mpool,
                                                    const char* boundary,
                                                    size_t boundary_len,
                                                    const vox_multipart_parser_config_t* config,
                                                    const vox_multipart_callbacks_t* callbacks) {
    if (!mpool || !boundary || boundary_len == 0)
        return NULL;
    if (boundary_len > 70)
        return NULL;

    struct vox_multipart_parser* p = (struct vox_multipart_parser*)vox_mpool_alloc(mpool, sizeof(struct vox_multipart_parser));
    if (!p) return NULL;
    memset(p, 0, sizeof(struct vox_multipart_parser));
    p->mpool = mpool;
    if (config)
        p->config = *config;
    if (callbacks)
        p->callbacks = *callbacks;
    if (callbacks && callbacks->user_data)
        p->user_data = callbacks->user_data;

    p->boundary = (char*)vox_mpool_alloc(mpool, boundary_len + 1);
    if (!p->boundary) {
        vox_mpool_free(mpool, p);
        return NULL;
    }
    memcpy(p->boundary, boundary, boundary_len);
    p->boundary[boundary_len] = '\0';
    p->boundary_len = boundary_len;

    p->first_delim_len = 2 + boundary_len + 2;
    p->first_delim = (char*)vox_mpool_alloc(mpool, p->first_delim_len + 1);
    if (!p->first_delim) {
        vox_mpool_free(mpool, p->boundary);
        vox_mpool_free(mpool, p);
        return NULL;
    }
    p->first_delim[0] = '-';
    p->first_delim[1] = '-';
    memcpy(p->first_delim + 2, boundary, boundary_len);
    p->first_delim[2 + boundary_len] = '\r';
    p->first_delim[2 + boundary_len + 1] = '\n';

    p->next_delim_len = 2 + 2 + boundary_len + 2;
    p->next_delim = (char*)vox_mpool_alloc(mpool, p->next_delim_len + 1);
    if (!p->next_delim) {
        vox_mpool_free(mpool, p->first_delim);
        vox_mpool_free(mpool, p->boundary);
        vox_mpool_free(mpool, p);
        return NULL;
    }
    p->next_delim[0] = '\r';
    p->next_delim[1] = '\n';
    p->next_delim[2] = '-';
    p->next_delim[3] = '-';
    memcpy(p->next_delim + 4, boundary, boundary_len);
    p->next_delim[4 + boundary_len] = '\r';
    p->next_delim[4 + boundary_len + 1] = '\n';

    p->end_delim_len = 2 + 2 + boundary_len + 2 + 2;
    p->end_delim = (char*)vox_mpool_alloc(mpool, p->end_delim_len + 1);
    if (!p->end_delim) {
        vox_mpool_free(mpool, p->next_delim);
        vox_mpool_free(mpool, p->first_delim);
        vox_mpool_free(mpool, p->boundary);
        vox_mpool_free(mpool, p);
        return NULL;
    }
    p->end_delim[0] = '\r';
    p->end_delim[1] = '\n';
    p->end_delim[2] = '-';
    p->end_delim[3] = '-';
    memcpy(p->end_delim + 4, boundary, boundary_len);
    p->end_delim[4 + boundary_len] = '-';
    p->end_delim[4 + boundary_len + 1] = '-';
    p->end_delim[4 + boundary_len + 2] = '\r';
    p->end_delim[4 + boundary_len + 3] = '\n';

    /* \n--boundary\r\n 与 \n--boundary--\r\n（兼容 body 仅 \n 结尾） */
    p->next_delim_ln_len = 1 + 2 + boundary_len + 2;
    p->next_delim_ln = (char*)vox_mpool_alloc(mpool, p->next_delim_ln_len + 1);
    if (!p->next_delim_ln) {
        vox_mpool_free(mpool, p->end_delim);
        vox_mpool_free(mpool, p->next_delim);
        vox_mpool_free(mpool, p->first_delim);
        vox_mpool_free(mpool, p->boundary);
        vox_mpool_free(mpool, p);
        return NULL;
    }
    p->next_delim_ln[0] = '\n';
    p->next_delim_ln[1] = '-';
    p->next_delim_ln[2] = '-';
    memcpy(p->next_delim_ln + 3, boundary, boundary_len);
    p->next_delim_ln[3 + boundary_len] = '\r';
    p->next_delim_ln[3 + boundary_len + 1] = '\n';

    p->end_delim_ln_len = 1 + 2 + boundary_len + 2 + 2;
    p->end_delim_ln = (char*)vox_mpool_alloc(mpool, p->end_delim_ln_len + 1);
    if (!p->end_delim_ln) {
        vox_mpool_free(mpool, p->next_delim_ln);
        vox_mpool_free(mpool, p->end_delim);
        vox_mpool_free(mpool, p->next_delim);
        vox_mpool_free(mpool, p->first_delim);
        vox_mpool_free(mpool, p->boundary);
        vox_mpool_free(mpool, p);
        return NULL;
    }
    p->end_delim_ln[0] = '\n';
    p->end_delim_ln[1] = '-';
    p->end_delim_ln[2] = '-';
    memcpy(p->end_delim_ln + 3, boundary, boundary_len);
    p->end_delim_ln[3 + boundary_len] = '-';
    p->end_delim_ln[3 + boundary_len + 1] = '-';
    p->end_delim_ln[3 + boundary_len + 2] = '\r';
    p->end_delim_ln[3 + boundary_len + 3] = '\n';

    p->first_end_delim_len = 2 + boundary_len + 2 + 2;
    p->first_end_delim = (char*)vox_mpool_alloc(mpool, p->first_end_delim_len + 1);
    if (!p->first_end_delim) {
        vox_mpool_free(mpool, p->end_delim_ln);
        vox_mpool_free(mpool, p->next_delim_ln);
        vox_mpool_free(mpool, p->end_delim);
        vox_mpool_free(mpool, p->next_delim);
        vox_mpool_free(mpool, p->first_delim);
        vox_mpool_free(mpool, p->boundary);
        vox_mpool_free(mpool, p);
        return NULL;
    }
    p->first_end_delim[0] = '-';
    p->first_end_delim[1] = '-';
    memcpy(p->first_end_delim + 2, boundary, boundary_len);
    p->first_end_delim[2 + boundary_len] = '-';
    p->first_end_delim[2 + boundary_len + 1] = '-';
    p->first_end_delim[2 + boundary_len + 2] = '\r';
    p->first_end_delim[2 + boundary_len + 3] = '\n';

    p->max_delim_len = p->end_delim_len;
    if (p->next_delim_len > p->max_delim_len) p->max_delim_len = p->next_delim_len;
    if (p->next_delim_ln_len > p->max_delim_len) p->max_delim_len = p->next_delim_ln_len;
    if (p->end_delim_ln_len > p->max_delim_len) p->max_delim_len = p->end_delim_ln_len;

    if (vox_scanner_stream_init(&p->stream, mpool, 0) != 0) {
        vox_mpool_free(mpool, p->first_end_delim);
        vox_mpool_free(mpool, p->end_delim_ln);
        vox_mpool_free(mpool, p->next_delim_ln);
        vox_mpool_free(mpool, p->end_delim);
        vox_mpool_free(mpool, p->next_delim);
        vox_mpool_free(mpool, p->first_delim);
        vox_mpool_free(mpool, p->boundary);
        vox_mpool_free(mpool, p);
        return NULL;
    }
    p->sc = vox_scanner_stream_get_scanner(&p->stream);
    return (vox_multipart_parser_t*)p;
}

void vox_multipart_parser_destroy(vox_multipart_parser_t* parser) {
    if (!parser) return;
    struct vox_multipart_parser* p = (struct vox_multipart_parser*)parser;
    vox_scanner_stream_destroy(&p->stream);
    if (p->first_end_delim) vox_mpool_free(p->mpool, p->first_end_delim);
    if (p->end_delim_ln) vox_mpool_free(p->mpool, p->end_delim_ln);
    if (p->next_delim_ln) vox_mpool_free(p->mpool, p->next_delim_ln);
    if (p->end_delim) vox_mpool_free(p->mpool, p->end_delim);
    if (p->next_delim) vox_mpool_free(p->mpool, p->next_delim);
    if (p->first_delim) vox_mpool_free(p->mpool, p->first_delim);
    if (p->boundary) vox_mpool_free(p->mpool, p->boundary);
    if (p->buf) vox_mpool_free(p->mpool, p->buf);
    vox_mpool_free(p->mpool, p);
}

ssize_t vox_multipart_parser_execute(vox_multipart_parser_t* parser, const char* data, size_t len) {
    if (!parser) return -1;
    struct vox_multipart_parser* p = (struct vox_multipart_parser*)parser;
    if (p->has_error || p->phase == VOX_MP_PHASE_ERROR) return -1;
    if (p->complete) return 0;
    if (len == 0) return 0;

    if (ensure_buf(p, len) != 0) {
        set_error(p, "buffer alloc failed");
        if (p->callbacks.on_error) p->callbacks.on_error((void*)p, p->error_msg);
        return -1;
    }
    memcpy(p->buf + p->buf_off + p->buf_size, data, len);
    p->buf_size += len;
    p->buf[p->buf_off + p->buf_size] = '\0';

    vox_scanner_stream_reset(&p->stream);
    if (vox_scanner_stream_feed(&p->stream, p->buf + p->buf_off, p->buf_size) != 0) {
        set_error(p, "stream feed failed");
        if (p->callbacks.on_error) p->callbacks.on_error((void*)p, p->error_msg);
        return -1;
    }
    p->sc = vox_scanner_stream_get_scanner(&p->stream);

    size_t consumed = 0;
    int r = do_parse(p, &consumed);
    if (r == -1) {
        if (p->callbacks.on_error) p->callbacks.on_error((void*)p, p->error_msg);
        return -1;
    }
    if (r == -2 && consumed == 0 && p->buf_size > 0) {
        if (p->config.max_header_size && p->header_size >= p->config.max_header_size) {
            set_error(p, "header too large");
            if (p->callbacks.on_error) p->callbacks.on_error((void*)p, p->error_msg);
            return -1;
        }
        /* 仅在尚未进入 part 时重置 phase，避免 PART_BODY 等阶段把 body 当边界重解析 */
        if (p->phase == VOX_MP_PHASE_INIT || p->phase == VOX_MP_PHASE_FIRST_BOUNDARY)
            p->phase = VOX_MP_PHASE_INIT;
    }

    if (consumed > 0) {
        p->buf_off += consumed;
        p->buf_size -= consumed;
        /* 延迟 memmove：仅当逻辑起始偏移过大时压缩，减少大 body 流式的搬移次数 */
        if (p->buf_off >= 4096u || (p->buf_capacity > 0 && p->buf_off > (p->buf_capacity >> 1)))
            compact_buf(p);
    }
    return (ssize_t)consumed;
}

void vox_multipart_parser_reset(vox_multipart_parser_t* parser) {
    if (!parser) return;
    struct vox_multipart_parser* p = (struct vox_multipart_parser*)parser;
    p->phase = VOX_MP_PHASE_INIT;
    p->complete = false;
    p->has_error = false;
    p->error_msg[0] = '\0';
    p->buf_off = 0;
    p->buf_size = 0;
    p->header_count = 0;
    p->header_size = 0;
    vox_scanner_stream_reset(&p->stream);
}

bool vox_multipart_parser_is_complete(const vox_multipart_parser_t* parser) {
    return parser && ((struct vox_multipart_parser*)parser)->complete;
}

bool vox_multipart_parser_has_error(const vox_multipart_parser_t* parser) {
    return parser && ((struct vox_multipart_parser*)parser)->has_error;
}

const char* vox_multipart_parser_get_error(const vox_multipart_parser_t* parser) {
    if (!parser || !((struct vox_multipart_parser*)parser)->has_error) return NULL;
    return ((struct vox_multipart_parser*)parser)->error_msg;
}

void* vox_multipart_parser_get_user_data(const vox_multipart_parser_t* parser) {
    return parser ? ((struct vox_multipart_parser*)parser)->user_data : NULL;
}

void vox_multipart_parser_set_user_data(vox_multipart_parser_t* parser, void* user_data) {
    if (parser) ((struct vox_multipart_parser*)parser)->user_data = user_data;
}
