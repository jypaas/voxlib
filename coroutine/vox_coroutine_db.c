/*
 * vox_coroutine_db.c - 数据库协程适配器实现
 */

#include "vox_coroutine_db.h"
#include "vox_coroutine_promise.h"
#include "../db/vox_db_internal.h"
#include "../db/vox_db_pool.h"
#include "../vox_log.h"

#include <string.h>

/* ===== exec操作的协程适配 ===== */

typedef struct {
    vox_coroutine_promise_t* promise;
    int64_t* out_affected_rows;
    int64_t affected_rows;
} db_exec_await_data_t;

static void db_exec_await_cb(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    VOX_UNUSED(conn);
    db_exec_await_data_t* data = (db_exec_await_data_t*)user_data;
    if (!data || !data->promise) return;
    
    if (data->out_affected_rows) {
        *data->out_affected_rows = affected_rows;
    }
    data->affected_rows = affected_rows;
    
    vox_coroutine_promise_complete(data->promise, status, NULL);
}

int vox_coroutine_db_exec_await(vox_coroutine_t* co,
                                 vox_db_conn_t* conn,
                                 const char* sql,
                                 const vox_db_value_t* params,
                                 size_t nparams,
                                 int64_t* out_affected_rows) {
    if (!co || !conn || !sql) return -1;
    
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop) return -1;
    
    /* 创建Promise */
    vox_coroutine_promise_t* promise = vox_coroutine_promise_create(loop);
    if (!promise) return -1;
    
    /* 创建回调数据 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    db_exec_await_data_t* data = (db_exec_await_data_t*)vox_mpool_alloc(mpool, sizeof(db_exec_await_data_t));
    if (!data) {
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    memset(data, 0, sizeof(*data));
    data->promise = promise;
    data->out_affected_rows = out_affected_rows;
    
    /* 调用异步exec */
    if (vox_db_exec_async(conn, sql, params, nparams, db_exec_await_cb, data) != 0) {
        vox_mpool_free(mpool, data);
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    
    /* 等待Promise完成 */
    int status = vox_coroutine_await(co, promise);
    
    /* 清理 */
    vox_coroutine_promise_destroy(promise);
    vox_mpool_free(mpool, data);
    
    return status;
}

/* ===== query操作的协程适配 ===== */

typedef struct {
    vox_coroutine_promise_t* promise;
    vox_db_row_t** out_rows;
    int64_t* out_row_count;
    vox_mpool_t* mpool;
    vox_db_row_t* rows;
    size_t row_capacity;
    size_t row_count;
} db_query_await_data_t;

