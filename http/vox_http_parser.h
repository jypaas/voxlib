/*
 * vox_http_parser.h - 高性能 HTTP 解析器
 * 
 * 使用流程:
 * 1. 创建解析器：vox_http_parser_create()
 * 2. 喂入数据：循环调用 vox_http_parser_execute()
 * 3. 检查完成：vox_http_parser_is_complete()
 * 4. 查询结果：vox_http_parser_get_method/status_code/content_length等
 * 5. 重置/销毁：vox_http_parser_reset() 或 vox_http_parser_destroy()
 */

#ifndef VOX_HTTP_PARSER_H
#define VOX_HTTP_PARSER_H

#include "../vox_os.h"
#include "../vox_mpool.h"
#include "../vox_string.h"
#include "../vox_list.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== HTTP 解析器类型 ===== */

/**
 * HTTP 解析器类型
 */
typedef enum {
    VOX_HTTP_PARSER_TYPE_BOTH = 0,    /* 自动检测请求或响应 */
    VOX_HTTP_PARSER_TYPE_REQUEST,      /* 仅解析请求 */
    VOX_HTTP_PARSER_TYPE_RESPONSE     /* 仅解析响应 */
} vox_http_parser_type_t;

/* ===== HTTP 方法 ===== */

/**
 * HTTP 方法枚举
 */
typedef enum {
    VOX_HTTP_METHOD_UNKNOWN = 0,
    VOX_HTTP_METHOD_GET,
    VOX_HTTP_METHOD_HEAD,
    VOX_HTTP_METHOD_POST,
    VOX_HTTP_METHOD_PUT,
    VOX_HTTP_METHOD_DELETE,
    VOX_HTTP_METHOD_CONNECT,
    VOX_HTTP_METHOD_OPTIONS,
    VOX_HTTP_METHOD_TRACE,
    VOX_HTTP_METHOD_PATCH
} vox_http_method_t;

/* ===== 回调函数类型 ===== */

/**
 * 消息开始回调
 * @param parser 解析器指针
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_http_on_message_begin_cb)(void* parser);

/**
 * URL 回调（数据回调，可能多次调用）
 * @param parser 解析器指针
 * @param data URL 数据块
 * @param len 数据块长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_http_on_url_cb)(void* parser, const char* data, size_t len);

/**
 * 状态行回调（仅响应，数据回调，可能多次调用）
 * @param parser 解析器指针
 * @param data 原因短语数据块
 * @param len 数据块长度
 * @return 成功返回0，失败返回-1（停止解析）
 * @note 状态码通过 vox_http_parser_get_status_code() 获取
 */
typedef int (*vox_http_on_status_cb)(void* parser, const char* data, size_t len);

/**
 * 头部字段名回调（数据回调，可能多次调用）
 * @param parser 解析器指针
 * @param data 头部字段名数据块
 * @param len 数据块长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_http_on_header_field_cb)(void* parser, const char* data, size_t len);

/**
 * 头部字段值回调（数据回调，可能多次调用）
 * @param parser 解析器指针
 * @param data 头部字段值数据块
 * @param len 数据块长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_http_on_header_value_cb)(void* parser, const char* data, size_t len);

/**
 * 头部解析完成回调
 * @param parser 解析器指针
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_http_on_headers_complete_cb)(void* parser);

/**
 * 消息体回调
 * @param parser 解析器指针
 * @param data 消息体数据
 * @param len 数据长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_http_on_body_cb)(void* parser, const char* data, size_t len);

/**
 * 消息完成回调
 * @param parser 解析器指针
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_http_on_message_complete_cb)(void* parser);

/**
 * 错误回调
 * @param parser 解析器指针
 * @param message 错误消息
 * @return 成功返回0，失败返回-1
 */
typedef int (*vox_http_on_error_cb)(void* parser, const char* message);

/* ===== 回调结构 ===== */

/**
 * HTTP 解析器回调结构
 */
typedef struct {
    vox_http_on_message_begin_cb on_message_begin;
    vox_http_on_url_cb on_url;
    vox_http_on_status_cb on_status;
    vox_http_on_header_field_cb on_header_field;
    vox_http_on_header_value_cb on_header_value;
    vox_http_on_headers_complete_cb on_headers_complete;
    vox_http_on_body_cb on_body;
    vox_http_on_message_complete_cb on_message_complete;
    vox_http_on_error_cb on_error;
    void* user_data;  /* 用户数据指针 */
} vox_http_callbacks_t;

/* ===== HTTP 解析器配置 ===== */

/**
 * HTTP 解析器配置
 */
typedef struct {
    vox_http_parser_type_t type;      /* 解析器类型 */
    size_t max_header_size;           /* 最大头部大小（0表示无限制） */
    size_t max_headers;                /* 最大头部数量（0表示无限制） */
    size_t max_url_size;               /* 最大 URL 大小（0表示无限制） */
    bool strict_mode;                  /* 严格模式（默认 false） */
} vox_http_parser_config_t;

