/*
 * vox_http_gzip.c - HTTP gzip 压缩实现
 */

#include "vox_http_gzip.h"
#include "vox_http_internal.h"
#include "../vox_log.h"
#include <string.h>

#ifdef VOX_USE_ZLIB

int vox_http_gzip_compress(vox_mpool_t* mpool, const void* input, size_t input_len, vox_string_t* output) {
    if (!mpool || !input || input_len == 0 || !output) return -1;

    /* 初始化 zlib stream */
    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    /* 使用 gzip 格式（windowBits + 16） */
    int ret = deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                           MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        VOX_LOG_ERROR("gzip deflateInit2 failed: %d", ret);
        return -1;
    }

    /* 估算输出缓冲区大小（通常压缩后不会超过原大小） */
    size_t out_size = input_len + (input_len / 10) + 12; /* 额外空间用于 gzip 头部和尾部 */
    if (out_size < 64) out_size = 64;

    /* 分配输出缓冲区 */
    void* out_buf = vox_mpool_alloc(mpool, out_size);
    if (!out_buf) {
        deflateEnd(&zs);
        return -1;
    }

    zs.next_in = (Bytef*)input;
    zs.avail_in = (uInt)input_len;
    zs.next_out = (Bytef*)out_buf;
    zs.avail_out = (uInt)out_size;

    /* 压缩数据 */
    ret = deflate(&zs, Z_FINISH);
    if (ret != Z_STREAM_END) {
        /* 如果输出缓冲区不够，尝试更大的缓冲区 */
        if (ret == Z_OK || ret == Z_BUF_ERROR) {
            size_t new_size = out_size * 2;
            void* new_buf = vox_mpool_alloc(mpool, new_size);
            if (!new_buf) {
                vox_mpool_free(mpool, out_buf);
                deflateEnd(&zs);
                return -1;
            }
            /* 复制已压缩的数据 */
            memcpy(new_buf, out_buf, out_size - zs.avail_out);
            vox_mpool_free(mpool, out_buf);
            out_buf = new_buf;
            zs.next_out = (Bytef*)out_buf + (out_size - zs.avail_out);
            zs.avail_out = (uInt)(new_size - (out_size - zs.avail_out));
            out_size = new_size;

            ret = deflate(&zs, Z_FINISH);
        }
        if (ret != Z_STREAM_END) {
            VOX_LOG_ERROR("gzip deflate failed: %d", ret);
            vox_mpool_free(mpool, out_buf);
            deflateEnd(&zs);
            return -1;
        }
    }

    /* 获取压缩后的实际大小 */
    size_t compressed_len = out_size - zs.avail_out;

    /* 将压缩数据写入输出字符串 */
    vox_string_clear(output);
    if (vox_string_append_data(output, out_buf, compressed_len) != 0) {
        vox_mpool_free(mpool, out_buf);
        deflateEnd(&zs);
        return -1;
    }

    /* 清理 */
    vox_mpool_free(mpool, out_buf);
    deflateEnd(&zs);

    return 0;
}

int vox_http_supports_gzip(const void* headers) {
    if (!headers) return 0;

    const vox_vector_t* vec = (const vox_vector_t*)headers;
    size_t cnt = vox_vector_size(vec);
    for (size_t i = 0; i < cnt; i++) {
        const vox_http_header_t* kv = (const vox_http_header_t*)vox_vector_get(vec, i);
        if (!kv || !kv->name.ptr || !kv->value.ptr) continue;

        /* 检查 Accept-Encoding 头 */
        if (vox_http_strieq(kv->name.ptr, kv->name.len, "Accept-Encoding", 15)) {
            /* 检查值中是否包含 gzip（支持逗号分隔的编码列表，如 "gzip, deflate"） */
            if (vox_http_str_contains_token_ci(kv->value.ptr, kv->value.len, "gzip")) {
                return 1;
            }
        }
    }
    return 0;
}

