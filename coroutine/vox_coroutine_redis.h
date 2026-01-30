/*
 * vox_coroutine_redis.h - Redis协程适配器
 * 提供async/await风格的协程API，避免回调地狱
 */

#ifndef VOX_COROUTINE_REDIS_H
#define VOX_COROUTINE_REDIS_H

#include "../redis/vox_redis_client.h"
#include "../redis/vox_redis_pool.h"
#include "vox_coroutine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 协程适配接口 ===== */

/**
 * 在协程中连接到Redis服务器
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param host 服务器地址
 * @param port 服务器端口
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_connect_await(vox_coroutine_t* co,
                                       vox_redis_client_t* client,
                                       const char* host,
                                       uint16_t port);

/**
 * 在协程中执行Redis命令（数组版本）
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param argc 参数个数（含命令名）
 * @param argv 参数数组（argv[0] 为命令名）
 * @param out_response 输出响应；成功时数据从 loop 的 mpool 分配，用完后须调用
 *                     vox_redis_response_free(vox_loop_get_mpool(loop), out_response) 释放
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_command_await(vox_coroutine_t* co,
                                       vox_redis_client_t* client,
                                       int argc,
                                       const char** argv,
                                       vox_redis_response_t* out_response);

/* ===== 常用命令便捷函数 ===== */

/**
 * 在协程中执行PING命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param out_response 输出响应（用完后须 vox_redis_response_free(mpool, out_response)）
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_ping_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    vox_redis_response_t* out_response);

/**
 * 在协程中执行GET命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 键名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_get_await(vox_coroutine_t* co,
                                   vox_redis_client_t* client,
                                   const char* key,
                                   vox_redis_response_t* out_response);

/**
 * 在协程中执行SET命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 键名
 * @param value 值
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_set_await(vox_coroutine_t* co,
                                   vox_redis_client_t* client,
                                   const char* key,
                                   const char* value,
                                   vox_redis_response_t* out_response);

/**
 * 在协程中执行DEL命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 键名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_del_await(vox_coroutine_t* co,
                                   vox_redis_client_t* client,
                                   const char* key,
                                   vox_redis_response_t* out_response);

/**
 * 在协程中执行EXISTS命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 键名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_exists_await(vox_coroutine_t* co,
                                      vox_redis_client_t* client,
                                      const char* key,
                                      vox_redis_response_t* out_response);

/**
 * 在协程中执行INCR命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 键名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_incr_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    vox_redis_response_t* out_response);

/**
 * 在协程中执行DECR命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 键名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_decr_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    vox_redis_response_t* out_response);

/* ===== 哈希命令 ===== */

/**
 * 在协程中执行HSET命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 哈希键名
 * @param field 字段名
 * @param value 值
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_hset_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    const char* field,
                                    const char* value,
                                    vox_redis_response_t* out_response);

/**
 * 在协程中执行HGET命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 哈希键名
 * @param field 字段名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_hget_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    const char* field,
                                    vox_redis_response_t* out_response);

/**
 * 在协程中执行HDEL命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 哈希键名
 * @param field 字段名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_hdel_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    const char* field,
                                    vox_redis_response_t* out_response);

/**
 * 在协程中执行HEXISTS命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 哈希键名
 * @param field 字段名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_hexists_await(vox_coroutine_t* co,
                                       vox_redis_client_t* client,
                                       const char* key,
                                       const char* field,
                                       vox_redis_response_t* out_response);

/* ===== 列表命令 ===== */

