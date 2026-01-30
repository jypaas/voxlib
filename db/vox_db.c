/*
 * vox_db.c - 数据库抽象层核心实现
 */

#include "vox_db_internal.h"

#include "../vox_log.h"

#include <string.h>

typedef enum {
    VOX_DB_OP_EXEC = 0,
    VOX_DB_OP_QUERY
} vox_db_op_t;

typedef struct vox_db_req {
    vox_db_op_t op;
    vox_db_conn_t* conn;

    const char* sql;
    const vox_db_value_t* params;
    size_t nparams;

    /* 1 = 本任务经 loop 入队执行（use_loop_thread_for_async），回调时已在 loop 线程，可省一次 queue */
    unsigned on_loop_thread : 1;

    union {
        struct {
            vox_db_exec_cb cb;
            void* user_data;
        } exec;
        struct {
            vox_db_row_cb row_cb;
            vox_db_done_cb done_cb;
            void* user_data;
        } query;
    } u;
} vox_db_req_t;

typedef struct {
    vox_db_conn_t* conn;
    vox_db_exec_cb cb;
    void* user_data;
    int status;
    int64_t affected;
} vox_db_exec_call_t;

typedef struct {
    vox_db_conn_t* conn;
    vox_db_done_cb cb;
    void* user_data;
    int status;
    int64_t row_count;
} vox_db_done_call_t;

typedef struct {
    vox_db_conn_t* conn;
    vox_db_row_cb cb;
    void* user_data;
    size_t column_count;
    char** column_names;     /* [column_count] */
    vox_db_value_t* values;  /* [column_count] (deep-copied for TEXT/BLOB) */
} vox_db_row_call_t;

static void db_exec_task(void* user_data);
static void db_query_task(void* user_data);

static const vox_db_driver_vtbl_t* select_vtbl(vox_db_driver_t driver) {
    switch (driver) {
        case VOX_DB_DRIVER_SQLITE3:
#ifdef VOX_USE_SQLITE3
            return vox_db_sqlite3_vtbl();
#else
            return NULL;
#endif
        case VOX_DB_DRIVER_DUCKDB:
#ifdef VOX_USE_DUCKDB
            return vox_db_duckdb_vtbl();
#else
            return NULL;
#endif
        case VOX_DB_DRIVER_PGSQL:
#ifdef VOX_USE_PGSQL
            return vox_db_pgsql_vtbl();
#else
            return NULL;
#endif
        case VOX_DB_DRIVER_MYSQL:
#ifdef VOX_USE_MYSQL
            return vox_db_mysql_vtbl();
#else
            return NULL;
#endif
        default:
            return NULL;
    }
}

int vox_db_set_callback_mode(vox_db_conn_t* conn, vox_db_callback_mode_t mode) {
    if (!conn) return -1;
    if (mode != VOX_DB_CALLBACK_WORKER && mode != VOX_DB_CALLBACK_LOOP) return -1;
    conn->cb_mode = mode;
    return 0;
}

vox_db_callback_mode_t vox_db_get_callback_mode(vox_db_conn_t* conn) {
    return conn ? conn->cb_mode : VOX_DB_CALLBACK_WORKER;
}

int vox_db_conn_try_begin(vox_db_conn_t* conn) {
    if (!conn) return -1;
    if (vox_mutex_lock(&conn->mu) != 0) return -1;
    if (conn->busy) {
        vox_mutex_unlock(&conn->mu);
        return -1;
    }
    conn->busy = true;
    vox_mutex_unlock(&conn->mu);
    return 0;
}

void vox_db_conn_end(vox_db_conn_t* conn) {
    if (!conn) return;
    if (vox_mutex_lock(&conn->mu) != 0) return;
    conn->busy = false;
    vox_mutex_unlock(&conn->mu);
}

