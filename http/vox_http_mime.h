/*
 * vox_http_mime.h - HTTP MIME 类型查询与注册
 * - 根据路径或扩展名返回 Content-Type
 * - 支持内置类型表与自定义注册（无 malloc）
 */

#ifndef VOX_HTTP_MIME_H
#define VOX_HTTP_MIME_H

#include "../vox_os.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 根据文件路径获取 MIME 类型
 * 从路径末尾解析扩展名（最后一个 '.' 之后），再查表。
 * @param path 文件路径（不必以 NUL 结尾，由 path_len 限定）
 * @param path_len 路径长度
 * @return 指向 MIME 类型字符串的指针（只读，来自内置表或自定义表），无扩展名时返回 "application/octet-stream"
 */
const char* vox_http_mime_from_path(const char* path, size_t path_len);

/**
 * 根据扩展名获取 MIME 类型
 * @param ext 扩展名字符串（如 "html"，不必含 '.'）
 * @param ext_len 扩展名长度
 * @return 指向 MIME 类型字符串的指针，未命中时返回 "application/octet-stream"
 */
const char* vox_http_mime_from_ext(const char* ext, size_t ext_len);

/**
 * 注册自定义扩展名 -> MIME 类型
 * 使用静态表，不调用 malloc。后注册的同名扩展会覆盖先前的。
 * @param ext 扩展名（以 NUL 结尾，建议小写，如 "xyz"）
 * @param mime_type MIME 类型字符串（以 NUL 结尾，如 "application/x-custom")
 * @return 成功返回 0，自定义表已满返回 -1
 */
int vox_http_mime_register(const char* ext, const char* mime_type);

/**
 * 未知类型时返回的默认 MIME 类型
 */
#define VOX_HTTP_MIME_DEFAULT "application/octet-stream"

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_MIME_H */