/**
 * 在协程中执行LPUSH命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 列表键名
 * @param value 值
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_lpush_await(vox_coroutine_t* co,
                                     vox_redis_client_t* client,
                                     const char* key,
                                     const char* value,
                                     vox_redis_response_t* out_response);

/**
 * 在协程中执行RPUSH命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 列表键名
 * @param value 值
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_rpush_await(vox_coroutine_t* co,
                                     vox_redis_client_t* client,
                                     const char* key,
                                     const char* value,
                                     vox_redis_response_t* out_response);

/**
 * 在协程中执行LPOP命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 列表键名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_lpop_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    vox_redis_response_t* out_response);

/**
 * 在协程中执行RPOP命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 列表键名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_rpop_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    vox_redis_response_t* out_response);

/**
 * 在协程中执行LLEN命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 列表键名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_llen_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    vox_redis_response_t* out_response);

/* ===== 集合命令 ===== */

/**
 * 在协程中执行SADD命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 集合键名
 * @param member 成员
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_sadd_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    const char* member,
                                    vox_redis_response_t* out_response);

/**
 * 在协程中执行SREM命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 集合键名
 * @param member 成员
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_srem_await(vox_coroutine_t* co,
                                    vox_redis_client_t* client,
                                    const char* key,
                                    const char* member,
                                    vox_redis_response_t* out_response);

/**
 * 在协程中执行SMEMBERS命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 集合键名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_smembers_await(vox_coroutine_t* co,
                                        vox_redis_client_t* client,
                                        const char* key,
                                        vox_redis_response_t* out_response);

/**
 * 在协程中执行SCARD命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 集合键名
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_scard_await(vox_coroutine_t* co,
                                     vox_redis_client_t* client,
                                     const char* key,
                                     vox_redis_response_t* out_response);

/**
 * 在协程中执行SISMEMBER命令
 * @param co 协程指针
 * @param client Redis客户端指针
 * @param key 集合键名
 * @param member 成员
 * @param out_response 输出响应数据
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_sismember_await(vox_coroutine_t* co,
                                         vox_redis_client_t* client,
                                         const char* key,
                                         const char* member,
                                         vox_redis_response_t* out_response);

/* ===== 连接池协程接口 ===== */

/**
 * 在协程中从连接池获取一个连接
 * @param co 协程指针
 * @param pool 连接池指针
 * @param out_client 输出获取到的客户端；成功时非 NULL，用完后须调用 vox_redis_pool_release(pool, client)
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_pool_acquire_await(vox_coroutine_t* co,
                                           vox_redis_pool_t* pool,
                                           vox_redis_client_t** out_client);

/**
 * 在协程中通过连接池执行 Redis 命令（内部取连接、发命令、归还）
 * @param co 协程指针
 * @param pool 连接池指针
 * @param argc 参数个数（含命令名）
 * @param argv 参数数组（argv[0] 为命令名）
 * @param out_response 输出响应；成功时数据从 loop 的 mpool 分配，用完后须 vox_redis_response_free(mpool, out_response)
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_redis_pool_command_await(vox_coroutine_t* co,
                                           vox_redis_pool_t* pool,
                                           int argc,
                                           const char** argv,
                                           vox_redis_response_t* out_response);

/**
 * 在协程中通过连接池执行 PING
 */
int vox_coroutine_redis_pool_ping_await(vox_coroutine_t* co,
                                         vox_redis_pool_t* pool,
                                         vox_redis_response_t* out_response);

/**
 * 在协程中通过连接池执行 GET
 */
int vox_coroutine_redis_pool_get_await(vox_coroutine_t* co,
                                        vox_redis_pool_t* pool,
                                        const char* key,
                                        vox_redis_response_t* out_response);

/**
 * 在协程中通过连接池执行 SET
 */
int vox_coroutine_redis_pool_set_await(vox_coroutine_t* co,
                                        vox_redis_pool_t* pool,
                                        const char* key,
                                        const char* value,
                                        vox_redis_response_t* out_response);

/**
 * 在协程中通过连接池执行 DEL
 */
int vox_coroutine_redis_pool_del_await(vox_coroutine_t* co,
                                        vox_redis_pool_t* pool,
                                        const char* key,
                                        vox_redis_response_t* out_response);

#ifdef __cplusplus
}
#endif

#endif /* VOX_COROUTINE_REDIS_H */
