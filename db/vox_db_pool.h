/*
 * vox_db_pool.h - 数据库连接池（纯连接管理）
 *
 * 设计思路与 Redis 连接池一致：
 * - 只做连接管理：初始连接数 + 最大连接数；超过初始部分的连接为临时连接，
 *   用完后归还时自动关闭并移除。调用方取连接后自行执行 SQL，用完后归还。
 * - 提供 acquire_async / release；可选便捷接口（exec/query 内部借还连接）。
 */

#ifndef VOX_DB_POOL_H
#define VOX_DB_POOL_H

#include "../vox_os.h"
#include "../vox_loop.h"
#include "vox_db.h"

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vox_db_pool vox_db_pool_t;

/**
 * 连接池创建完成回调（所有初始连接创建完成后调用）
 * @param pool 连接池
 * @param status 0 表示至少有一个初始连接成功，非 0 表示全部失败
 * @param user_data 创建时传入的 user_data
 */
typedef void (*vox_db_pool_connect_cb)(vox_db_pool_t* pool, int status, void* user_data);

/**
 * 异步取到连接（或失败）的回调
 * @param pool 连接池
 * @param conn 成功时为取到的连接，失败时为 NULL
 * @param status 0 表示成功，非 0 表示失败
 * @param user_data 调用 acquire 时传入的 user_data
 */
typedef void (*vox_db_pool_acquire_cb)(vox_db_pool_t* pool,
                                       vox_db_conn_t* conn,
                                       int status,
                                       void* user_data);

/* ===== 连接池生命周期 ===== */

/**
 * 创建连接池
 * @param loop 事件循环
 * @param driver 驱动类型
 * @param conninfo 连接串
 * @param initial_size 初始连接数（常驻池中）
 * @param max_size 最大连接数（含初始；可再创建的临时连接数 = max_size - initial_size）
 * @param connect_cb 初始连接全部完成后的回调，可为 NULL
 * @param user_data 传给 connect_cb
 * @return 成功返回池指针，失败返回 NULL。要求 initial_size <= max_size 且 initial_size > 0。
 */
vox_db_pool_t* vox_db_pool_create(vox_loop_t* loop,
                                  vox_db_driver_t driver,
                                  const char* conninfo,
                                  size_t initial_size,
                                  size_t max_size,
                                  vox_db_pool_connect_cb connect_cb,
                                  void* user_data);

/**
 * 创建连接池（兼容旧 API：无 connect_cb）
 * @deprecated 使用 vox_db_pool_create 替代
 */
static inline vox_db_pool_t* vox_db_pool_create_ex(vox_loop_t* loop,
                                                    vox_db_driver_t driver,
                                                    const char* conninfo,
                                                    size_t initial_size,
                                                    size_t max_size) {
    return vox_db_pool_create(loop, driver, conninfo, initial_size, max_size, NULL, NULL);
}

/**
 * 销毁连接池（会关闭所有连接，不再执行未完成的 acquire 回调）
 */
void vox_db_pool_destroy(vox_db_pool_t* pool);

/* ===== 连接获取与归还 ===== */

/**
 * 异步获取一个空闲连接（或在不超 max 时新建临时连接，或排队等待）
 * 回调在 loop 线程执行；成功时 conn 非 NULL、status==0，用完后必须调用 vox_db_pool_release。
 * @param pool 连接池
 * @param cb 取到连接或失败时回调
 * @param user_data 传给 cb
 * @return 0 表示已排队/已安排，非 0 表示参数错误等
 */
int vox_db_pool_acquire_async(vox_db_pool_t* pool,
                              vox_db_pool_acquire_cb cb,
                              void* user_data);

/**
 * 同步获取一个空闲连接（无空闲且已达 max 时返回 NULL）
 * 用完后必须调用 vox_db_pool_release。
 * @param pool 连接池
 * @return 成功返回连接，失败或池满无空闲时返回 NULL
 */
vox_db_conn_t* vox_db_pool_acquire_sync(vox_db_pool_t* pool);

/**
 * 归还连接。若该连接是临时连接，则关闭并从池中移除；否则标记为空闲供后续 acquire 使用。
 * @param pool 连接池
 * @param conn 之前通过 acquire 回调得到的连接
 */
void vox_db_pool_release(vox_db_pool_t* pool, vox_db_conn_t* conn);

/* ===== 回调模式（对池内所有连接生效） ===== */

int vox_db_pool_set_callback_mode(vox_db_pool_t* pool, vox_db_callback_mode_t mode);
vox_db_callback_mode_t vox_db_pool_get_callback_mode(vox_db_pool_t* pool);

/* ===== 池状态查询 ===== */

/** 初始连接数（创建时传入的 initial_size） */
size_t vox_db_pool_initial_size(vox_db_pool_t* pool);

/** 最大连接数（创建时传入的 max_size） */
size_t vox_db_pool_max_size(vox_db_pool_t* pool);

/** 当前总连接数（初始连接中已建立的 + 临时连接数） */
size_t vox_db_pool_current_size(vox_db_pool_t* pool);

/** 当前空闲连接数（仅统计常驻连接中的空闲数，不含正在创建的临时连接） */
size_t vox_db_pool_available(vox_db_pool_t* pool);

/* ===== 便捷接口（内部借还连接） ===== */

int vox_db_pool_exec_async(vox_db_pool_t* pool,
                           const char* sql,
                           const vox_db_value_t* params,
                           size_t nparams,
                           vox_db_exec_cb cb,
                           void* user_data);

int vox_db_pool_query_async(vox_db_pool_t* pool,
                            const char* sql,
                            const vox_db_value_t* params,
                            size_t nparams,
                            vox_db_row_cb row_cb,
                            vox_db_done_cb done_cb,
                            void* user_data);

int vox_db_pool_exec(vox_db_pool_t* pool,
                     const char* sql,
                     const vox_db_value_t* params,
                     size_t nparams,
                     int64_t* out_affected_rows);

int vox_db_pool_query(vox_db_pool_t* pool,
                      const char* sql,
                      const vox_db_value_t* params,
                      size_t nparams,
                      vox_db_row_cb row_cb,
                      void* row_user_data,
                      int64_t* out_row_count);

#ifdef __cplusplus
}
#endif

#endif /* VOX_DB_POOL_H */
