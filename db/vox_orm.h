/*
 * vox_orm.h - ORM 层（基于 vox_db）
 *
 * 设计目标：
 * - 按实体描述符生成 SQL 与参数，屏蔽各数据库占位符与方言差异
 * - 提供 Insert / Update / Delete / Select 单行与多行，同步与异步
 * - 行数据与结构体双向映射（row↔struct），TEXT/BLOB 按 buffer_size 拷贝
 *
 * 使用方式：定义结构体 + vox_orm_field_t 描述符，调用 vox_orm_* API。
 * 复杂查询仍请使用 vox_db_exec / vox_db_query。
 *
 * 自增 id：字段设 is_primary_key=1、auto_gen=1，建表时生成 AUTOINCREMENT/SERIAL/AUTO_INCREMENT，
 * INSERT 时跳过该列由数据库生成；查回后可从 last_insert_id 或 SELECT 取回。
 * 索引：仅在建表时根据描述符创建；字段设 indexed=1 或 unique_index=1 时，建表后自动建单列索引（名 idx_表名_列名）。
 */

#ifndef VOX_ORM_H
#define VOX_ORM_H

#include "vox_db.h"
#include "../vox_vector.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 实体描述符 ===== */

typedef struct vox_orm_field {
    const char* name;         /* 列名 */
    vox_db_type_t type;       /* 列类型（与 vox_db_value_t 一致） */
    size_t offset;            /* 在结构体中的偏移 offsetof(struct, field) */
    unsigned is_primary_key : 1;
    unsigned auto_gen : 1;    /* 1 = INSERT 时跳过，由数据库生成（如自增主键） */
    unsigned indexed : 1;     /* 1 = 建表时自动建单列普通索引（索引名 idx_表名_列名） */
    unsigned unique_index : 1;/* 1 = 建表时自动建单列唯一索引；与 indexed 可只设其一 */
    size_t buffer_size;      /* 仅 row→struct：TEXT/BLOB 目标缓冲区最大字节数，0 表示 256 */
} vox_orm_field_t;

/* ===== 回调类型 ===== */

typedef void (*vox_orm_exec_cb)(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data);
typedef void (*vox_orm_select_one_cb)(vox_db_conn_t* conn, int status, void* row_struct, void* user_data);
typedef void (*vox_orm_select_done_cb)(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data);

/* ===== DDL：建表 / 删表 ===== */

/**
 * 同步建表：按描述符生成 CREATE TABLE，并按当前驱动生成列类型与主键/自增
 * 列类型映射：I64/U64→BIGINT/INTEGER，F64→DOUBLE/REAL，BOOL→BOOLEAN/TINYINT(1)，TEXT→TEXT/VARCHAR(n)，BLOB→BLOB/BYTEA
 * @return 0 成功，-1 失败
 */
int vox_orm_create_table(vox_db_conn_t* conn,
                         const char* table,
                         const vox_orm_field_t* fields,
                         size_t nfields);

/**
 * 异步建表
 */
int vox_orm_create_table_async(vox_db_conn_t* conn,
                                const char* table,
                                const vox_orm_field_t* fields,
                                size_t nfields,
                                vox_orm_exec_cb cb,
                                void* user_data);

/**
 * 同步删表：DROP TABLE IF EXISTS table
 */
int vox_orm_drop_table(vox_db_conn_t* conn, const char* table);

/**
 * 异步删表
 */
int vox_orm_drop_table_async(vox_db_conn_t* conn,
                              const char* table,
                              vox_orm_exec_cb cb,
                              void* user_data);

/* ===== Insert ===== */

/**
 * 同步插入：按描述符生成 INSERT，绑定 struct，执行
 * @param out_affected 可 NULL；成功时写入受影响行数（通常为 1）
 * @return 0 成功，-1 失败
 */
int vox_orm_insert(vox_db_conn_t* conn,
                  const char* table,
                  const vox_orm_field_t* fields,
                  size_t nfields,
                  const void* row_struct,
                  int64_t* out_affected);

/**
 * 异步插入
 */
