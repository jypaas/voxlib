/*
 * vox_redis_parser.h - RESP (REdis Serialization Protocol) 解析器
 * 支持流式解析，零拷贝设计
 */

#ifndef VOX_REDIS_PARSER_H
#define VOX_REDIS_PARSER_H

#include "../vox_os.h"
#include "../vox_mpool.h"
#include "../vox_string.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== RESP 数据类型 ===== */

typedef enum {
    VOX_REDIS_TYPE_SIMPLE_STRING = 0,  /* +OK\r\n */
    VOX_REDIS_TYPE_ERROR,              /* -ERR message\r\n */
    VOX_REDIS_TYPE_INTEGER,            /* :1234\r\n */
    VOX_REDIS_TYPE_BULK_STRING,        /* $5\r\nhello\r\n 或 $-1\r\n */
    VOX_REDIS_TYPE_ARRAY               /* *2\r\n$3\r\nGET\r\n$3\r\nkey\r\n */
} vox_redis_type_t;

/* ===== 回调函数类型 ===== */

/**
 * Simple String 回调
 * @param parser 解析器指针
 * @param data 字符串数据（不包含\r\n）
 * @param len 数据长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_redis_on_simple_string_cb)(void* parser, const char* data, size_t len);

/**
 * Error 回调
 * @param parser 解析器指针
 * @param data 错误消息数据（不包含\r\n）
 * @param len 数据长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_redis_on_error_cb)(void* parser, const char* data, size_t len);

/**
 * Integer 回调
 * @param parser 解析器指针
 * @param value 整数值
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_redis_on_integer_cb)(void* parser, int64_t value);

/**
 * Bulk String 开始回调（在读取长度后调用）
 * @param parser 解析器指针
 * @param len 字符串长度（-1表示NULL）
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_redis_on_bulk_string_start_cb)(void* parser, int64_t len);

/**
 * Bulk String 数据回调（可能多次调用）
 * @param parser 解析器指针
 * @param data 字符串数据块
 * @param len 数据块长度
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_redis_on_bulk_string_data_cb)(void* parser, const char* data, size_t len);

/**
 * Bulk String 完成回调
 * @param parser 解析器指针
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_redis_on_bulk_string_complete_cb)(void* parser);

/**
 * Array 开始回调（在读取元素数量后调用）
 * @param parser 解析器指针
 * @param count 元素数量（-1表示NULL数组）
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_redis_on_array_start_cb)(void* parser, int64_t count);

/**
 * Array 元素开始回调（每个元素解析前调用）
 * @param parser 解析器指针
 * @param index 元素索引（从0开始）
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_redis_on_array_element_start_cb)(void* parser, size_t index);

/**
 * Array 元素完成回调（每个元素解析后调用）
 * @param parser 解析器指针
 * @param index 元素索引（从0开始）
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_redis_on_array_element_complete_cb)(void* parser, size_t index);

/**
 * Array 完成回调
 * @param parser 解析器指针
 * @return 成功返回0，失败返回-1（停止解析）
 */
typedef int (*vox_redis_on_array_complete_cb)(void* parser);

/**
 * 解析完成回调（整个RESP对象解析完成）
 * @param parser 解析器指针
 * @return 成功返回0，失败返回-1
 */
typedef int (*vox_redis_on_complete_cb)(void* parser);

/**
 * 错误回调
 * @param parser 解析器指针
 * @param message 错误消息
 * @return 成功返回0，失败返回-1
 */
typedef int (*vox_redis_on_parse_error_cb)(void* parser, const char* message);

/* ===== 回调结构 ===== */

typedef struct {
    vox_redis_on_simple_string_cb on_simple_string;
    vox_redis_on_error_cb on_error;
    vox_redis_on_integer_cb on_integer;
    vox_redis_on_bulk_string_start_cb on_bulk_string_start;
    vox_redis_on_bulk_string_data_cb on_bulk_string_data;
    vox_redis_on_bulk_string_complete_cb on_bulk_string_complete;
    vox_redis_on_array_start_cb on_array_start;
    vox_redis_on_array_element_start_cb on_array_element_start;
    vox_redis_on_array_element_complete_cb on_array_element_complete;
    vox_redis_on_array_complete_cb on_array_complete;
    vox_redis_on_complete_cb on_complete;
    vox_redis_on_parse_error_cb on_error_parse;
    void* user_data;  /* 用户数据指针 */
} vox_redis_parser_callbacks_t;

/* ===== 解析器配置 ===== */

typedef struct {
    size_t max_bulk_string_size;  /* 最大bulk string大小，0表示无限制 */
    size_t max_array_size;       /* 最大数组大小，0表示无限制 */
    size_t max_nesting_depth;   /* 最大嵌套深度，0表示无限制 */
} vox_redis_parser_config_t;

/* ===== 解析器 ===== */

typedef struct vox_redis_parser vox_redis_parser_t;

/**
 * 创建RESP解析器
 * @param mpool 内存池
 * @param config 配置（可为NULL使用默认配置）
 * @param callbacks 回调函数
 * @return 成功返回解析器指针，失败返回NULL
 */
vox_redis_parser_t* vox_redis_parser_create(vox_mpool_t* mpool,
                                            const vox_redis_parser_config_t* config,
                                            const vox_redis_parser_callbacks_t* callbacks);

/**
 * 销毁解析器
 * @param parser 解析器指针
 */
void vox_redis_parser_destroy(vox_redis_parser_t* parser);

/**
 * 重置解析器状态（用于解析新的RESP对象）
 * @param parser 解析器指针
 */
void vox_redis_parser_reset(vox_redis_parser_t* parser);

/**
 * 执行解析（流式输入）
 * @param parser 解析器指针
 * @param data 输入数据
 * @param len 数据长度
 * @return 成功返回已解析的字节数，失败返回-1
 * @note 可以多次调用，每次传入新的数据块
 */
ssize_t vox_redis_parser_execute(vox_redis_parser_t* parser, const char* data, size_t len);

/**
 * 检查解析是否完成
 * @param parser 解析器指针
 * @return 完成返回true，否则返回false
 */
bool vox_redis_parser_is_complete(const vox_redis_parser_t* parser);

/**
 * 检查是否有错误
 * @param parser 解析器指针
 * @return 有错误返回true，否则返回false
 */
bool vox_redis_parser_has_error(const vox_redis_parser_t* parser);

/**
 * 获取错误消息
 * @param parser 解析器指针
 * @return 错误消息字符串，无错误返回NULL
 */
const char* vox_redis_parser_get_error(const vox_redis_parser_t* parser);

/**
 * 获取用户数据
 * @param parser 解析器指针
 * @return 用户数据指针
 */
void* vox_redis_parser_get_user_data(const vox_redis_parser_t* parser);

/**
 * 设置用户数据
 * @param parser 解析器指针
 * @param user_data 用户数据指针
 */
void vox_redis_parser_set_user_data(vox_redis_parser_t* parser, void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* VOX_REDIS_PARSER_H */