int vox_db_conn_ping_and_reconnect(vox_db_conn_t* conn) {
    if (!conn || !conn->vtbl) return -1;

    /* 如果驱动没有实现 ping 方法，直接返回成功 */
    if (!conn->vtbl->ping) return 0;

    /* 检测连接状态 */
    int ping_result = conn->vtbl->ping(conn);
    if (ping_result == 0) {
        /* 连接正常 */
        return 0;
    }

    /* 连接已断开，尝试重连 */
    VOX_LOG_WARN("[db] connection lost (%s), attempting reconnect...", 
                 conn->vtbl->name ? conn->vtbl->name : "unknown");

    /* 先断开旧连接 */
    if (conn->vtbl->disconnect) {
        conn->vtbl->disconnect(conn);
        conn->native = NULL;
    }

    /* 使用保存的连接信息重连 */
    if (!conn->conninfo || conn->vtbl->connect(conn, conn->conninfo) != 0) {
        const char* err = conn->vtbl->last_error ? conn->vtbl->last_error(conn) : NULL;
        VOX_LOG_ERROR("[db] reconnect failed (%s): %s", 
                      conn->vtbl->name ? conn->vtbl->name : "unknown", 
                      err ? err : "(no error)");
        return -1;
    }

    VOX_LOG_INFO("[db] reconnect successful (%s)", conn->vtbl->name ? conn->vtbl->name : "unknown");
    return 0;
}

vox_db_conn_t* vox_db_connect(vox_loop_t* loop, vox_db_driver_t driver, const char* conninfo) {
    if (!loop || !conninfo) return NULL;

    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) return NULL;

    const vox_db_driver_vtbl_t* vtbl = select_vtbl(driver);
    if (!vtbl) {
        VOX_LOG_ERROR("[db] driver not enabled for driver=%d", (int)driver);
        return NULL;
    }
    else {
        VOX_LOG_INFO("[db] driver selected: %s", vtbl->name ? vtbl->name : "unknown");
    }

    vox_db_conn_t* conn = (vox_db_conn_t*)vox_mpool_alloc(mpool, sizeof(vox_db_conn_t));
    if (!conn) return NULL;
    memset(conn, 0, sizeof(*conn));
    conn->loop = loop;
    conn->mpool = mpool;
    conn->driver = driver;
    conn->vtbl = vtbl;
    conn->native = NULL;
    conn->busy = false;
    conn->cb_mode = VOX_DB_CALLBACK_WORKER;

    /* 保存连接信息用于重连 */
    size_t conninfo_len = strlen(conninfo) + 1;
    conn->conninfo = (char*)vox_mpool_alloc(mpool, conninfo_len);
    if (!conn->conninfo) {
        vox_mpool_free(mpool, conn);
        return NULL;
    }
    memcpy(conn->conninfo, conninfo, conninfo_len);

    if (vox_mutex_create(&conn->mu) != 0) {
        vox_mpool_free(mpool, conn->conninfo);
        vox_mpool_free(mpool, conn);
        return NULL;
    }

    if (conn->vtbl->connect(conn, conninfo) != 0) {
        const char* err = conn->vtbl->last_error ? conn->vtbl->last_error(conn) : NULL;
        VOX_LOG_ERROR("[db] connect failed (%s): %s", conn->vtbl->name ? conn->vtbl->name : "unknown", err ? err : "(no error)");
        if (conn->vtbl->disconnect)
            conn->vtbl->disconnect(conn);
        vox_mutex_destroy(&conn->mu);
        vox_mpool_free(mpool, conn->conninfo);
        vox_mpool_free(mpool, conn);
        return NULL;
    }

    return conn;
}

void vox_db_disconnect(vox_db_conn_t* conn) {
    if (!conn) return;

    vox_mpool_t* mpool = conn->mpool;
    if (conn->vtbl && conn->vtbl->disconnect) {
        conn->vtbl->disconnect(conn);
    }
    vox_mutex_destroy(&conn->mu);
    if (mpool) {
        if (conn->conninfo) vox_mpool_free(mpool, conn->conninfo);
        vox_mpool_free(mpool, conn);
    }
}

vox_loop_t* vox_db_get_loop(vox_db_conn_t* conn) {
    return conn ? conn->loop : NULL;
}

vox_db_driver_t vox_db_get_driver(vox_db_conn_t* conn) {
    return conn ? conn->driver : (vox_db_driver_t)0;
}

const char* vox_db_last_error(vox_db_conn_t* conn) {
    if (!conn || !conn->vtbl || !conn->vtbl->last_error) return NULL;
    return conn->vtbl->last_error(conn);
}

