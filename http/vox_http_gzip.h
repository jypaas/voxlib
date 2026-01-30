/*
 * vox_http_gzip.h - HTTP gzip 压缩支持
 */

#ifndef VOX_HTTP_GZIP_H
#define VOX_HTTP_GZIP_H

#include "../vox_os.h"
#include "../vox_string.h"
#include "../vox_mpool.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef VOX_USE_ZLIB
#include <zlib.h>

/**
 * 压缩数据为 gzip 格式
 * @param mpool 内存池（用于分配输出缓冲区）
 * @param input 输入数据
 * @param input_len 输入数据长度
 * @param output 输出字符串（将压缩后的数据写入）
 * @return 成功返回0，失败返回-1
 */
int vox_http_gzip_compress(vox_mpool_t* mpool, const void* input, size_t input_len, vox_string_t* output);

/**
 * 检查请求头中是否支持 gzip 编码
 * @param headers 请求头向量（vox_vector_t*，元素为 vox_http_header_t*）
 * @return 如果支持 gzip 返回1，否则返回0
 */
int vox_http_supports_gzip(const void* headers);

/**
 * 解压缩 gzip 格式的数据
 * @param mpool 内存池（用于分配输出缓冲区）
 * @param input 压缩的输入数据
 * @param input_len 输入数据长度
 * @param output 输出字符串（将解压缩后的数据写入）
 * @return 成功返回0，失败返回-1
 */
int vox_http_gzip_decompress(vox_mpool_t* mpool, const void* input, size_t input_len, vox_string_t* output);

/**
 * 检查响应头中是否包含 gzip 编码
 * @param header_name 响应头名称（大小写不敏感）
 * @param header_name_len 响应头名称长度
 * @param header_value 响应头值（大小写不敏感）
 * @param header_value_len 响应头值长度
 * @return 如果是 gzip 编码返回1，否则返回0
 */
int vox_http_is_gzip_encoded(const char* header_name, size_t header_name_len,
                              const char* header_value, size_t header_value_len);

#endif /* VOX_USE_ZLIB */

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_GZIP_H */