/* ===== HTTP 解析器 ===== */

/**
 * HTTP 解析器不透明类型
 */
typedef struct vox_http_parser vox_http_parser_t;

/**
 * 创建 HTTP 解析器
 * @param mpool 内存池指针，必须非NULL
 * @param config 配置结构体，NULL表示使用默认配置
 * @param callbacks 回调结构体，NULL表示无回调
 * @return 成功返回解析器指针，失败返回NULL
 */
vox_http_parser_t* vox_http_parser_create(vox_mpool_t* mpool,
                                          const vox_http_parser_config_t* config,
                                          const vox_http_callbacks_t* callbacks);

/**
 * 销毁 HTTP 解析器
 * @param parser 解析器指针
 */
void vox_http_parser_destroy(vox_http_parser_t* parser);

/**
 * 执行解析
 * @param parser 解析器指针
 * @param data 输入数据
 * @param len 数据长度
 * @return 成功返回已消费（解析）的字节数，失败返回-1
 * @note 返回值可能小于 len：例如解析完成一个完整消息后停止，以便上层继续处理 data+ret 的剩余字节（HTTP pipeline）。
 * @note len==0 时返回 0（此时 data 可以为 NULL）。
 */
ssize_t vox_http_parser_execute(vox_http_parser_t* parser, const char* data, size_t len);

/**
 * 重置解析器状态（用于解析下一个消息）
 * @param parser 解析器指针
 */
void vox_http_parser_reset(vox_http_parser_t* parser);

/**
 * 检查解析器是否完成
 * @param parser 解析器指针
 * @return 完成返回true，否则返回false
 */
bool vox_http_parser_is_complete(const vox_http_parser_t* parser);

/**
 * 检查解析器是否出错
 * @param parser 解析器指针
 * @return 出错返回true，否则返回false
 */
bool vox_http_parser_has_error(const vox_http_parser_t* parser);

/**
 * 获取错误消息
 * @param parser 解析器指针
 * @return 返回错误消息，无错误返回NULL
 */
const char* vox_http_parser_get_error(const vox_http_parser_t* parser);

/* ===== 解析结果查询 ===== */

/**
 * 获取 HTTP 方法（仅请求）
 * @param parser 解析器指针
 * @return 返回 HTTP 方法
 */
vox_http_method_t vox_http_parser_get_method(const vox_http_parser_t* parser);

/**
 * 获取 HTTP 版本主版本号
 * @param parser 解析器指针
 * @return 返回主版本号（如 1）
 */
int vox_http_parser_get_http_major(const vox_http_parser_t* parser);

/**
 * 获取 HTTP 版本次版本号
 * @param parser 解析器指针
 * @return 返回次版本号（如 1）
 */
int vox_http_parser_get_http_minor(const vox_http_parser_t* parser);

/**
 * 获取状态码（仅响应）
 * @param parser 解析器指针
 * @return 返回状态码
 */
int vox_http_parser_get_status_code(const vox_http_parser_t* parser);

/**
 * 获取 Content-Length（若不存在则为 0）
 * @param parser 解析器指针
 * @return 返回 Content-Length
 */
uint64_t vox_http_parser_get_content_length(const vox_http_parser_t* parser);

/**
 * 是否为 chunked 传输编码
 * @param parser 解析器指针
 * @return 是 chunked 返回 true，否则返回 false
 */
bool vox_http_parser_is_chunked(const vox_http_parser_t* parser);

/**
 * Connection: close 是否出现过
 * @param parser 解析器指针
 * @return 出现过返回 true，否则返回 false
 */
bool vox_http_parser_is_connection_close(const vox_http_parser_t* parser);

/**
 * Connection: keep-alive 是否出现过
 * @param parser 解析器指针
 * @return 出现过返回 true，否则返回 false
 */
bool vox_http_parser_is_connection_keep_alive(const vox_http_parser_t* parser);

/**
 * 检查本次消息头是否包含 Upgrade 头
 * @param parser 解析器指针
 * @return 若出现过 Upgrade 头名则返回 true，否则返回 false
 * @note 仅判断是否存在 Upgrade 头名，不解析其具体值（如 websocket）。
 */
bool vox_http_parser_is_upgrade(const vox_http_parser_t* parser);

/**
 * 获取用户数据
 * @param parser 解析器指针
 * @return 返回用户数据指针
 */
void* vox_http_parser_get_user_data(const vox_http_parser_t* parser);

/**
 * 设置用户数据
 * @param parser 解析器指针
 * @param user_data 用户数据指针
 */
void vox_http_parser_set_user_data(vox_http_parser_t* parser, void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_PARSER_H */