int vox_db_exec(vox_db_conn_t* conn,
                const char* sql,
                const vox_db_value_t* params,
                size_t nparams,
                int64_t* out_affected_rows) {
    if (!conn || !sql) return -1;
    if (!conn->vtbl || !conn->vtbl->exec) return -1;

    if (vox_db_conn_try_begin(conn) != 0) return -1;

    int64_t affected = 0;
    int rc = conn->vtbl->exec(conn, sql, params, nparams, &affected);

    vox_db_conn_end(conn);
    if (out_affected_rows) *out_affected_rows = affected;
    return (rc == 0) ? 0 : -1;
}

int vox_db_query(vox_db_conn_t* conn,
                 const char* sql,
                 const vox_db_value_t* params,
                 size_t nparams,
                 vox_db_row_cb row_cb,
                 void* row_user_data,
                 int64_t* out_row_count) {
    if (!conn || !sql) return -1;
    if (!conn->vtbl || !conn->vtbl->query) return -1;

    if (vox_db_conn_try_begin(conn) != 0) return -1;

    int64_t row_count = 0;
    int rc = conn->vtbl->query(conn, sql, params, nparams, row_cb, row_user_data, &row_count);

    vox_db_conn_end(conn);
    if (out_row_count) *out_row_count = row_count;
    return (rc == 0) ? 0 : -1;
}

static void db_loop_invoke_exec(vox_loop_t* loop, void* user_data) {
    VOX_UNUSED(loop);
    vox_db_exec_call_t* call = (vox_db_exec_call_t*)user_data;
    if (!call) return;
    vox_db_conn_end(call->conn);  /* 先释放 busy，以便回调内可再次提交 async */
    if (call->cb) {
        call->cb(call->conn, call->status, call->affected, call->user_data);
    }
    vox_mpool_free(call->conn->mpool, call);
}

static void db_loop_invoke_done(vox_loop_t* loop, void* user_data) {
    VOX_UNUSED(loop);
    vox_db_done_call_t* call = (vox_db_done_call_t*)user_data;
    if (!call) return;
    vox_db_conn_end(call->conn);  /* 先释放 busy，以便回调内可再次提交 async */
    if (call->cb) {
        call->cb(call->conn, call->status, call->row_count, call->user_data);
    }
    vox_mpool_free(call->conn->mpool, call);
}

static void db_free_row_call(vox_db_row_call_t* call) {
    if (!call || !call->conn) return;
    vox_mpool_t* mp = call->conn->mpool;
    if (call->column_names) {
        for (size_t i = 0; i < call->column_count; i++) {
            if (call->column_names[i]) vox_mpool_free(mp, call->column_names[i]);
        }
        vox_mpool_free(mp, call->column_names);
    }
    if (call->values) {
        for (size_t i = 0; i < call->column_count; i++) {
            if (call->values[i].type == VOX_DB_TYPE_TEXT) {
                if (call->values[i].u.text.ptr) vox_mpool_free(mp, (void*)call->values[i].u.text.ptr);
            } else if (call->values[i].type == VOX_DB_TYPE_BLOB) {
                if (call->values[i].u.blob.data) vox_mpool_free(mp, (void*)call->values[i].u.blob.data);
            }
        }
        vox_mpool_free(mp, call->values);
    }
    vox_mpool_free(mp, call);
}

static void db_loop_invoke_row(vox_loop_t* loop, void* user_data) {
    VOX_UNUSED(loop);
    vox_db_row_call_t* call = (vox_db_row_call_t*)user_data;
    if (!call) return;
    if (call->cb) {
        vox_db_row_t row = {
            .column_count = call->column_count,
            .column_names = (const char* const*)call->column_names,
            .values = call->values
        };
        call->cb(call->conn, &row, call->user_data);
    }
    db_free_row_call(call);
}