int vox_http_gzip_decompress(vox_mpool_t* mpool, const void* input, size_t input_len, vox_string_t* output) {
    if (!mpool || !input || input_len == 0 || !output) return -1;

    /* 初始化 zlib stream */
    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    /* 使用 gzip 格式（windowBits + 16） */
    int ret = inflateInit2(&zs, MAX_WBITS + 16);
    if (ret != Z_OK) {
        VOX_LOG_ERROR("gzip inflateInit2 failed: %d", ret);
        return -1;
    }

    /* 估算输出缓冲区大小（通常解压缩后比压缩数据大） */
    size_t out_size = input_len * 4; /* 初始估计：压缩比通常 > 4:1 */
    if (out_size < 1024) out_size = 1024;

    /* 分配输出缓冲区 */
    void* out_buf = vox_mpool_alloc(mpool, out_size);
    if (!out_buf) {
        inflateEnd(&zs);
        return -1;
    }

    zs.next_in = (Bytef*)input;
    zs.avail_in = (uInt)input_len;
    zs.next_out = (Bytef*)out_buf;
    zs.avail_out = (uInt)out_size;

    size_t total_out = 0;

    /* 解压缩数据（循环直到完成或需要更多空间） */
    do {
        ret = inflate(&zs, Z_NO_FLUSH);
        
        if (ret == Z_STREAM_END) {
            /* 解压缩完成 */
            total_out = out_size - zs.avail_out;
            break;
        } else if (ret == Z_OK) {
            /* 需要更多输出空间 */
            if (zs.avail_out == 0) {
                size_t used = out_size - zs.avail_out;
                size_t new_size = out_size * 2;
                void* new_buf = vox_mpool_alloc(mpool, new_size);
                if (!new_buf) {
                    vox_mpool_free(mpool, out_buf);
                    inflateEnd(&zs);
                    return -1;
                }
                /* 复制已解压缩的数据 */
                memcpy(new_buf, out_buf, used);
                vox_mpool_free(mpool, out_buf);
                out_buf = new_buf;
                zs.next_out = (Bytef*)out_buf + used;
                zs.avail_out = (uInt)(new_size - used);
                out_size = new_size;
            }
        } else if (ret == Z_BUF_ERROR) {
            /* 输出缓冲区已满，需要扩展 */
            size_t used = out_size - zs.avail_out;
            size_t new_size = out_size * 2;
            void* new_buf = vox_mpool_alloc(mpool, new_size);
            if (!new_buf) {
                vox_mpool_free(mpool, out_buf);
                inflateEnd(&zs);
                return -1;
            }
            memcpy(new_buf, out_buf, used);
            vox_mpool_free(mpool, out_buf);
            out_buf = new_buf;
            zs.next_out = (Bytef*)out_buf + used;
            zs.avail_out = (uInt)(new_size - used);
            out_size = new_size;
        } else {
            /* 解压缩错误 */
            VOX_LOG_ERROR("gzip inflate failed: %d", ret);
            vox_mpool_free(mpool, out_buf);
            inflateEnd(&zs);
            return -1;
        }
    } while (ret != Z_STREAM_END);

    if (ret != Z_STREAM_END) {
        VOX_LOG_ERROR("gzip decompress incomplete");
        vox_mpool_free(mpool, out_buf);
        inflateEnd(&zs);
        return -1;
    }

    /* 将解压缩数据写入输出字符串 */
    vox_string_clear(output);
    if (vox_string_append_data(output, out_buf, total_out) != 0) {
        vox_mpool_free(mpool, out_buf);
        inflateEnd(&zs);
        return -1;
    }

    /* 清理 */
    vox_mpool_free(mpool, out_buf);
    inflateEnd(&zs);

    return 0;
}

int vox_http_is_gzip_encoded(const char* header_name, size_t header_name_len,
                              const char* header_value, size_t header_value_len) {
    if (!header_name || !header_value) return 0;
    
    /* 检查是否是 Content-Encoding 头 */
    if (vox_http_strieq(header_name, header_name_len, "Content-Encoding", 16)) {
        /* 检查值中是否包含 gzip */
        if (vox_http_str_contains_token_ci(header_value, header_value_len, "gzip")) {
            return 1;
        }
    }
    return 0;
}

#else /* VOX_USE_ZLIB */

int vox_http_gzip_compress(vox_mpool_t* mpool, const void* input, size_t input_len, vox_string_t* output) {
    VOX_UNUSED(mpool);
    VOX_UNUSED(input);
    VOX_UNUSED(input_len);
    VOX_UNUSED(output);
    return -1;
}

int vox_http_supports_gzip(const void* headers) {
    VOX_UNUSED(headers);
    return 0;
}

int vox_http_gzip_decompress(vox_mpool_t* mpool, const void* input, size_t input_len, vox_string_t* output) {
    VOX_UNUSED(mpool);
    VOX_UNUSED(input);
    VOX_UNUSED(input_len);
    VOX_UNUSED(output);
    return -1;
}

int vox_http_is_gzip_encoded(const char* header_name, size_t header_name_len,
                              const char* header_value, size_t header_value_len) {
    VOX_UNUSED(header_name);
    VOX_UNUSED(header_name_len);
    VOX_UNUSED(header_value);
    VOX_UNUSED(header_value_len);
    return 0;
}

#endif /* VOX_USE_ZLIB */