/* 深拷贝一行数据 */
static int copy_row_data(vox_mpool_t* mpool, const vox_db_row_t* src, vox_db_row_t* dst) {
    if (!mpool || !src || !dst) return -1;
    
    dst->column_count = src->column_count;
    
    if (src->column_count == 0) {
        dst->column_names = NULL;
        dst->values = NULL;
        return 0;
    }
    
    /* 分配列名数组 */
    const char** col_names = (const char**)vox_mpool_alloc(mpool, src->column_count * sizeof(char*));
    if (!col_names) return -1;
    
    /* 分配值数组 */
    vox_db_value_t* values = (vox_db_value_t*)vox_mpool_alloc(mpool, src->column_count * sizeof(vox_db_value_t));
    if (!values) {
        vox_mpool_free(mpool, col_names);
        return -1;
    }
    
    memset(col_names, 0, src->column_count * sizeof(char*));
    memset(values, 0, src->column_count * sizeof(vox_db_value_t));
    
    /* 拷贝列名和值 */
    for (size_t i = 0; i < src->column_count; i++) {
        /* 拷贝列名 */
        if (src->column_names && src->column_names[i]) {
            size_t name_len = strlen(src->column_names[i]);
            char* name_copy = (char*)vox_mpool_alloc(mpool, name_len + 1);
            if (!name_copy) {
                /* 清理已分配的内存 */
                for (size_t j = 0; j < i; j++) {
                    if (col_names[j]) vox_mpool_free(mpool, (void*)col_names[j]);
                    if (values[j].type == VOX_DB_TYPE_TEXT && values[j].u.text.ptr) {
                        vox_mpool_free(mpool, (void*)values[j].u.text.ptr);
                    } else if (values[j].type == VOX_DB_TYPE_BLOB && values[j].u.blob.data) {
                        vox_mpool_free(mpool, (void*)values[j].u.blob.data);
                    }
                }
                vox_mpool_free(mpool, col_names);
                vox_mpool_free(mpool, values);
                return -1;
            }
            memcpy(name_copy, src->column_names[i], name_len + 1);
            col_names[i] = name_copy;
        }
        
        /* 拷贝值 */
        const vox_db_value_t* src_val = &src->values[i];
        vox_db_value_t* dst_val = &values[i];
        dst_val->type = src_val->type;
        
        switch (src_val->type) {
            case VOX_DB_TYPE_I64:
                dst_val->u.i64 = src_val->u.i64;
                break;
            case VOX_DB_TYPE_U64:
                dst_val->u.u64 = src_val->u.u64;
                break;
            case VOX_DB_TYPE_F64:
                dst_val->u.f64 = src_val->u.f64;
                break;
            case VOX_DB_TYPE_BOOL:
                dst_val->u.boolean = src_val->u.boolean;
                break;
            case VOX_DB_TYPE_TEXT: {
                size_t text_len = src_val->u.text.len;
                char* text_copy = (char*)vox_mpool_alloc(mpool, text_len + 1);
                if (!text_copy) {
                    /* 清理已分配的内存 */
                    for (size_t j = 0; j <= i; j++) {
                        if (col_names[j]) vox_mpool_free(mpool, (void*)col_names[j]);
                        if (j < i) {
                            if (values[j].type == VOX_DB_TYPE_TEXT && values[j].u.text.ptr) {
                                vox_mpool_free(mpool, (void*)values[j].u.text.ptr);
                            } else if (values[j].type == VOX_DB_TYPE_BLOB && values[j].u.blob.data) {
                                vox_mpool_free(mpool, (void*)values[j].u.blob.data);
                            }
                        }
                    }
                    vox_mpool_free(mpool, col_names);
                    vox_mpool_free(mpool, values);
                    return -1;
                }
                if (src_val->u.text.ptr && text_len > 0) {
                    memcpy(text_copy, src_val->u.text.ptr, text_len);
                }
                text_copy[text_len] = '\0';
                dst_val->u.text.ptr = text_copy;
                dst_val->u.text.len = text_len;
                break;
            }
            case VOX_DB_TYPE_BLOB: {
                size_t blob_len = src_val->u.blob.len;
                void* blob_copy = NULL;
                if (blob_len > 0) {
                    blob_copy = vox_mpool_alloc(mpool, blob_len);
                    if (!blob_copy) {
                        /* 清理已分配的内存 */
                        for (size_t j = 0; j <= i; j++) {
                            if (col_names[j]) vox_mpool_free(mpool, (void*)col_names[j]);
                            if (j < i) {
                                if (values[j].type == VOX_DB_TYPE_TEXT && values[j].u.text.ptr) {
                                    vox_mpool_free(mpool, (void*)values[j].u.text.ptr);
                                } else if (values[j].type == VOX_DB_TYPE_BLOB && values[j].u.blob.data) {
                                    vox_mpool_free(mpool, (void*)values[j].u.blob.data);
                                }
                            }
                        }
                        vox_mpool_free(mpool, col_names);
                        vox_mpool_free(mpool, values);
                        return -1;
                    }
                    if (src_val->u.blob.data && blob_len > 0) {
                        memcpy(blob_copy, src_val->u.blob.data, blob_len);
                    }
                }
                dst_val->u.blob.data = blob_copy;
                dst_val->u.blob.len = blob_len;
                break;
            }
            case VOX_DB_TYPE_NULL:
            default:
                break;
        }
    }
    
    dst->column_names = (const char* const*)col_names;
    dst->values = values;
    return 0;
}

static void db_query_row_cb(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    VOX_UNUSED(conn);
    db_query_await_data_t* data = (db_query_await_data_t*)user_data;
    if (!data || !row) return;
    
    /* 扩展行数组 */
    if (data->row_count >= data->row_capacity) {
        size_t new_capacity = data->row_capacity == 0 ? 16 : data->row_capacity * 2;
        vox_db_row_t* new_rows = (vox_db_row_t*)vox_mpool_alloc(data->mpool, new_capacity * sizeof(vox_db_row_t));
        if (!new_rows) return; /* 内存不足，跳过此行 */
        
        if (data->rows) {
            memcpy(new_rows, data->rows, data->row_count * sizeof(vox_db_row_t));
        }
        data->rows = new_rows;
        data->row_capacity = new_capacity;
    }
    
    /* 深拷贝行数据 */
    if (copy_row_data(data->mpool, row, &data->rows[data->row_count]) != 0) {
        return; /* 拷贝失败，跳过此行 */
    }
    
    data->row_count++;
}