static int db_copy_row(vox_db_conn_t* conn, const vox_db_row_t* src, vox_db_row_call_t** out_call) {
    if (!conn || !src || !out_call) return -1;
    *out_call = NULL;

    vox_mpool_t* mp = conn->mpool;
    vox_db_row_call_t* call = (vox_db_row_call_t*)vox_mpool_alloc(mp, sizeof(vox_db_row_call_t));
    if (!call) return -1;
    memset(call, 0, sizeof(*call));
    call->conn = conn;
    call->column_count = src->column_count;

    if (src->column_count > 0) {
        call->column_names = (char**)vox_mpool_alloc(mp, src->column_count * sizeof(char*));
        call->values = (vox_db_value_t*)vox_mpool_alloc(mp, src->column_count * sizeof(vox_db_value_t));
        if (!call->column_names || !call->values) {
            db_free_row_call(call);
            return -1;
        }
        memset(call->column_names, 0, src->column_count * sizeof(char*));
        memset(call->values, 0, src->column_count * sizeof(vox_db_value_t));

        for (size_t i = 0; i < src->column_count; i++) {
            const char* name = (src->column_names && src->column_names[i]) ? src->column_names[i] : "";
            size_t nlen = strlen(name);
            call->column_names[i] = (char*)vox_mpool_alloc(mp, nlen + 1);
            if (!call->column_names[i]) {
                db_free_row_call(call);
                return -1;
            }
            memcpy(call->column_names[i], name, nlen + 1);

            const vox_db_value_t* v = &src->values[i];
            call->values[i].type = v->type;
            switch (v->type) {
                case VOX_DB_TYPE_I64:
                    call->values[i].u.i64 = v->u.i64;
                    break;
                case VOX_DB_TYPE_U64:
                    call->values[i].u.u64 = v->u.u64;
                    break;
                case VOX_DB_TYPE_F64:
                    call->values[i].u.f64 = v->u.f64;
                    break;
                case VOX_DB_TYPE_BOOL:
                    call->values[i].u.boolean = v->u.boolean;
                    break;
                case VOX_DB_TYPE_TEXT: {
                    size_t len = v->u.text.len;
                    char* p = (char*)vox_mpool_alloc(mp, len + 1);
                    if (!p) {
                        db_free_row_call(call);
                        return -1;
                    }
                    if (v->u.text.ptr && len > 0) memcpy(p, v->u.text.ptr, len);
                    p[len] = '\0';
                    call->values[i].u.text.ptr = p;
                    call->values[i].u.text.len = len;
                    break;
                }
                case VOX_DB_TYPE_BLOB: {
                    size_t len = v->u.blob.len;
                    void* p = NULL;
                    if (len > 0) {
                        p = vox_mpool_alloc(mp, len);
                        if (!p) {
                            db_free_row_call(call);
                            return -1;
                        }
                        if (v->u.blob.data) memcpy(p, v->u.blob.data, len);
                    }
                    call->values[i].u.blob.data = p;
                    call->values[i].u.blob.len = len;
                    break;
                }
                case VOX_DB_TYPE_NULL:
                default:
                    break;
            }
        }
    }

    *out_call = call;
    return 0;
}

static void db_row_dispatch(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    vox_db_req_t* req = (vox_db_req_t*)user_data;
    if (!req || !conn) return;
    if (!req->u.query.row_cb || !row) return;

    /* 已在 loop 线程（use_loop_thread_for_async）：直接回调，保证行回调在 done 之前执行，避免 user_data 被 done 里释放 */
    if (req->on_loop_thread) {
        req->u.query.row_cb(conn, row, req->u.query.user_data);
        return;
    }
    if (conn->cb_mode == VOX_DB_CALLBACK_WORKER) {
        req->u.query.row_cb(conn, row, req->u.query.user_data);
        return;
    }

    /* LOOP 模式（非 loop 线程）：深拷贝本行数据后切回 loop */
    vox_db_row_call_t* call = NULL;
    if (db_copy_row(conn, row, &call) != 0) {
        /* 拷贝失败：退化为工作线程直接回调（避免丢数据） */
        req->u.query.row_cb(conn, row, req->u.query.user_data);
        return;
    }
    call->cb = req->u.query.row_cb;
    call->user_data = req->u.query.user_data;

    if (vox_loop_queue_work(conn->loop, db_loop_invoke_row, call) != 0) {
        /* 入队失败：退化为工作线程直接回调 */
        db_free_row_call(call);
        req->u.query.row_cb(conn, row, req->u.query.user_data);
    }
}

/* loop 线程执行包装（供 use_loop_thread_for_async 使用） */
static void db_exec_task_loop(vox_loop_t* loop, void* user_data) {
    (void)loop;
    db_exec_task(user_data);
}

