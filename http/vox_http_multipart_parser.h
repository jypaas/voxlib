/*
 * vox_http_multipart_parser.h - 高性能 HTTP Multipart 解析器
 * 参考 vox_http_parser 设计，使用状态机实现零拷贝解析
 * 支持 HTTP multipart/form-data 和 multipart/mixed 格式
 */

#ifndef VOX_HTTP_MULTIPART_PARSER_H
#define VOX_HTTP_MULTIPART_PARSER_H

#include "../vox_os.h"
#include "../vox_mpool.h"
#include "../vox_vector.h"
#include "../vox_string.h"
#include "../vox_list.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 回调函数类型 ===== */

/**
 * Part 开始回调
 * @param parser 解析器指针
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_multipart_on_part_begin_cb)(void* parser);

/**
 * Part 头部字段名回调（数据回调，可能多次调用）
 * @param parser 解析器指针
 * @param data 头部字段名数据块
 * @param len 数据块长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_multipart_on_header_field_cb)(void* parser, const char* data, size_t len);

/**
 * Part 头部字段值回调（数据回调，可能多次调用）
 * @param parser 解析器指针
 * @param data 头部字段值数据块
 * @param len 数据块长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_multipart_on_header_value_cb)(void* parser, const char* data, size_t len);

/**
 * Content-Disposition name 回调（数据回调，可能多次调用）
 * @param parser 解析器指针
 * @param data name 数据块
 * @param len 数据块长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_multipart_on_name_cb)(void* parser, const char* data, size_t len);

/**
 * Content-Disposition filename 回调（数据回调，可能多次调用）
 * @param parser 解析器指针
 * @param data filename 数据块
 * @param len 数据块长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_multipart_on_filename_cb)(void* parser, const char* data, size_t len);

/**
 * Part 头部解析完成回调
 * @param parser 解析器指针
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_multipart_on_headers_complete_cb)(void* parser);

/**
 * Part 数据回调
 * @param parser 解析器指针
 * @param data 数据指针
 * @param len 数据长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_multipart_on_part_data_cb)(void* parser, const char* data, size_t len);

/**
 * Part 完成回调
 * @param parser 解析器指针
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_multipart_on_part_complete_cb)(void* parser);

/**
 * Multipart 完成回调
 * @param parser 解析器指针
 * @return 成功返回0，失败返回-1
 */
typedef int (*vox_multipart_on_complete_cb)(void* parser);

/**
 * 错误回调
 * @param parser 解析器指针
 * @param message 错误消息
 * @return 成功返回0，失败返回-1
 */
typedef int (*vox_multipart_on_error_cb)(void* parser, const char* message);

/* ===== 回调结构 ===== */

/**
 * Multipart 解析器回调结构
 */
typedef struct {
    vox_multipart_on_part_begin_cb on_part_begin;
    vox_multipart_on_header_field_cb on_header_field;
    vox_multipart_on_header_value_cb on_header_value;
    vox_multipart_on_name_cb on_name;
    vox_multipart_on_filename_cb on_filename;
    vox_multipart_on_headers_complete_cb on_headers_complete;
    vox_multipart_on_part_data_cb on_part_data;
    vox_multipart_on_part_complete_cb on_part_complete;
    vox_multipart_on_complete_cb on_complete;
    vox_multipart_on_error_cb on_error;
    void* user_data;  /* 用户数据指针 */
} vox_multipart_callbacks_t;

/* ===== Multipart 解析器配置 ===== */

/**
 * Multipart 解析器配置
 */
typedef struct {
    size_t max_header_size;           /* 最大头部大小（0表示无限制） */
    size_t max_headers;                /* 最大头部数量（0表示无限制） */
    size_t max_field_name_size;       /* 最大字段名大小（0表示无限制） */
    size_t max_filename_size;         /* 最大文件名大小（0表示无限制） */
    bool strict_mode;                  /* 严格模式（默认 false） */
} vox_multipart_parser_config_t;

/* ===== Multipart 解析器 ===== */

/**
 * Multipart 解析器不透明类型
 */
typedef struct vox_multipart_parser vox_multipart_parser_t;

/**
 * 创建 Multipart 解析器
 * @param mpool 内存池指针，必须非NULL
 * @param boundary boundary 字符串（不含前导 "--"）
 * @param boundary_len boundary 长度
 * @param config 配置结构体，NULL表示使用默认配置
 * @param callbacks 回调结构体，NULL表示无回调
 * @return 成功返回解析器指针，失败返回NULL
 */
vox_multipart_parser_t* vox_multipart_parser_create(vox_mpool_t* mpool,
                                                     const char* boundary,
                                                     size_t boundary_len,
                                                     const vox_multipart_parser_config_t* config,
                                                     const vox_multipart_callbacks_t* callbacks);

/**
 * 销毁 Multipart 解析器
 * @param parser 解析器指针
 */
void vox_multipart_parser_destroy(vox_multipart_parser_t* parser);

/**
 * 执行解析
 * @param parser 解析器指针
 * @param data 输入数据
 * @param len 数据长度
 * @return 成功返回已消费（解析）的字节数，失败返回-1
 * @note len==0 时返回 0（此时 data 可以为 NULL）。
 */
ssize_t vox_multipart_parser_execute(vox_multipart_parser_t* parser, const char* data, size_t len);

/**
 * 重置解析器状态（用于解析下一个 multipart 消息）
 * @param parser 解析器指针
 */
void vox_multipart_parser_reset(vox_multipart_parser_t* parser);

/**
 * 检查解析器是否完成
 * @param parser 解析器指针
 * @return 完成返回true，否则返回false
 */
bool vox_multipart_parser_is_complete(const vox_multipart_parser_t* parser);

/**
 * 检查解析器是否出错
 * @param parser 解析器指针
 * @return 出错返回true，否则返回false
 */
bool vox_multipart_parser_has_error(const vox_multipart_parser_t* parser);

/**
 * 获取错误消息
 * @param parser 解析器指针
 * @return 返回错误消息，无错误返回NULL
 */
const char* vox_multipart_parser_get_error(const vox_multipart_parser_t* parser);

/**
 * 获取用户数据
 * @param parser 解析器指针
 * @return 返回用户数据指针
 */
void* vox_multipart_parser_get_user_data(const vox_multipart_parser_t* parser);

/**
 * 设置用户数据
 * @param parser 解析器指针
 * @param user_data 用户数据指针
 */
void vox_multipart_parser_set_user_data(vox_multipart_parser_t* parser, void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_MULTIPART_PARSER_H */
