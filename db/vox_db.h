/*
 * vox_db.h - 高性能数据库抽象层（MySQL / PostgreSQL / SQLite3 / DuckDB）
 *
 * 设计目标：
 * - 统一 API：connect/exec/query
 * - 高性能：默认 row streaming（逐行回调，避免一次性物化大结果集）
 * - 异步：复用 vox_loop 内部线程池；回调在工作线程触发（与 vox_fs 一致）
 *
 * 注意：
 * - `row_cb` 中收到的数据视图只在该回调期间有效；若需持久化请自行复制
 * - 同一个 `vox_db_conn_t` 默认不支持并发执行（避免 native handle 的并发未定义行为）
 */

#ifndef VOX_DB_H
#define VOX_DB_H

#include "../vox_os.h"
#include "../vox_loop.h"
#include "../vox_mpool.h"
#include "../vox_string.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 基础类型 ===== */

typedef enum {
    VOX_DB_DRIVER_MYSQL = 0,
    VOX_DB_DRIVER_PGSQL,
    VOX_DB_DRIVER_SQLITE3,
    VOX_DB_DRIVER_DUCKDB
} vox_db_driver_t;

typedef enum {
    VOX_DB_TYPE_NULL = 0,
    VOX_DB_TYPE_I64,
    VOX_DB_TYPE_U64,
    VOX_DB_TYPE_F64,
    VOX_DB_TYPE_BOOL,
    VOX_DB_TYPE_TEXT, /* UTF-8 文本 */
    VOX_DB_TYPE_BLOB  /* 二进制 */
} vox_db_type_t;

typedef struct {
    const void* data;
    size_t len;
} vox_db_blob_t;

typedef struct vox_db_value {
    vox_db_type_t type;
    union {
        int64_t i64;
        uint64_t u64;
        double f64;
        bool boolean;
        vox_strview_t text; /* 指向外部内存 */
        vox_db_blob_t blob; /* 指向外部内存 */
    } u;
} vox_db_value_t;

typedef struct {
    size_t column_count;
    const char* const* column_names; /* 指针数组；name 指针在回调期间有效 */
    const vox_db_value_t* values;    /* 值数组；值引用的内存仅在回调期间有效 */
} vox_db_row_t;

typedef struct vox_db_conn vox_db_conn_t;

/* ===== 回调线程模式 ===== */

typedef enum {
    /* 默认：回调在工作线程触发（与 vox_fs 一致，最快） */
    VOX_DB_CALLBACK_WORKER = 0,
    /* 可选：回调通过 vox_loop_queue_work 切回 loop 线程触发（更适合 HTTP） */
    VOX_DB_CALLBACK_LOOP = 1
} vox_db_callback_mode_t;

/* 与 use_loop_thread_for_async（驱动 vtbl）的关系：
 * 当驱动在 loop 线程执行异步任务时，两种模式最终都在 loop 线程回调，
 * 内部会避免冗余 queue；不冲突。 */

/**
 * 设置连接的回调线程模式
 * @note 仅影响后续提交的异步操作
 */
int vox_db_set_callback_mode(vox_db_conn_t* conn, vox_db_callback_mode_t mode);

/**
 * 获取连接的回调线程模式
 */
vox_db_callback_mode_t vox_db_get_callback_mode(vox_db_conn_t* conn);

/* ===== 回调类型 ===== */

typedef void (*vox_db_exec_cb)(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data);
typedef void (*vox_db_row_cb)(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data);
typedef void (*vox_db_done_cb)(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data);

/* ===== 连接 ===== */

/**
 * 建立数据库连接
 * @param loop 事件循环（用于内存池与线程池）
 * @param driver 驱动类型
 * @param conninfo 连接串（各驱动自定义格式；sqlite3/duckdb 可用文件路径）
 * @return 成功返回连接对象，失败返回 NULL
 */
vox_db_conn_t* vox_db_connect(vox_loop_t* loop, vox_db_driver_t driver, const char* conninfo);

/**
 * 关闭并释放连接对象
 */
void vox_db_disconnect(vox_db_conn_t* conn);

/**
 * 获取连接所属的 loop
 */
vox_loop_t* vox_db_get_loop(vox_db_conn_t* conn);

/**
 * 获取连接使用的驱动类型（便于示例或上层按驱动选择 SQL 方言）
 */
vox_db_driver_t vox_db_get_driver(vox_db_conn_t* conn);

/**
 * 获取连接的最后错误信息（如果驱动支持）
 */
const char* vox_db_last_error(vox_db_conn_t* conn);

/* ===== 同步执行（会阻塞当前线程） ===== */

int vox_db_exec(vox_db_conn_t* conn,
                const char* sql,
                const vox_db_value_t* params,
                size_t nparams,
                int64_t* out_affected_rows);

/**
 * 同步查询（逐行回调，回调在当前线程触发）
 * @return 成功返回0，失败返回-1
 */
int vox_db_query(vox_db_conn_t* conn,
                 const char* sql,
                 const vox_db_value_t* params,
                 size_t nparams,
                 vox_db_row_cb row_cb,
                 void* row_user_data,
                 int64_t* out_row_count);

/* ===== 异步执行（不返回结果集） ===== */

int vox_db_exec_async(vox_db_conn_t* conn,
                      const char* sql,
                      const vox_db_value_t* params,
                      size_t nparams,
                      vox_db_exec_cb cb,
                      void* user_data);

/* ===== 异步查询（逐行回调） ===== */

int vox_db_query_async(vox_db_conn_t* conn,
                       const char* sql,
                       const vox_db_value_t* params,
                       size_t nparams,
                       vox_db_row_cb row_cb,
                       vox_db_done_cb done_cb,
                       void* user_data);

/* ===== 事务处理 ===== */

/**
 * 同步开始事务
 * @return 成功返回0，失败返回-1
 */
int vox_db_begin_transaction(vox_db_conn_t* conn);

/**
 * 同步提交事务
 * @return 成功返回0，失败返回-1
 */
int vox_db_commit(vox_db_conn_t* conn);

/**
 * 同步回滚事务
 * @return 成功返回0，失败返回-1
 */
int vox_db_rollback(vox_db_conn_t* conn);

/**
 * 异步开始事务
 * @param cb 完成回调（status: 0=成功, -1=失败）
 */
int vox_db_begin_transaction_async(vox_db_conn_t* conn, vox_db_exec_cb cb, void* user_data);

/**
 * 异步提交事务
 * @param cb 完成回调（status: 0=成功, -1=失败）
 */
int vox_db_commit_async(vox_db_conn_t* conn, vox_db_exec_cb cb, void* user_data);

/**
 * 异步回滚事务
 * @param cb 完成回调（status: 0=成功, -1=失败）
 */
int vox_db_rollback_async(vox_db_conn_t* conn, vox_db_exec_cb cb, void* user_data);

/* 连接池 API 见 vox_db_pool.h */

#ifdef __cplusplus
}
#endif

#endif /* VOX_DB_H */