static void db_exec_task(void* user_data) {
    vox_db_req_t* req = (vox_db_req_t*)user_data;
    vox_db_conn_t* conn = req ? req->conn : NULL;
    if (!req || !conn) return;
    if (!conn->vtbl || !conn->vtbl->exec) {
        if (req->u.exec.cb) {
            if (conn->cb_mode == VOX_DB_CALLBACK_LOOP) {
                vox_db_exec_call_t* call = (vox_db_exec_call_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_exec_call_t));
                if (call) {
                    call->conn = conn;
                    call->cb = req->u.exec.cb;
                    call->user_data = req->u.exec.user_data;
                    call->status = -1;
                    call->affected = 0;
                    if (vox_loop_queue_work(conn->loop, db_loop_invoke_exec, call) != 0) {
                        vox_mpool_free(conn->mpool, call);
                        req->u.exec.cb(conn, -1, 0, req->u.exec.user_data);
                        vox_db_conn_end(conn);
                    }
                } else {
                    req->u.exec.cb(conn, -1, 0, req->u.exec.user_data);
                    vox_db_conn_end(conn);
                }
            } else {
                req->u.exec.cb(conn, -1, 0, req->u.exec.user_data);
                vox_db_conn_end(conn);
            }
        } else {
            vox_db_conn_end(conn);
        }
        vox_mpool_free(conn->mpool, req);
        return;
    }

    int status = 0;
    int64_t affected = 0;
    if (conn->vtbl->exec(conn, req->sql, req->params, req->nparams, &affected) != 0) {
        status = -1;
    }

    /* SQLite/DuckDB 等驱动在“上次调用成功”时 last_error 可能返回 "not an error"，此时按成功处理 */
    if (status != 0 && conn->vtbl->last_error) {
        const char* err = conn->vtbl->last_error(conn);
        if (err && strcmp(err, "not an error") == 0)
            status = 0;
    }

    /* 注意：LOOP 模式下需在回调前释放 busy，否则用户在回调内再次提交 async 会因 conn 仍 busy 而失败 */
    if (req->u.exec.cb) {
        /* 已在 loop 线程（use_loop_thread_for_async）且用户要 LOOP 回调：直接调，避免冗余 queue */
        if (req->on_loop_thread && conn->cb_mode == VOX_DB_CALLBACK_LOOP) {
            vox_db_conn_end(conn);
            req->u.exec.cb(conn, status, affected, req->u.exec.user_data);
        } else if (conn->cb_mode == VOX_DB_CALLBACK_LOOP) {
            vox_db_exec_call_t* call = (vox_db_exec_call_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_exec_call_t));
            if (call) {
                call->conn = conn;
                call->cb = req->u.exec.cb;
                call->user_data = req->u.exec.user_data;
                call->status = status;
                call->affected = affected;
                if (vox_loop_queue_work(conn->loop, db_loop_invoke_exec, call) != 0) {
                    vox_mpool_free(conn->mpool, call);
                    req->u.exec.cb(conn, status, affected, req->u.exec.user_data);
                    vox_db_conn_end(conn);  /* 入队失败时释放 */
                }
            } else {
                req->u.exec.cb(conn, status, affected, req->u.exec.user_data);
                vox_db_conn_end(conn);  /* 分配失败时释放 */
            }
        } else {
            /* WORKER 模式：直接回调，然后立即释放 */
            req->u.exec.cb(conn, status, affected, req->u.exec.user_data);
            vox_db_conn_end(conn);
        }
    } else {
        /* 没有回调，直接释放 */
        vox_db_conn_end(conn);
    }
    vox_mpool_free(conn->mpool, req);
}

static void db_query_task_loop(vox_loop_t* loop, void* user_data) {
    (void)loop;
    db_query_task(user_data);
}

