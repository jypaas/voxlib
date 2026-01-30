/*
 * vox_redis_client.h - 异步 Redis 客户端
 * 基于 vox_loop 和 vox_tcp 实现
 */

#ifndef VOX_REDIS_CLIENT_H
#define VOX_REDIS_CLIENT_H

#include "../vox_os.h"
#include "../vox_loop.h"
#include "../vox_mpool.h"
#include "../vox_string.h"
#include "../vox_socket.h"
#include "../vox_tcp.h"
#include "../vox_dns.h"
#include "vox_redis_parser.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 类型定义 ===== */

typedef struct vox_redis_client vox_redis_client_t;
typedef struct vox_redis_command vox_redis_command_t;

/* ===== Redis 响应值 ===== */

typedef enum {
    VOX_REDIS_RESPONSE_SIMPLE_STRING = 0,
    VOX_REDIS_RESPONSE_ERROR,
    VOX_REDIS_RESPONSE_INTEGER,
    VOX_REDIS_RESPONSE_BULK_STRING,
    VOX_REDIS_RESPONSE_ARRAY,
    VOX_REDIS_RESPONSE_NULL
} vox_redis_response_type_t;

/* 前向声明 */
typedef struct vox_redis_response vox_redis_response_t;

struct vox_redis_response {
    vox_redis_response_type_t type;
    union {
        struct {
            const char* data;
            size_t len;
        } simple_string;
        struct {
            const char* message;
            size_t len;
        } error;
        int64_t integer;
        struct {
            const char* data;
            size_t len;
            bool is_null;
        } bulk_string;
        struct {
            size_t count;
            vox_redis_response_t* elements;  /* 动态数组 */
        } array;
    } u;
};

/* ===== 回调函数类型 ===== */

/**
 * 连接回调
 * @param client 客户端指针
 * @param status 状态（0表示成功，非0表示失败）
 * @param user_data 用户数据
 */
typedef void (*vox_redis_connect_cb)(vox_redis_client_t* client, int status, void* user_data);

/**
 * 命令响应回调
 * @param client 客户端指针
 * @param response 响应数据（仅在回调期间有效）
 * @param user_data 用户数据
 */
typedef void (*vox_redis_response_cb)(vox_redis_client_t* client, 
                                     const vox_redis_response_t* response,
                                     void* user_data);

/**
 * 错误回调
 * @param client 客户端指针
 * @param message 错误消息
 * @param user_data 用户数据
 */
typedef void (*vox_redis_error_cb)(vox_redis_client_t* client, 
                                   const char* message,
                                   void* user_data);

/* ===== 客户端 API ===== */

/**
 * 创建 Redis 客户端
 * @param loop 事件循环
 * @return 成功返回客户端指针，失败返回NULL
 */
vox_redis_client_t* vox_redis_client_create(vox_loop_t* loop);

/**
 * 销毁 Redis 客户端
 * @param client 客户端指针
 */
void vox_redis_client_destroy(vox_redis_client_t* client);

/**
 * 连接到 Redis 服务器
 * @param client 客户端指针
 * @param host 服务器地址
 * @param port 服务器端口（默认6379）
 * @param cb 连接回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_connect(vox_redis_client_t* client,
                            const char* host,
                            uint16_t port,
                            vox_redis_connect_cb cb,
                            void* user_data);

/**
 * 断开连接
 * @param client 客户端指针
 */
void vox_redis_client_disconnect(vox_redis_client_t* client);

/**
 * 检查是否已连接
 * @param client 客户端指针
 * @return 已连接返回true，否则返回false
 */
bool vox_redis_client_is_connected(vox_redis_client_t* client);

/* ===== 命令执行 API ===== */

/**
 * 执行通用命令（可变参数）
 * @param client 客户端指针
 * @param cb 响应回调
 * @param error_cb 错误回调（可选）
 * @param user_data 用户数据
 * @param format 命令格式字符串（类似printf，但参数必须是字符串）
 * @param ... 命令参数（字符串）
 * @return 成功返回0，失败返回-1
 * 
 * 示例: vox_redis_client_command(client, cb, NULL, user_data, "GET", "mykey", NULL)
 */