int vox_orm_insert_async(vox_db_conn_t* conn,
                         const char* table,
                         const vox_orm_field_t* fields,
                         size_t nfields,
                         const void* row_struct,
                         vox_orm_exec_cb cb,
                         void* user_data);

/* ===== Update ===== */

/**
 * 同步更新：按描述符生成 UPDATE table SET ... WHERE where_clause
 * @param where_clause 如 "id = ?"，不可为 NULL（至少 "1=1"）
 * @param where_params 与 where_clause 占位符对应的参数
 */
int vox_orm_update(vox_db_conn_t* conn,
                  const char* table,
                  const vox_orm_field_t* fields,
                  size_t nfields,
                  const void* row_struct,
                  const char* where_clause,
                  const vox_db_value_t* where_params,
                  size_t n_where_params,
                  int64_t* out_affected);

int vox_orm_update_async(vox_db_conn_t* conn,
                         const char* table,
                         const vox_orm_field_t* fields,
                         size_t nfields,
                         const void* row_struct,
                         const char* where_clause,
                         const vox_db_value_t* where_params,
                         size_t n_where_params,
                         vox_orm_exec_cb cb,
                         void* user_data);

/* ===== Delete ===== */

/**
 * 同步删除：DELETE FROM table WHERE where_clause
 */
int vox_orm_delete(vox_db_conn_t* conn,
                  const char* table,
                  const char* where_clause,
                  const vox_db_value_t* where_params,
                  size_t n_where_params,
                  int64_t* out_affected);

int vox_orm_delete_async(vox_db_conn_t* conn,
                         const char* table,
                         const char* where_clause,
                         const vox_db_value_t* where_params,
                         size_t n_where_params,
                         vox_orm_exec_cb cb,
                         void* user_data);

/* ===== Select 单行 ===== */

/**
 * 同步查询单行：SELECT ... FROM table WHERE where_clause LIMIT 1，结果填入 row_struct
 * @param row_struct 非 NULL，至少 row_size 字节；由调用方分配，ORM 只写入
 * @param row_size 结构体大小（如 sizeof(user_row_t)）
 * @return 0 成功且查到一行，-1 失败，1 未查到行（可选约定，实现时用 out_found 更清晰）
 */
int vox_orm_select_one(vox_db_conn_t* conn,
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
 * 异步查询单行：完成时回调，status=0 且 out_found=1 时 row_struct 有效（由 ORM 在 mpool 分配，回调内有效；若需持久化请拷贝）
 */
int vox_orm_select_one_async(vox_db_conn_t* conn,
                             const char* table,
                             const vox_orm_field_t* fields,
                             size_t nfields,
                             size_t row_size,
                             const char* where_clause,
                             const vox_db_value_t* where_params,
                             size_t n_where_params,
                             vox_orm_select_one_cb cb,
                             void* user_data);

/* ===== Select 多行 ===== */

/**
 * 同步查询多行：SELECT ... FROM table WHERE where_clause [ORDER BY ... LIMIT ? OFFSET ?]
 * 每行在内部分配 row_size 字节（mpool），填充后指针 push 到 out_list（vox_vector_t*）
 * @param out_list 非 NULL，由调用方创建（vox_vector_create(conn->mpool)）；元素为 void*，指向 row_size 大小的结构体
 * @param out_row_count 可 NULL；成功时写入行数
 */
int vox_orm_select(vox_db_conn_t* conn,
                   const char* table,
                   const vox_orm_field_t* fields,
                   size_t nfields,
                   size_t row_size,
                   vox_vector_t* out_list,
                   int64_t* out_row_count,
                   const char* where_clause,
                   const vox_db_value_t* where_params,
                   size_t n_where_params);

/**
 * 异步查询多行：完成时 done_cb(status, row_count, user_data)；out_list 在调用前由调用方创建，row_cb 中 ORM 会 push 每行指针
 */
int vox_orm_select_async(vox_db_conn_t* conn,
                         const char* table,
                         const vox_orm_field_t* fields,
                         size_t nfields,
                         size_t row_size,
                         vox_vector_t* out_list,
                         const char* where_clause,
                         const vox_db_value_t* where_params,
                         size_t n_where_params,
                         vox_orm_select_done_cb done_cb,
                         void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* VOX_ORM_H */