static void db_query_task(void* user_data) {
    vox_db_req_t* req = (vox_db_req_t*)user_data;
    vox_db_conn_t* conn = req ? req->conn : NULL;
    if (!req || !conn) return;
    if (!conn->vtbl || !conn->vtbl->query) {
        if (req->u.query.done_cb) {
            if (conn->cb_mode == VOX_DB_CALLBACK_LOOP) {
                vox_db_done_call_t* call = (vox_db_done_call_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_done_call_t));
                if (call) {
                    call->conn = conn;
                    call->cb = req->u.query.done_cb;
                    call->user_data = req->u.query.user_data;
                    call->status = -1;
                    call->row_count = 0;
                    if (vox_loop_queue_work(conn->loop, db_loop_invoke_done, call) != 0) {
                        vox_mpool_free(conn->mpool, call);
                        req->u.query.done_cb(conn, -1, 0, req->u.query.user_data);
                        vox_db_conn_end(conn);
                    }
                } else {
                    req->u.query.done_cb(conn, -1, 0, req->u.query.user_data);
                    vox_db_conn_end(conn);
                }
            } else {
                req->u.query.done_cb(conn, -1, 0, req->u.query.user_data);
                vox_db_conn_end(conn);
            }
        } else {
            vox_db_conn_end(conn);
        }
        vox_mpool_free(conn->mpool, req);
        return;
    }

    int status = 0;
    int64_t rows = 0;
    /* 统一通过 row dispatch 控制回调线程模式 */
    if (conn->vtbl->query(conn, req->sql, req->params, req->nparams,
                          db_row_dispatch, req, &rows) != 0) {
        status = -1;
    }

    /* SQLite 等驱动在“上次调用成功”时 last_error 会返回 "not an error"，此时按成功处理 */
    if (status != 0 && conn->vtbl->last_error) {
        const char* err = conn->vtbl->last_error(conn);
        if (err && strcmp(err, "not an error") == 0)
            status = 0;
    }

    /* 注意：不在这里调用 vox_db_conn_end()，busy 标志由回调函数释放
     * 这样可以防止连接在回调执行前被其他线程重新获取 */
    if (req->u.query.done_cb) {
        /* 已在 loop 线程且用户要 LOOP 回调：直接调，避免冗余 queue */
        if (req->on_loop_thread && conn->cb_mode == VOX_DB_CALLBACK_LOOP) {
            req->u.query.done_cb(conn, status, rows, req->u.query.user_data);
            vox_db_conn_end(conn);
        } else if (conn->cb_mode == VOX_DB_CALLBACK_LOOP) {
            vox_db_done_call_t* call = (vox_db_done_call_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_done_call_t));
            if (call) {
                call->conn = conn;
                call->cb = req->u.query.done_cb;
                call->user_data = req->u.query.user_data;
                call->status = status;
                call->row_count = rows;
                if (vox_loop_queue_work(conn->loop, db_loop_invoke_done, call) != 0) {
                    vox_mpool_free(conn->mpool, call);
                    req->u.query.done_cb(conn, status, rows, req->u.query.user_data);
                    vox_db_conn_end(conn);  /* 入队失败时释放 */
                }
            } else {
                req->u.query.done_cb(conn, status, rows, req->u.query.user_data);
                vox_db_conn_end(conn);  /* 分配失败时释放 */
            }
        } else {
            /* WORKER 模式：直接回调，然后立即释放 */
            req->u.query.done_cb(conn, status, rows, req->u.query.user_data);
            vox_db_conn_end(conn);
        }
    } else {
        /* 没有回调，直接释放 */
        vox_db_conn_end(conn);
    }
    vox_mpool_free(conn->mpool, req);
}

int vox_db_exec_async(vox_db_conn_t* conn,
                      const char* sql,
                      const vox_db_value_t* params,
                      size_t nparams,
                      vox_db_exec_cb cb,
                      void* user_data) {
    if (!conn || !sql) return -1;
    if (!conn->vtbl || !conn->vtbl->exec) return -1;

    if (vox_db_conn_try_begin(conn) != 0) return -1;

    vox_db_req_t* req = (vox_db_req_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_req_t));
    if (!req) {
        vox_db_conn_end(conn);
        return -1;
    }
    memset(req, 0, sizeof(*req));
    req->op = VOX_DB_OP_EXEC;
    req->conn = conn;
    req->sql = sql;
    req->params = params;
    req->nparams = nparams;
    req->u.exec.cb = cb;
    req->u.exec.user_data = user_data;

    /* 部分驱动（如 SQLite 非线程安全构建）要求连接仅在创建线程使用，则投递到 loop 线程执行 */
    if (conn->vtbl && conn->vtbl->use_loop_thread_for_async) {
        req->on_loop_thread = 1;
        if (vox_loop_queue_work(conn->loop, db_exec_task_loop, req) != 0) {
            vox_mpool_free(conn->mpool, req);
            vox_db_conn_end(conn);
            return -1;
        }
        return 0;
    }

    vox_tpool_t* tpool = vox_loop_get_thread_pool(conn->loop);
    if (!tpool) {
        vox_mpool_free(conn->mpool, req);
        vox_db_conn_end(conn);
        return -1;
    }

    if (vox_tpool_submit(tpool, db_exec_task, req, NULL) != 0) {
        vox_mpool_free(conn->mpool, req);
        vox_db_conn_end(conn);
        return -1;
    }

    return 0;
}