static void db_query_done_cb(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    VOX_UNUSED(conn);
    VOX_UNUSED(row_count);
    db_query_await_data_t* data = (db_query_await_data_t*)user_data;
    if (!data || !data->promise) return;
    
    if (data->out_row_count) {
        *data->out_row_count = (int64_t)data->row_count;
    }
    
    if (data->out_rows) {
        *data->out_rows = data->rows;
    }
    
    /* 注意：data结构体和行数据都从loop的内存池分配，会在loop销毁时自动释放
     * 这里不释放data，让行数据在协程结束后仍然有效 */
    
    vox_coroutine_promise_complete(data->promise, status, NULL);
}

int vox_coroutine_db_query_await(vox_coroutine_t* co,
                                  vox_db_conn_t* conn,
                                  const char* sql,
                                  const vox_db_value_t* params,
                                  size_t nparams,
                                  vox_db_row_t** out_rows,
                                  int64_t* out_row_count) {
    if (!co || !conn || !sql) return -1;
    
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop) return -1;
    
    /* 创建Promise */
    vox_coroutine_promise_t* promise = vox_coroutine_promise_create(loop);
    if (!promise) return -1;
    
    /* 创建回调数据（从loop的内存池分配，会在loop销毁时自动释放） */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    db_query_await_data_t* data = (db_query_await_data_t*)vox_mpool_alloc(mpool, sizeof(db_query_await_data_t));
    if (!data) {
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    memset(data, 0, sizeof(*data));
    data->promise = promise;
    data->out_rows = out_rows;
    data->out_row_count = out_row_count;
    data->mpool = mpool;
    data->rows = NULL;
    data->row_capacity = 0;
    data->row_count = 0;
    
    /* 调用异步query */
    if (vox_db_query_async(conn, sql, params, nparams, db_query_row_cb, db_query_done_cb, data) != 0) {
        vox_mpool_free(mpool, data);
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    
    /* 等待Promise完成 */
    int status = vox_coroutine_await(co, promise);
    
    /* 清理Promise（data和行数据从loop的内存池分配，会在loop销毁时自动释放） */
    vox_coroutine_promise_destroy(promise);
    
    return status;
}

/* ===== 事务操作的协程适配 ===== */

typedef struct {
    vox_coroutine_promise_t* promise;
} db_tx_await_data_t;

static void db_tx_await_cb(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    VOX_UNUSED(conn);
    VOX_UNUSED(affected_rows);
    db_tx_await_data_t* data = (db_tx_await_data_t*)user_data;
    if (!data || !data->promise) return;
    
    vox_coroutine_promise_complete(data->promise, status, NULL);
}

int vox_coroutine_db_begin_transaction_await(vox_coroutine_t* co,
                                             vox_db_conn_t* conn) {
    if (!co || !conn) return -1;
    
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop) return -1;
    
    /* 创建Promise */
    vox_coroutine_promise_t* promise = vox_coroutine_promise_create(loop);
    if (!promise) return -1;
    
    /* 创建回调数据 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    db_tx_await_data_t* data = (db_tx_await_data_t*)vox_mpool_alloc(mpool, sizeof(db_tx_await_data_t));
    if (!data) {
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    memset(data, 0, sizeof(*data));
    data->promise = promise;
    
    /* 调用异步begin_transaction */
    if (vox_db_begin_transaction_async(conn, db_tx_await_cb, data) != 0) {
        vox_mpool_free(mpool, data);
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    
    /* 等待Promise完成 */
    int status = vox_coroutine_await(co, promise);
    
    /* 清理 */
    vox_coroutine_promise_destroy(promise);
    vox_mpool_free(mpool, data);
    
    return status;
}

int vox_coroutine_db_commit_await(vox_coroutine_t* co,
                                   vox_db_conn_t* conn) {
    if (!co || !conn) return -1;
    
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop) return -1;
    
    /* 创建Promise */
    vox_coroutine_promise_t* promise = vox_coroutine_promise_create(loop);
    if (!promise) return -1;
    
    /* 创建回调数据 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    db_tx_await_data_t* data = (db_tx_await_data_t*)vox_mpool_alloc(mpool, sizeof(db_tx_await_data_t));
    if (!data) {
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    memset(data, 0, sizeof(*data));
    data->promise = promise;
    
    /* 调用异步commit */
    if (vox_db_commit_async(conn, db_tx_await_cb, data) != 0) {
        vox_mpool_free(mpool, data);
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    
    /* 等待Promise完成 */
    int status = vox_coroutine_await(co, promise);
    
    /* 清理 */
    vox_coroutine_promise_destroy(promise);
    vox_mpool_free(mpool, data);
    
    return status;
}

int vox_coroutine_db_rollback_await(vox_coroutine_t* co,
                                    vox_db_conn_t* conn) {
    if (!co || !conn) return -1;
    
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop) return -1;
    
    /* 创建Promise */
    vox_coroutine_promise_t* promise = vox_coroutine_promise_create(loop);
    if (!promise) return -1;
    
    /* 创建回调数据 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    db_tx_await_data_t* data = (db_tx_await_data_t*)vox_mpool_alloc(mpool, sizeof(db_tx_await_data_t));
    if (!data) {
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    memset(data, 0, sizeof(*data));
    data->promise = promise;
    
    /* 调用异步rollback */
    if (vox_db_rollback_async(conn, db_tx_await_cb, data) != 0) {
        vox_mpool_free(mpool, data);
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    
    /* 等待Promise完成 */
    int status = vox_coroutine_await(co, promise);
    
    /* 清理 */
    vox_coroutine_promise_destroy(promise);
    vox_mpool_free(mpool, data);
    
    return status;
}

/* ===== 连接池协程适配（仅用 acquire/release，不用池的便捷接口，与 vox_coroutine_redis 一致） ===== */

typedef struct {
    vox_coroutine_promise_t* promise;
    int status;
    vox_db_conn_t* conn;  /* 成功时由回调写入 */
} db_pool_acquire_state_t;

static void db_pool_acquire_cb(vox_db_pool_t* pool,
                               vox_db_conn_t* conn,
                               int status,
                               void* user_data) {
    (void)pool;
    db_pool_acquire_state_t* state = (db_pool_acquire_state_t*)user_data;
    if (!state) return;
    state->status = status;
    state->conn = conn;
    vox_coroutine_promise_complete(state->promise, status, NULL);
}

int vox_coroutine_db_pool_acquire_await(vox_coroutine_t* co,
                                         vox_db_pool_t* db_pool,
                                         vox_db_conn_t** out_conn) {
    if (!co || !db_pool || !out_conn) return -1;

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop) return -1;

    vox_coroutine_promise_t* promise = vox_coroutine_promise_create(loop);
    if (!promise) return -1;

    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    db_pool_acquire_state_t* state = (db_pool_acquire_state_t*)vox_mpool_alloc(mpool, sizeof(db_pool_acquire_state_t));
    if (!state) {
        vox_coroutine_promise_destroy(promise);
        return -1;
    }
    memset(state, 0, sizeof(*state));
    state->promise = promise;

    if (vox_db_pool_acquire_async(db_pool, db_pool_acquire_cb, state) != 0) {
        vox_mpool_free(mpool, state);
        vox_coroutine_promise_destroy(promise);
        return -1;
    }

    int ret = vox_coroutine_await(co, promise);
    vox_coroutine_promise_destroy(promise);
    if (ret == 0 && state->status == 0 && state->conn) {
        *out_conn = state->conn;
        vox_mpool_free(mpool, state);
        return 0;
    }
    *out_conn = NULL;
    vox_mpool_free(mpool, state);
    return -1;
}

int vox_coroutine_db_pool_exec_await(vox_coroutine_t* co,
                                      vox_db_pool_t* db_pool,
                                      const char* sql,
                                      const vox_db_value_t* params,
                                      size_t nparams,
                                      int64_t* out_affected_rows) {
    vox_db_conn_t* conn = NULL;
    if (vox_coroutine_db_pool_acquire_await(co, db_pool, &conn) != 0)
        return -1;
    int ret = vox_coroutine_db_exec_await(co, conn, sql, params, nparams, out_affected_rows);
    vox_db_pool_release(db_pool, conn);
    return ret;
}

int vox_coroutine_db_pool_query_await(vox_coroutine_t* co,
                                       vox_db_pool_t* db_pool,
                                       const char* sql,
                                       const vox_db_value_t* params,
                                       size_t nparams,
                                       vox_db_row_t** out_rows,
                                       int64_t* out_row_count) {
    vox_db_conn_t* conn = NULL;
    if (vox_coroutine_db_pool_acquire_await(co, db_pool, &conn) != 0)
        return -1;
    int ret = vox_coroutine_db_query_await(co, conn, sql, params, nparams, out_rows, out_row_count);
    vox_db_pool_release(db_pool, conn);
    return ret;
}