int vox_redis_client_command(vox_redis_client_t* client,
                            vox_redis_response_cb cb,
                            vox_redis_error_cb error_cb,
                            void* user_data,
                            const char* format, ...);

/**
 * 发送已序列化的 RESP 命令（用于连接池等场景）
 * @param client 客户端指针
 * @param buf 命令缓冲区（完整 RESP 格式）
 * @param len 长度
 * @param cb 响应回调
 * @param error_cb 错误回调（可选）
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_command_raw(vox_redis_client_t* client,
                                 const char* buf,
                                 size_t len,
                                 vox_redis_response_cb cb,
                                 vox_redis_error_cb error_cb,
                                 void* user_data);

/**
 * 执行通用命令（va_list版本）
 * @param client 客户端指针
 * @param cb 响应回调
 * @param error_cb 错误回调（可选）
 * @param user_data 用户数据
 * @param format 命令格式字符串
 * @param args va_list参数列表
 * @return 成功返回0，失败返回-1
 * 
 * 这是 vox_redis_client_command 的 va_list 版本，用于在包装函数中转发可变参数
 */
int vox_redis_client_command_va(vox_redis_client_t* client,
                                vox_redis_response_cb cb,
                                vox_redis_error_cb error_cb,
                                void* user_data,
                                const char* format,
                                va_list args);

/**
 * 执行通用命令（数组版本，更安全）
 * @param client 客户端指针
 * @param cb 响应回调
 * @param error_cb 错误回调（可选）
 * @param user_data 用户数据
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 成功返回0，失败返回-1
 * 
 * 示例: 
 *   const char* args[] = {"GET", "mykey"};
 *   vox_redis_client_commandv(client, cb, NULL, user_data, 2, args);
 */
int vox_redis_client_commandv(vox_redis_client_t* client,
                              vox_redis_response_cb cb,
                              vox_redis_error_cb error_cb,
                              void* user_data,
                              int argc,
                              const char** argv);

/**
 * PING 命令
 * @param client 客户端指针
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_ping(vox_redis_client_t* client,
                          vox_redis_response_cb cb,
                          void* user_data);

/**
 * GET 命令
 * @param client 客户端指针
 * @param key 键名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_get(vox_redis_client_t* client,
                         const char* key,
                         vox_redis_response_cb cb,
                         void* user_data);

/**
 * SET 命令
 * @param client 客户端指针
 * @param key 键名
 * @param value 值
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_set(vox_redis_client_t* client,
                         const char* key,
                         const char* value,
                         vox_redis_response_cb cb,
                         void* user_data);

/**
 * DEL 命令
 * @param client 客户端指针
 * @param key 键名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_del(vox_redis_client_t* client,
                         const char* key,
                         vox_redis_response_cb cb,
                         void* user_data);

/**
 * EXISTS 命令
 * @param client 客户端指针
 * @param key 键名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_exists(vox_redis_client_t* client,
                            const char* key,
                            vox_redis_response_cb cb,
                            void* user_data);

/**
 * INCR 命令
 * @param client 客户端指针
 * @param key 键名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_incr(vox_redis_client_t* client,
                          const char* key,
                          vox_redis_response_cb cb,
                          void* user_data);

/**
 * DECR 命令
 * @param client 客户端指针
 * @param key 键名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_decr(vox_redis_client_t* client,
                          const char* key,
                          vox_redis_response_cb cb,
                          void* user_data);

/* ===== 哈希命令 ===== */

