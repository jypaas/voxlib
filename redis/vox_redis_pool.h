/*
 * vox_redis_pool.h - Redis 连接池（纯连接管理）
 *
 * 只做连接管理：初始连接数 + 最大连接数；超过初始部分的连接为临时连接，
 * 用完后归还时自动关闭并移除。调用方取连接后自行发命令，用完后归还。
 */

#ifndef VOX_REDIS_POOL_H
#define VOX_REDIS_POOL_H

#include "../vox_os.h"
#include "../vox_loop.h"
#include "vox_redis_client.h"

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vox_redis_pool vox_redis_pool_t;

/**
 * 连接池创建完成回调（所有初始连接尝试完成后调用）
 * @param pool 连接池指针（可能在 create 返回前即调用，故由实现传入）
 * @param status 0 表示至少有一个初始连接成功，非 0 表示全部失败
 * @param user_data 创建时传入的 user_data
 */
typedef void (*vox_redis_pool_connect_cb)(vox_redis_pool_t* pool, int status, void* user_data);

/**
 * 异步取到连接（或失败）的回调
 * @param pool 连接池
 * @param client 成功时为取到的客户端，失败时为 NULL
 * @param status 0 表示成功，非 0 表示失败
 * @param user_data 调用 acquire 时传入的 user_data
 */
typedef void (*vox_redis_pool_acquire_cb)(vox_redis_pool_t* pool,
                                          vox_redis_client_t* client,
                                          int status,
                                          void* user_data);

/**
 * 创建连接池
 * @param loop 事件循环
 * @param host 主机
 * @param port 端口
 * @param initial_size 初始连接数（常驻池中）
 * @param max_size 最大连接数（含初始；可再创建的临时连接数 = max_size - initial_size）
 * @param connect_cb 初始连接全部完成后的回调，可为 NULL
 * @param user_data 传给 connect_cb
 * @return 成功返回池指针，失败返回 NULL。要求 initial_size <= max_size 且 initial_size > 0。
 */
vox_redis_pool_t* vox_redis_pool_create(vox_loop_t* loop,
                                        const char* host,
                                        uint16_t port,
                                        size_t initial_size,
                                        size_t max_size,
                                        vox_redis_pool_connect_cb connect_cb,
                                        void* user_data);

/**
 * 销毁连接池（会关闭所有连接，不再执行未完成的 acquire 回调；若需通知可先自行记录等待者）
 */
void vox_redis_pool_destroy(vox_redis_pool_t* pool);

/**
 * 异步获取一个空闲连接（或在不超 max 时新建临时连接，或排队等待）
 * 回调在 loop 线程执行；成功时 client 非 NULL、status==0，用完后必须调用 vox_redis_pool_release。
 * @param pool 连接池
 * @param cb 取到连接或失败时回调
 * @param user_data 传给 cb
 * @return 0 表示已排队/已安排，非 0 表示参数错误等
 */
int vox_redis_pool_acquire_async(vox_redis_pool_t* pool,
                                  vox_redis_pool_acquire_cb cb,
                                  void* user_data);

/**
 * 归还连接。若该连接是临时连接，则关闭并从池中移除；否则标记为空闲供后续 acquire 使用。
 * @param pool 连接池
 * @param client 之前通过 acquire 回调得到的 client
 */
void vox_redis_pool_release(vox_redis_pool_t* pool, vox_redis_client_t* client);

/**
 * 初始连接数（创建时传入的 initial_size）
 */
size_t vox_redis_pool_initial_size(vox_redis_pool_t* pool);

/**
 * 最大连接数（创建时传入的 max_size）
 */
size_t vox_redis_pool_max_size(vox_redis_pool_t* pool);

/**
 * 当前总连接数（初始连接中已建立的 + 临时连接数）
 */
size_t vox_redis_pool_current_size(vox_redis_pool_t* pool);

/**
 * 当前空闲连接数（仅统计常驻连接中的空闲数，不含正在创建的临时连接）
 */
size_t vox_redis_pool_available(vox_redis_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif /* VOX_REDIS_POOL_H */
