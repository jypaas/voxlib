/*
 * vox_db_coroutine.h - 数据库协程适配器
 * 提供async/await风格的协程API，避免回调地狱
 */

#ifndef VOX_DB_COROUTINE_H
#define VOX_DB_COROUTINE_H

#include "../db/vox_db.h"
#include "../db/vox_db_pool.h"
#include "../db/vox_orm.h"
#include "vox_coroutine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 协程适配接口 ===== */

/**
 * 在协程中执行SQL（不返回结果集）
 * @param co 协程指针
 * @param conn 数据库连接
 * @param sql SQL语句
 * @param params 参数数组（可为NULL）
 * @param nparams 参数数量
 * @param out_affected_rows 输出受影响的行数（可为NULL）
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_db_exec_await(vox_coroutine_t* co,
                                 vox_db_conn_t* conn,
                                 const char* sql,
                                 const vox_db_value_t* params,
                                 size_t nparams,
                                 int64_t* out_affected_rows);

/**
 * 在协程中查询SQL（返回结果集）
 * @param co 协程指针
 * @param conn 数据库连接
 * @param sql SQL语句
 * @param params 参数数组（可为NULL）
 * @param nparams 参数数量
 * @param out_rows 输出行数组指针（可为NULL，需要调用者释放）
 * @param out_row_count 输出行数（可为NULL）
 * @return 成功返回0，失败返回-1
 * @note out_rows指向的行数据在协程结束后仍然有效（深拷贝），需要调用者释放
 */
int vox_coroutine_db_query_await(vox_coroutine_t* co,
                                  vox_db_conn_t* conn,
                                  const char* sql,
                                  const vox_db_value_t* params,
                                  size_t nparams,
                                  vox_db_row_t** out_rows,
                                  int64_t* out_row_count);

/**
 * 在协程中开始事务
 * @param co 协程指针
 * @param conn 数据库连接
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_db_begin_transaction_await(vox_coroutine_t* co,
                                             vox_db_conn_t* conn);

/**
 * 在协程中提交事务
 * @param co 协程指针
 * @param conn 数据库连接
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_db_commit_await(vox_coroutine_t* co,
                                   vox_db_conn_t* conn);

/**
 * 在协程中回滚事务
 * @param co 协程指针
 * @param conn 数据库连接
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_db_rollback_await(vox_coroutine_t* co,
                                    vox_db_conn_t* conn);

/* ===== 连接池协程适配接口（参考 vox_coroutine_redis：仅用 acquire/release，不用池的便捷接口） ===== */

/**
 * 在协程中从连接池获取一个连接
 * @param co 协程指针
 * @param db_pool 数据库连接池
 * @param out_conn 输出获取到的连接；成功时非 NULL，用完后须调用 vox_db_pool_release(db_pool, conn)
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_db_pool_acquire_await(vox_coroutine_t* co,
                                         vox_db_pool_t* db_pool,
                                         vox_db_conn_t** out_conn);

/**
 * 在协程中通过连接池执行SQL（内部取连接、执行、归还，不返回结果集）
 * @param co 协程指针
 * @param db_pool 数据库连接池
 * @param sql SQL语句
 * @param params 参数数组（可为NULL）
 * @param nparams 参数数量
 * @param out_affected_rows 输出受影响的行数（可为NULL）
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_db_pool_exec_await(vox_coroutine_t* co,
                                      vox_db_pool_t* db_pool,
                                      const char* sql,
                                      const vox_db_value_t* params,
                                      size_t nparams,
                                      int64_t* out_affected_rows);

/**
 * 在协程中通过连接池查询SQL（内部取连接、查询、归还，返回结果集）
 * @param co 协程指针
 * @param db_pool 数据库连接池
 * @param sql SQL语句
 * @param params 参数数组（可为NULL）
 * @param nparams 参数数量
 * @param out_rows 输出行数组指针（可为NULL，需要调用者释放）
 * @param out_row_count 输出行数（可为NULL）
 * @return 成功返回0，失败返回-1
 * @note out_rows指向的行数据在协程结束后仍然有效（深拷贝），需要调用者释放
 */
int vox_coroutine_db_pool_query_await(vox_coroutine_t* co,
                                       vox_db_pool_t* db_pool,
                                       const char* sql,
                                       const vox_db_value_t* params,
                                       size_t nparams,
                                       vox_db_row_t** out_rows,
                                       int64_t* out_row_count);

/* ===== ORM 协程适配接口 ===== */

/**
 * 协程中建表
 */
int vox_coroutine_orm_create_table_await(vox_coroutine_t* co,
                                         vox_db_conn_t* conn,
                                         const char* table,
                                         const vox_orm_field_t* fields,
                                         size_t nfields);

/**
 * 协程中删表
 */
int vox_coroutine_orm_drop_table_await(vox_coroutine_t* co,
                                       vox_db_conn_t* conn,
                                       const char* table);

/**
 * 协程中插入一行
 * @param out_affected 可为 NULL
 */
int vox_coroutine_orm_insert_await(vox_coroutine_t* co,
                                  vox_db_conn_t* conn,
                                  const char* table,
                                  const vox_orm_field_t* fields,
                                  size_t nfields,
                                  const void* row_struct,
                                  int64_t* out_affected);

/**
 * 协程中更新
 * @param out_affected 可为 NULL
 */
int vox_coroutine_orm_update_await(vox_coroutine_t* co,
                                   vox_db_conn_t* conn,
                                   const char* table,
                                   const vox_orm_field_t* fields,
                                   size_t nfields,
                                   const void* row_struct,
                                   const char* where_clause,
                                   const vox_db_value_t* where_params,
                                   size_t n_where_params,
                                   int64_t* out_affected);

/**
 * 协程中删除
 * @param out_affected 可为 NULL
 */
int vox_coroutine_orm_delete_await(vox_coroutine_t* co,
                                   vox_db_conn_t* conn,
                                   const char* table,
                                   const char* where_clause,
                                   const vox_db_value_t* where_params,
                                   size_t n_where_params,
                                   int64_t* out_affected);

/**
 * 协程中查单行：结果写入 row_struct（调用方分配，至少 row_size 字节）
 * @param out_found 可为 NULL；成功且查到一行时写 1，否则写 0
 */
int vox_coroutine_orm_select_one_await(vox_coroutine_t* co,
                                       vox_db_conn_t* conn,
                                       const char* table,
                                       const vox_orm_field_t* fields,
                                       size_t nfields,
                                       void* row_struct,
                                       size_t row_size,
                                       const char* where_clause,
                                       const vox_db_value_t* where_params,
                                       size_t n_where_params,
                                       int* out_found);

/**
 * 协程中查多行：结果 push 到 out_list（调用方创建 vox_vector），out_row_count 写行数
 */
int vox_coroutine_orm_select_await(vox_coroutine_t* co,
                                   vox_db_conn_t* conn,
                                   const char* table,
                                   const vox_orm_field_t* fields,
                                   size_t nfields,
                                   size_t row_size,
                                   vox_vector_t* out_list,
                                   int64_t* out_row_count,
                                   const char* where_clause,
                                   const vox_db_value_t* where_params,
                                   size_t n_where_params);

#ifdef __cplusplus
}
#endif

#endif /* VOX_DB_COROUTINE_H */