/**
 * HSET 命令
 * @param client 客户端指针
 * @param key 哈希键名
 * @param field 字段名
 * @param value 值
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_hset(vox_redis_client_t* client,
                         const char* key,
                         const char* field,
                         const char* value,
                         vox_redis_response_cb cb,
                         void* user_data);

/**
 * HGET 命令
 * @param client 客户端指针
 * @param key 哈希键名
 * @param field 字段名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_hget(vox_redis_client_t* client,
                         const char* key,
                         const char* field,
                         vox_redis_response_cb cb,
                         void* user_data);

/**
 * HDEL 命令
 * @param client 客户端指针
 * @param key 哈希键名
 * @param field 字段名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_hdel(vox_redis_client_t* client,
                          const char* key,
                          const char* field,
                          vox_redis_response_cb cb,
                          void* user_data);

/**
 * HEXISTS 命令
 * @param client 客户端指针
 * @param key 哈希键名
 * @param field 字段名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_hexists(vox_redis_client_t* client,
                             const char* key,
                             const char* field,
                             vox_redis_response_cb cb,
                             void* user_data);

/* ===== 列表命令 ===== */

/**
 * LPUSH 命令
 * @param client 客户端指针
 * @param key 列表键名
 * @param value 值
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_lpush(vox_redis_client_t* client,
                          const char* key,
                          const char* value,
                          vox_redis_response_cb cb,
                          void* user_data);

/**
 * RPUSH 命令
 * @param client 客户端指针
 * @param key 列表键名
 * @param value 值
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_rpush(vox_redis_client_t* client,
                          const char* key,
                          const char* value,
                          vox_redis_response_cb cb,
                          void* user_data);

/**
 * LPOP 命令
 * @param client 客户端指针
 * @param key 列表键名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_lpop(vox_redis_client_t* client,
                         const char* key,
                         vox_redis_response_cb cb,
                         void* user_data);

/**
 * RPOP 命令
 * @param client 客户端指针
 * @param key 列表键名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_rpop(vox_redis_client_t* client,
                         const char* key,
                         vox_redis_response_cb cb,
                         void* user_data);

/**
 * LLEN 命令
 * @param client 客户端指针
 * @param key 列表键名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_llen(vox_redis_client_t* client,
                          const char* key,
                          vox_redis_response_cb cb,
                          void* user_data);

/* ===== 集合命令 ===== */

/**
 * SADD 命令
 * @param client 客户端指针
 * @param key 集合键名
 * @param member 成员
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_sadd(vox_redis_client_t* client,
                          const char* key,
                          const char* member,
                          vox_redis_response_cb cb,
                          void* user_data);

/**
 * SREM 命令
 * @param client 客户端指针
 * @param key 集合键名
 * @param member 成员
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_srem(vox_redis_client_t* client,
                          const char* key,
                          const char* member,
                          vox_redis_response_cb cb,
                          void* user_data);

/**
 * SMEMBERS 命令
 * @param client 客户端指针
 * @param key 集合键名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_smembers(vox_redis_client_t* client,
                              const char* key,
                              vox_redis_response_cb cb,
                              void* user_data);

/**
 * SCARD 命令
 * @param client 客户端指针
 * @param key 集合键名
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_scard(vox_redis_client_t* client,
                           const char* key,
                           vox_redis_response_cb cb,
                           void* user_data);

/**
 * SISMEMBER 命令
 * @param client 客户端指针
 * @param key 集合键名
 * @param member 成员
 * @param cb 响应回调
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_redis_client_sismember(vox_redis_client_t* client,
                              const char* key,
                              const char* member,
                              vox_redis_response_cb cb,
                              void* user_data);

/* ===== 辅助函数 ===== */

/**
 * 释放响应数据（用于手动管理响应生命周期）
 * 注意：响应数据在回调期间有效，通常不需要手动释放
 * 只有当你在回调外部复制了响应结构时才需要调用此函数
 * 
 * @param mpool 内存池指针
 * @param response 响应指针
 */
void vox_redis_response_free(vox_mpool_t* mpool, vox_redis_response_t* response);

/**
 * 深度复制响应数据（用于在回调外部保留响应）
 * @param mpool 目标内存池
 * @param src 源响应
 * @param dst 目标响应（已分配）
 * @return 成功返回0，失败返回-1
 */
int vox_redis_response_copy(vox_mpool_t* mpool, 
                            const vox_redis_response_t* src,
                            vox_redis_response_t* dst);

#ifdef __cplusplus
}
#endif

#endif /* VOX_REDIS_CLIENT_H */