int vox_db_query_async(vox_db_conn_t* conn,
                       const char* sql,
                       const vox_db_value_t* params,
                       size_t nparams,
                       vox_db_row_cb row_cb,
                       vox_db_done_cb done_cb,
                       void* user_data) {
    if (!conn || !sql) return -1;
    if (!conn->vtbl || !conn->vtbl->query) return -1;

    if (vox_db_conn_try_begin(conn) != 0) return -1;

    vox_db_req_t* req = (vox_db_req_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_req_t));
    if (!req) {
        vox_db_conn_end(conn);
        return -1;
    }
    memset(req, 0, sizeof(*req));
    req->op = VOX_DB_OP_QUERY;
    req->conn = conn;
    req->sql = sql;
    req->params = params;
    req->nparams = nparams;
    req->u.query.row_cb = row_cb;
    req->u.query.done_cb = done_cb;
    req->u.query.user_data = user_data;

    if (conn->vtbl && conn->vtbl->use_loop_thread_for_async) {
        req->on_loop_thread = 1;
        if (vox_loop_queue_work(conn->loop, db_query_task_loop, req) != 0) {
            vox_mpool_free(conn->mpool, req);
            vox_db_conn_end(conn);
            return -1;
        }
        return 0;
    }

    vox_tpool_t* tpool = vox_loop_get_thread_pool(conn->loop);
    if (!tpool) {
        vox_mpool_free(conn->mpool, req);
        vox_db_conn_end(conn);
        return -1;
    }

    if (vox_tpool_submit(tpool, db_query_task, req, NULL) != 0) {
        vox_mpool_free(conn->mpool, req);
        vox_db_conn_end(conn);
        return -1;
    }

    return 0;
}

/* ===== 事务处理 ===== */

int vox_db_begin_transaction(vox_db_conn_t* conn) {
    if (!conn) return -1;
    if (!conn->vtbl || !conn->vtbl->begin_transaction) return -1;

    if (vox_db_conn_try_begin(conn) != 0) return -1;

    int rc = conn->vtbl->begin_transaction(conn);

    vox_db_conn_end(conn);
    return (rc == 0) ? 0 : -1;
}

int vox_db_commit(vox_db_conn_t* conn) {
    if (!conn) return -1;
    if (!conn->vtbl || !conn->vtbl->commit) return -1;

    if (vox_db_conn_try_begin(conn) != 0) return -1;

    int rc = conn->vtbl->commit(conn);

    vox_db_conn_end(conn);
    return (rc == 0) ? 0 : -1;
}

int vox_db_rollback(vox_db_conn_t* conn) {
    if (!conn) return -1;
    if (!conn->vtbl || !conn->vtbl->rollback) return -1;

    if (vox_db_conn_try_begin(conn) != 0) return -1;

    int rc = conn->vtbl->rollback(conn);

    vox_db_conn_end(conn);
    return (rc == 0) ? 0 : -1;
}

typedef enum {
    VOX_DB_OP_BEGIN = 2,
    VOX_DB_OP_COMMIT,
    VOX_DB_OP_ROLLBACK
} vox_db_tx_op_t;

typedef struct {
    vox_db_conn_t* conn;
    vox_db_tx_op_t op;
    vox_db_exec_cb cb;
    void* user_data;
} vox_db_tx_req_t;

