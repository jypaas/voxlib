/*
 * vox_scanner.c - 零拷贝字符串扫描器实现
 * 通过指针移动实现零拷贝解析
 */

#include "vox_scanner.h"
#include "vox_mpool.h"
#include <string.h>

/* ===== 字符集规范实现 ===== */

void vox_charset_init(vox_charset_t* cs) {
    if (cs) {
        memset(cs->bitmap, 0, sizeof(cs->bitmap));
    }
}

static inline void set_bit(uint8_t* bitmap, int bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline bool get_bit(const uint8_t* bitmap, int bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

int vox_charset_add_char(vox_charset_t* cs, char ch) {
    if (!cs) return -1;
    set_bit(cs->bitmap, (unsigned char)ch);
    return 0;
}

int vox_charset_add_range(vox_charset_t* cs, char start, char end) {
    if (!cs) return -1;
    
    unsigned char s = (unsigned char)start;
    unsigned char e = (unsigned char)end;
    
    if (s > e) {
        unsigned char tmp = s;
        s = e;
        e = tmp;
    }
    
    for (unsigned char ch = s; ch <= e; ch++) {
        set_bit(cs->bitmap, ch);
    }
    
    return 0;
}

int vox_charset_add_alpha(vox_charset_t* cs) {
    if (!cs) return -1;
    vox_charset_add_range(cs, 'a', 'z');
    vox_charset_add_range(cs, 'A', 'Z');
    return 0;
}

int vox_charset_add_digit(vox_charset_t* cs) {
    if (!cs) return -1;
    vox_charset_add_range(cs, '0', '9');
    return 0;
}

int vox_charset_add_alnum(vox_charset_t* cs) {
    if (!cs) return -1;
    vox_charset_add_alpha(cs);
    vox_charset_add_digit(cs);
    return 0;
}

int vox_charset_add_space(vox_charset_t* cs) {
    if (!cs) return -1;
    vox_charset_add_char(cs, ' ');
    vox_charset_add_char(cs, '\t');
    vox_charset_add_char(cs, '\n');
    vox_charset_add_char(cs, '\r');
    vox_charset_add_char(cs, '\v');
    vox_charset_add_char(cs, '\f');
    return 0;
}

bool vox_charset_contains(const vox_charset_t* cs, char ch) {
    if (!cs) return false;
    return get_bit(cs->bitmap, (unsigned char)ch);
}

/* ===== 扫描器实现 ===== */

/* 扫描器状态保存结构已在头文件中定义 */

/* 检查字符是否为空白字符 */
static inline bool is_whitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || 
           ch == '\v' || ch == '\f';
}

/* 检查字符是否为换行符 */
static inline bool is_newline(char ch) {
    return ch == '\n' || ch == '\r';
}

/* 自动跳过选项指定的字符 */
static void auto_skip(vox_scanner_t* scanner) {
    if (!scanner) return;
    
    int flags = scanner->flags;
    bool skip_ws = (flags & VOX_SCANNER_AUTOSKIP_WS) != 0;
    bool skip_nl = (flags & VOX_SCANNER_AUTOSKIP_NEWLINE) != 0;
    
    /* 如果没有设置任何自动跳过选项，直接返回 */
    if (!skip_ws && !skip_nl) {
        return;
    }
    
    /* 在一个循环中处理所有需要跳过的字符 */
    while (scanner->curptr < scanner->end) {
        char ch = *scanner->curptr;
        bool should_skip = false;
        
        /* 如果设置了 AUTOSKIP_WS，检查是否为空白字符 */
        if (skip_ws && is_whitespace(ch)) {
            should_skip = true;
        }
        /* 如果只设置了 AUTOSKIP_NEWLINE（且未设置 AUTOSKIP_WS），检查是否为换行符 */
        else if (skip_nl && !skip_ws && is_newline(ch)) {
            should_skip = true;
        }
        
        if (!should_skip) {
            break;
        }
        
        scanner->curptr++;
    }
}

int vox_scanner_init(vox_scanner_t* scanner, char* buf, size_t len, int flags) {
    if (!scanner || !buf) return -1;
    
    /* 确保缓冲区末尾有'\0' */
    if (len > 0 && buf[len] != '\0') {
        return -1;
    }
    
    scanner->begin = buf;
    scanner->end = buf + len;
    scanner->curptr = buf;
    scanner->flags = flags;
    
    /* 应用自动跳过选项 */
    auto_skip(scanner);
    
    return 0;
}

void vox_scanner_destroy(vox_scanner_t* scanner) {
    /* 扫描器本身不分配内存，只是清理指针 */
    if (scanner) {
        scanner->begin = NULL;
        scanner->end = NULL;
        scanner->curptr = NULL;
        scanner->flags = 0;
    }
}

const char* vox_scanner_curptr(const vox_scanner_t* scanner) {
    return scanner ? scanner->curptr : NULL;
}

size_t vox_scanner_offset(const vox_scanner_t* scanner) {
    if (!scanner || !scanner->begin) return 0;
    return (size_t)(scanner->curptr - scanner->begin);
}

size_t vox_scanner_remaining(const vox_scanner_t* scanner) {
    if (!scanner || scanner->curptr >= scanner->end) return 0;
    return (size_t)(scanner->end - scanner->curptr);
}

bool vox_scanner_eof(const vox_scanner_t* scanner) {
    if (!scanner || !scanner->begin || !scanner->end) {
        return true;
    }
    return scanner->curptr >= scanner->end;
}

int vox_scanner_peek_char(const vox_scanner_t* scanner) {
    if (!scanner || scanner->curptr >= scanner->end) {
        return -1;
    }
    return (unsigned char)*scanner->curptr;
}

int vox_scanner_peek_char_at(const vox_scanner_t* scanner, size_t offset) {
    if (!scanner) return -1;
    
    const char* ptr = scanner->curptr + offset;
    if (ptr >= scanner->end) {
        return -1;
    }
    return (unsigned char)*ptr;
}

int vox_scanner_peek(const vox_scanner_t* scanner, size_t len, vox_strview_t* out) {
    if (!scanner || !out) return -1;
    
    if (scanner->curptr + len > scanner->end) {
        len = (size_t)(scanner->end - scanner->curptr);
    }
    
    out->ptr = scanner->curptr;
    out->len = len;
    
    return 0;
}

int vox_scanner_peek_until_charset(const vox_scanner_t* scanner,
                                   const vox_charset_t* charset,
                                   bool include_match,
                                   vox_strview_t* out) {
    if (!scanner || !out) return -1;
    
    const char* start = scanner->curptr;
    const char* ptr = start;
    
    if (charset) {
        /* 查找匹配字符集的字符 */
        while (ptr < scanner->end && !vox_charset_contains(charset, *ptr)) {
            ptr++;
        }
    } else {
        /* 如果没有字符集，匹配到末尾 */
        ptr = scanner->end;
    }
    
    if (include_match && ptr < scanner->end) {
        ptr++;
    }
    
    out->ptr = start;
    out->len = (size_t)(ptr - start);
    
    return 0;
}

int vox_scanner_peek_until_char(const vox_scanner_t* scanner,
                                char ch,
                                bool include_match,
                                vox_strview_t* out) {
    if (!scanner || !out) return -1;
    
    const char* start = scanner->curptr;
    const char* ptr = start;
    
    /* 查找匹配字符 */
    while (ptr < scanner->end && *ptr != ch) {
        ptr++;
    }
    
    if (include_match && ptr < scanner->end) {
        ptr++;
    }
    
    out->ptr = start;
    out->len = (size_t)(ptr - start);
    
    return 0;
}

int vox_scanner_peek_until_str(const vox_scanner_t* scanner,
                               const char* str,
                               bool include_match,
                               vox_strview_t* out) {
    if (!scanner || !str || !out) return -1;
    
    const char* start = scanner->curptr;
    size_t str_len = strlen(str);
    
    if (str_len == 0) {
        out->ptr = start;
        out->len = 0;
        return 0;
    }
    
    const char* ptr = start;
    const char* search_end = scanner->end - str_len + 1;
    
    /* 简单字符串搜索 */
    while (ptr < search_end) {
        if (memcmp(ptr, str, str_len) == 0) {
            if (include_match) {
                ptr += str_len;
            }
            out->ptr = start;
            out->len = (size_t)(ptr - start);
            return 0;
        }
        ptr++;
    }
    
    /* 未找到，返回到末尾 */
    out->ptr = start;
    out->len = (size_t)(scanner->end - start);
    return 0;
}

int vox_scanner_get_char(vox_scanner_t* scanner) {
    if (!scanner || scanner->curptr >= scanner->end) {
        return -1;
    }
    
    int ch = (unsigned char)*scanner->curptr;
    scanner->curptr++;
    
    /* 应用自动跳过选项 */
    auto_skip(scanner);
    
    return ch;
}

int vox_scanner_get(vox_scanner_t* scanner, size_t len, vox_strview_t* out) {
    if (!scanner || !out) return -1;
    
    if (vox_scanner_peek(scanner, len, out) != 0) {
        return -1;
    }
    
    scanner->curptr += out->len;
    
    /* 应用自动跳过选项 */
    auto_skip(scanner);
    
    return 0;
}

int vox_scanner_get_until_charset(vox_scanner_t* scanner,
                                  const vox_charset_t* charset,
                                  bool include_match,
                                  vox_strview_t* out) {
    if (!scanner || !out) return -1;
    
    if (vox_scanner_peek_until_charset(scanner, charset, include_match, out) != 0) {
        return -1;
    }
    
    scanner->curptr += out->len;
    
    /* 应用自动跳过选项 */
    auto_skip(scanner);
    
    return 0;
}

int vox_scanner_get_until_char(vox_scanner_t* scanner,
                               char ch,
                               bool include_match,
                               vox_strview_t* out) {
    if (!scanner || !out) return -1;
    
    if (vox_scanner_peek_until_char(scanner, ch, include_match, out) != 0) {
        return -1;
    }
    
    scanner->curptr += out->len;
    
    /* 应用自动跳过选项 */
    auto_skip(scanner);
    
    return 0;
}

int vox_scanner_get_until_str(vox_scanner_t* scanner,
                              const char* str,
                              bool include_match,
                              vox_strview_t* out) {
    if (!scanner || !out) return -1;
    
    if (vox_scanner_peek_until_str(scanner, str, include_match, out) != 0) {
        return -1;
    }
    
    scanner->curptr += out->len;
    
    /* 应用自动跳过选项 */
    auto_skip(scanner);
    
    return 0;
}

int vox_scanner_get_charset(vox_scanner_t* scanner,
                            const vox_charset_t* charset,
                            vox_strview_t* out) {
    if (!scanner || !charset || !out) return -1;
    
    const char* start = scanner->curptr;
    const char* ptr = start;
    
    /* 获取所有在字符集中的连续字符 */
    while (ptr < scanner->end && vox_charset_contains(charset, *ptr)) {
        ptr++;
    }
    
    out->ptr = start;
    out->len = (size_t)(ptr - start);
    
    scanner->curptr = (char*)ptr;
    
    /* 应用自动跳过选项 */
    auto_skip(scanner);
    
    return 0;
}

int vox_scanner_skip(vox_scanner_t* scanner, size_t count) {
    if (!scanner) return -1;
    
    size_t remaining = vox_scanner_remaining(scanner);
    if (count > remaining) {
        count = remaining;
    }
    
    scanner->curptr += count;
    
    /* 应用自动跳过选项 */
    auto_skip(scanner);
    
    return 0;
}

size_t vox_scanner_skip_charset(vox_scanner_t* scanner, const vox_charset_t* charset) {
    if (!scanner || !charset) return 0;
    
    size_t skipped = 0;
    while (scanner->curptr < scanner->end && 
           vox_charset_contains(charset, *scanner->curptr)) {
        scanner->curptr++;
        skipped++;
    }
    
    /* 应用自动跳过选项 */
    auto_skip(scanner);
    
    return skipped;
}

size_t vox_scanner_skip_ws(vox_scanner_t* scanner) {
    if (!scanner) return 0;
    
    size_t skipped = 0;
    while (scanner->curptr < scanner->end && is_whitespace(*scanner->curptr)) {
        scanner->curptr++;
        skipped++;
    }
    
    return skipped;
}

size_t vox_scanner_skip_newline(vox_scanner_t* scanner) {
    if (!scanner) return 0;
    
    size_t skipped = 0;
    while (scanner->curptr < scanner->end && is_newline(*scanner->curptr)) {
        scanner->curptr++;
        skipped++;
    }
    
    return skipped;
}

int vox_scanner_save_state(const vox_scanner_t* scanner, vox_scanner_state_t* state) {
    if (!scanner || !state) return -1;
    
    state->curptr = scanner->curptr;
    return 0;
}

int vox_scanner_restore_state(vox_scanner_t* scanner, const vox_scanner_state_t* state) {
    if (!scanner || !state) return -1;
    
    /* 确保恢复的位置在有效范围内 */
    if (state->curptr < scanner->begin || state->curptr > scanner->end) {
        return -1;
    }
    
    scanner->curptr = (char*)state->curptr;
    return 0;
}

/* ===== 流式扫描器实现（零拷贝模式） ===== */

/* 临时缓冲区默认容量（用于跨片段匹配） */
#define VOX_SCANNER_STREAM_TEMP_BUFFER_SIZE 4096

int vox_scanner_stream_init(vox_scanner_stream_t* stream, 
                            void* mpool,
                            int flags) {
    if (!stream || !mpool) return -1;
    
    memset(stream, 0, sizeof(vox_scanner_stream_t));
    
    stream->mpool = mpool;
    stream->flags = flags;
    
    /* 初始化基础扫描器（初始为空） */
    stream->scanner.begin = NULL;
    stream->scanner.end = NULL;
    stream->scanner.curptr = NULL;
    stream->scanner.flags = flags;
    
    return 0;
}

void vox_scanner_stream_destroy(vox_scanner_stream_t* stream) {
    if (!stream) return;
    
    vox_mpool_t* mpool = (vox_mpool_t*)stream->mpool;
    
    /* 释放所有片段 */
    vox_scanner_chunk_t* chunk = stream->chunks;
    while (chunk) {
        vox_scanner_chunk_t* next = chunk->next;
        vox_mpool_free(mpool, chunk);
        chunk = next;
    }
    
    /* 释放临时缓冲区 */
    if (stream->temp_buffer) {
        vox_mpool_free(mpool, stream->temp_buffer);
        stream->temp_buffer = NULL;
    }
    
    vox_scanner_destroy(&stream->scanner);
    
    memset(stream, 0, sizeof(vox_scanner_stream_t));
}

int vox_scanner_stream_feed(vox_scanner_stream_t* stream, 
                            const void* data, 
                            size_t len) {
    if (!stream || !data || len == 0) return -1;
    
    vox_mpool_t* mpool = (vox_mpool_t*)stream->mpool;
    
    /* 创建新的片段（零拷贝，不复制数据） */
    vox_scanner_chunk_t* chunk = (vox_scanner_chunk_t*)vox_mpool_alloc(mpool, sizeof(vox_scanner_chunk_t));
    if (!chunk) {
        return -1;
    }
    
    chunk->data = (char*)data;
    chunk->len = len;
    chunk->next = NULL;
    
    /* 添加到链表末尾 */
    if (stream->chunks_tail) {
        stream->chunks_tail->next = chunk;
    } else {
        stream->chunks = chunk;
    }
    stream->chunks_tail = chunk;
    
    stream->total_size += len;
    
    /* 自动更新视图 */
    return vox_scanner_stream_update_view(stream);
}

int vox_scanner_stream_consume(vox_scanner_stream_t* stream, size_t bytes) {
    if (!stream) return -1;
    
    vox_mpool_t* mpool = (vox_mpool_t*)stream->mpool;
    
    /* 计算已扫描的字节数 */
    size_t scanned = 0;
    if (stream->scanner.begin && stream->scanner.curptr) {
        scanned = (size_t)(stream->scanner.curptr - stream->scanner.begin);
    }
    
    /* 不能消费超过已扫描的字节数 */
    if (bytes > scanned) {
        return -1;
    }
    
    /* 从链表头部移除已消费的片段 */
    size_t remaining = bytes;
    while (remaining > 0 && stream->chunks) {
        vox_scanner_chunk_t* chunk = stream->chunks;
        
        if (chunk->len <= remaining) {
            /* 整个片段都被消费了 */
            remaining -= chunk->len;
            stream->total_size -= chunk->len;
            
            stream->chunks = chunk->next;
            if (stream->chunks_tail == chunk) {
                stream->chunks_tail = NULL;
            }
            
            vox_mpool_free(mpool, chunk);
        } else {
            /* 只消费片段的一部分（这种情况不应该发生，因为consume应该按片段边界） */
            /* 但为了健壮性，我们支持这种情况 */
            chunk->data += remaining;
            chunk->len -= remaining;
            stream->total_size -= remaining;
            remaining = 0;
        }
    }
    
    /* 更新视图 */
    return vox_scanner_stream_update_view(stream);
}

vox_scanner_t* vox_scanner_stream_get_scanner(vox_scanner_stream_t* stream) {
    return stream ? &stream->scanner : NULL;
}

size_t vox_scanner_stream_get_size(const vox_scanner_stream_t* stream) {
    if (!stream) return 0;
    return stream->total_size;
}

void vox_scanner_stream_reset(vox_scanner_stream_t* stream) {
    if (!stream) return;
    
    vox_mpool_t* mpool = (vox_mpool_t*)stream->mpool;
    
    /* 释放所有片段 */
    vox_scanner_chunk_t* chunk = stream->chunks;
    while (chunk) {
        vox_scanner_chunk_t* next = chunk->next;
        vox_mpool_free(mpool, chunk);
        chunk = next;
    }
    
    stream->chunks = NULL;
    stream->chunks_tail = NULL;
    stream->total_size = 0;
    
    /* 释放临时缓冲区 */
    if (stream->temp_buffer) {
        vox_mpool_free(mpool, stream->temp_buffer);
        stream->temp_buffer = NULL;
        stream->temp_buffer_size = 0;
        stream->temp_buffer_capacity = 0;
    }
    
    stream->scanner.begin = NULL;
    stream->scanner.end = NULL;
    stream->scanner.curptr = NULL;
}

/* 更新扫描器视图：如果数据在单个片段内，直接使用；如果跨片段，合并到临时缓冲区 */
int vox_scanner_stream_update_view(vox_scanner_stream_t* stream) {
    if (!stream) return -1;
    
    vox_mpool_t* mpool = (vox_mpool_t*)stream->mpool;
    
    /* 如果没有片段，清空视图 */
    if (!stream->chunks) {
        stream->scanner.begin = NULL;
        stream->scanner.end = NULL;
        stream->scanner.curptr = NULL;
        return 0;
    }
    
    /* 检查是否只有一个片段 */
    if (!stream->chunks->next) {
        /* 单个片段：直接使用，零拷贝 */
        char* chunk_data = stream->chunks->data;
        stream->scanner.begin = chunk_data;
        stream->scanner.end = chunk_data + stream->chunks->len;
        /* 如果curptr未设置或不在有效范围内，设置为begin */
        if (!stream->scanner.curptr || 
            stream->scanner.curptr < stream->scanner.begin || 
            stream->scanner.curptr > stream->scanner.end) {
            stream->scanner.curptr = stream->scanner.begin;
        }
        return 0;
    }
    
    /* 多个片段：需要合并到临时缓冲区 */
    size_t total_len = stream->total_size;
    
    /* 扩展临时缓冲区（如果需要） */
    if (total_len > stream->temp_buffer_capacity) {
        size_t new_capacity = stream->temp_buffer_capacity;
        if (new_capacity == 0) {
            new_capacity = VOX_SCANNER_STREAM_TEMP_BUFFER_SIZE;
        }
        while (new_capacity < total_len) {
            size_t old_capacity = new_capacity;
            new_capacity = old_capacity + (old_capacity >> 1);  /* 1.5倍 */
            if (new_capacity < old_capacity) {
                return -1;  /* 溢出 */
            }
        }
        
        char* new_buffer = (char*)vox_mpool_alloc(mpool, new_capacity);
        if (!new_buffer) {
            return -1;
        }
        
        if (stream->temp_buffer) {
            /* 保存当前curptr的偏移量 */
            size_t curptr_offset = 0;
            if (stream->scanner.curptr && stream->scanner.begin) {
                curptr_offset = (size_t)(stream->scanner.curptr - stream->scanner.begin);
            }
            
            /* 复制旧数据 */
            if (curptr_offset < stream->temp_buffer_size) {
                memcpy(new_buffer, stream->temp_buffer, curptr_offset);
            }
            
            vox_mpool_free(mpool, stream->temp_buffer);
            stream->temp_buffer = new_buffer;
            stream->temp_buffer_capacity = new_capacity;
        } else {
            stream->temp_buffer = new_buffer;
            stream->temp_buffer_capacity = new_capacity;
        }
    }
    
    /* 合并所有片段到临时缓冲区 */
    char* dst = stream->temp_buffer;
    size_t curptr_offset = 0;
    if (stream->scanner.curptr && stream->scanner.begin) {
        curptr_offset = (size_t)(stream->scanner.curptr - stream->scanner.begin);
    }
    
    vox_scanner_chunk_t* chunk = stream->chunks;
    while (chunk) {
        memcpy(dst, chunk->data, chunk->len);
        dst += chunk->len;
        chunk = chunk->next;
    }
    
    stream->temp_buffer_size = total_len;
    
    /* 更新扫描器 */
    stream->scanner.begin = stream->temp_buffer;
    stream->scanner.end = stream->temp_buffer + stream->temp_buffer_size;
    stream->scanner.curptr = stream->temp_buffer + curptr_offset;
    
    return 0;
}

bool vox_scanner_stream_check_partial_match(vox_scanner_stream_t* stream,
                                            const char* str,
                                            size_t* partial_match_len) {
    if (!stream || !str || !partial_match_len) {
        if (partial_match_len) *partial_match_len = 0;
        return false;
    }
    
    size_t str_len = strlen(str);
    if (str_len == 0) {
        *partial_match_len = 0;
        return true;
    }
    
    /* 获取当前可扫描的数据 */
    const char* data_start = stream->scanner.curptr;
    const char* data_end = stream->scanner.end;
    
    if (!data_start || !data_end || data_start >= data_end) {
        *partial_match_len = 0;
        return false;
    }
    
    size_t data_len = (size_t)(data_end - data_start);
    
    /* 如果数据长度大于等于字符串长度，不需要部分匹配检查 */
    if (data_len >= str_len) {
        *partial_match_len = 0;
        return true;
    }
    
    /* 检查数据末尾是否与字符串开头部分匹配 */
    /* 使用KMP算法的思想，检查所有可能的前缀匹配 */
    size_t max_partial = data_len < str_len ? data_len : str_len;
    
    for (size_t len = max_partial; len > 0; len--) {
        /* 检查数据末尾的len个字符是否与字符串开头的len个字符匹配 */
        if (memcmp(data_end - len, str, len) == 0) {
            *partial_match_len = len;
            return true;
        }
    }
    
    *partial_match_len = 0;
    return false;
}
