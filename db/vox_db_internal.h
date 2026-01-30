/*
 * vox_db_internal.h - DB 模块内部声明
 */

#ifndef VOX_DB_INTERNAL_H
#define VOX_DB_INTERNAL_H

#include "vox_db.h"

#include "../vox_mutex.h"
#include "../vox_tpool.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vox_db_driver_vtbl {
    const char* name;

    /* 1 = 异步 exec/query 在 loop 线程执行，避免跨线程使用同一连接导致崩溃。
     * SQLite/DuckDB/MySQL 建议同线程使用连接；libpq (PGSQL) 允许不同线程串行使用同一连接，可设为 0。 */
    int use_loop_thread_for_async;

    /* 生命周期 */
    int (*connect)(vox_db_conn_t* conn, const char* conninfo);
    void (*disconnect)(vox_db_conn_t* conn);

    /* 连接健康检查（返回0表示连接正常，-1表示连接断开） */
    int (*ping)(vox_db_conn_t* conn);

    /* 执行/查询（在工作线程中调用） */
    int (*exec)(vox_db_conn_t* conn,
                const char* sql,
                const vox_db_value_t* params,
                size_t nparams,
                int64_t* out_affected_rows);

    int (*query)(vox_db_conn_t* conn,
                 const char* sql,
                 const vox_db_value_t* params,
                 size_t nparams,
                 vox_db_row_cb row_cb,
                 void* row_user_data,
                 int64_t* out_row_count);

    /* 事务处理（在工作线程中调用） */
    int (*begin_transaction)(vox_db_conn_t* conn);
    int (*commit)(vox_db_conn_t* conn);
    int (*rollback)(vox_db_conn_t* conn);

    const char* (*last_error)(vox_db_conn_t* conn);
} vox_db_driver_vtbl_t;

struct vox_db_conn {
    vox_loop_t* loop;
    vox_mpool_t* mpool;

    vox_db_driver_t driver;
    const vox_db_driver_vtbl_t* vtbl;

    /* native 连接句柄（各驱动自定义类型） */
    void* native;

    /* 连接信息（用于重连） */
    char* conninfo;

    /* 防止同一连接并发执行 */
    vox_mutex_t mu;
    bool busy;

    /* 回调触发线程模式 */
    vox_db_callback_mode_t cb_mode;
};

/* 每个驱动模块提供自己的 vtbl getter（当对应宏启用且被编译时可用） */
#ifdef VOX_USE_SQLITE3
const vox_db_driver_vtbl_t* vox_db_sqlite3_vtbl(void);
#endif

#ifdef VOX_USE_DUCKDB
const vox_db_driver_vtbl_t* vox_db_duckdb_vtbl(void);
#endif

#ifdef VOX_USE_PGSQL
const vox_db_driver_vtbl_t* vox_db_pgsql_vtbl(void);
#endif

#ifdef VOX_USE_MYSQL
const vox_db_driver_vtbl_t* vox_db_mysql_vtbl(void);
#endif

/* 内部：连接并发控制 */
int vox_db_conn_try_begin(vox_db_conn_t* conn);
void vox_db_conn_end(vox_db_conn_t* conn);

/* 内部：连接健康检查（检测并自动重连） */
int vox_db_conn_ping_and_reconnect(vox_db_conn_t* conn);

#ifdef __cplusplus
}
#endif

#endif /* VOX_DB_INTERNAL_H */