static void db_tx_task(void* user_data) {
    vox_db_tx_req_t* req = (vox_db_tx_req_t*)user_data;
    vox_db_conn_t* conn = req ? req->conn : NULL;
    if (!req || !conn) return;

    int status = -1;
    int (*tx_func)(vox_db_conn_t*) = NULL;

    switch (req->op) {
        case VOX_DB_OP_BEGIN:
            if (conn->vtbl && conn->vtbl->begin_transaction) {
                tx_func = conn->vtbl->begin_transaction;
            }
            break;
        case VOX_DB_OP_COMMIT:
            if (conn->vtbl && conn->vtbl->commit) {
                tx_func = conn->vtbl->commit;
            }
            break;
        case VOX_DB_OP_ROLLBACK:
            if (conn->vtbl && conn->vtbl->rollback) {
                tx_func = conn->vtbl->rollback;
            }
            break;
        default:
            break;
    }

    if (tx_func && tx_func(conn) == 0) {
        status = 0;
    }

    vox_db_conn_end(conn);
    if (req->cb) {
        if (conn->cb_mode == VOX_DB_CALLBACK_LOOP) {
            vox_db_exec_call_t* call = (vox_db_exec_call_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_exec_call_t));
            if (call) {
                call->conn = conn;
                call->cb = req->cb;
                call->user_data = req->user_data;
                call->status = status;
                call->affected = 0;
                if (vox_loop_queue_work(conn->loop, db_loop_invoke_exec, call) != 0) {
                    vox_mpool_free(conn->mpool, call);
                    req->cb(conn, status, 0, req->user_data);
                }
            } else {
                req->cb(conn, status, 0, req->user_data);
            }
        } else {
            req->cb(conn, status, 0, req->user_data);
        }
    }
    vox_mpool_free(conn->mpool, req);
}

int vox_db_begin_transaction_async(vox_db_conn_t* conn, vox_db_exec_cb cb, void* user_data) {
    if (!conn) return -1;
    if (!conn->vtbl || !conn->vtbl->begin_transaction) return -1;

    if (vox_db_conn_try_begin(conn) != 0) return -1;

    vox_db_tx_req_t* req = (vox_db_tx_req_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_tx_req_t));
    if (!req) {
        vox_db_conn_end(conn);
        return -1;
    }
    memset(req, 0, sizeof(*req));
    req->conn = conn;
    req->op = VOX_DB_OP_BEGIN;
    req->cb = cb;
    req->user_data = user_data;

    vox_tpool_t* tpool = vox_loop_get_thread_pool(conn->loop);
    if (!tpool) {
        vox_mpool_free(conn->mpool, req);
        vox_db_conn_end(conn);
        return -1;
    }

    if (vox_tpool_submit(tpool, db_tx_task, req, NULL) != 0) {
        vox_mpool_free(conn->mpool, req);
        vox_db_conn_end(conn);
        return -1;
    }

    return 0;
}

int vox_db_commit_async(vox_db_conn_t* conn, vox_db_exec_cb cb, void* user_data) {
    if (!conn) return -1;
    if (!conn->vtbl || !conn->vtbl->commit) return -1;

    if (vox_db_conn_try_begin(conn) != 0) return -1;

    vox_db_tx_req_t* req = (vox_db_tx_req_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_tx_req_t));
    if (!req) {
        vox_db_conn_end(conn);
        return -1;
    }
    memset(req, 0, sizeof(*req));
    req->conn = conn;
    req->op = VOX_DB_OP_COMMIT;
    req->cb = cb;
    req->user_data = user_data;

    vox_tpool_t* tpool = vox_loop_get_thread_pool(conn->loop);
    if (!tpool) {
        vox_mpool_free(conn->mpool, req);
        vox_db_conn_end(conn);
        return -1;
    }

    if (vox_tpool_submit(tpool, db_tx_task, req, NULL) != 0) {
        vox_mpool_free(conn->mpool, req);
        vox_db_conn_end(conn);
        return -1;
    }

    return 0;
}

int vox_db_rollback_async(vox_db_conn_t* conn, vox_db_exec_cb cb, void* user_data) {
    if (!conn) return -1;
    if (!conn->vtbl || !conn->vtbl->rollback) return -1;

    if (vox_db_conn_try_begin(conn) != 0) return -1;

    vox_db_tx_req_t* req = (vox_db_tx_req_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_tx_req_t));
    if (!req) {
        vox_db_conn_end(conn);
        return -1;
    }
    memset(req, 0, sizeof(*req));
    req->conn = conn;
    req->op = VOX_DB_OP_ROLLBACK;
    req->cb = cb;
    req->user_data = user_data;

    vox_tpool_t* tpool = vox_loop_get_thread_pool(conn->loop);
    if (!tpool) {
        vox_mpool_free(conn->mpool, req);
        vox_db_conn_end(conn);
        return -1;
    }

    if (vox_tpool_submit(tpool, db_tx_task, req, NULL) != 0) {
        vox_mpool_free(conn->mpool, req);
        vox_db_conn_end(conn);
        return -1;
    }

    return 0;
}

