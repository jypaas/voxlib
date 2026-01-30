/*
 * vox_db_sqlite3.c - SQLite3 driver
 */

#include "vox_db_internal.h"

#ifdef VOX_USE_SQLITE3

#include "../vox_log.h"

#include <sqlite3.h>
#include <string.h>

typedef struct {
    sqlite3* db;
} vox_db_sqlite3_native_t;

static vox_db_sqlite3_native_t* get_native(vox_db_conn_t* conn) {
    return conn ? (vox_db_sqlite3_native_t*)conn->native : NULL;
}

static const char* db_sqlite3_last_error(vox_db_conn_t* conn) {
    vox_db_sqlite3_native_t* n = get_native(conn);
    if (!n || !n->db) return NULL;
    return sqlite3_errmsg(n->db);
}

static int bind_params(sqlite3_stmt* stmt, const vox_db_value_t* params, size_t nparams) {
    if (!stmt) return -1;
    for (size_t i = 0; i < nparams; i++) {
        const vox_db_value_t* v = &params[i];
        int idx = (int)i + 1; /* sqlite 参数从 1 开始 */
        int rc = SQLITE_OK;
        switch (v->type) {
            case VOX_DB_TYPE_NULL:
                rc = sqlite3_bind_null(stmt, idx);
                break;
            case VOX_DB_TYPE_BOOL:
                rc = sqlite3_bind_int(stmt, idx, v->u.boolean ? 1 : 0);
                break;
            case VOX_DB_TYPE_I64:
                rc = sqlite3_bind_int64(stmt, idx, (sqlite3_int64)v->u.i64);
                break;
            case VOX_DB_TYPE_U64:
                rc = sqlite3_bind_int64(stmt, idx, (sqlite3_int64)v->u.u64);
                break;
            case VOX_DB_TYPE_F64:
                rc = sqlite3_bind_double(stmt, idx, v->u.f64);
                break;
            case VOX_DB_TYPE_TEXT: {
                const char* p = v->u.text.ptr ? v->u.text.ptr : "";
                int len = (int)v->u.text.len;
                rc = sqlite3_bind_text(stmt, idx, p, len, SQLITE_TRANSIENT);
                break;
            }
            case VOX_DB_TYPE_BLOB:
                rc = sqlite3_bind_blob(stmt, idx, v->u.blob.data, (int)v->u.blob.len, SQLITE_TRANSIENT);
                break;
            default:
                return -1;
        }
        if (rc != SQLITE_OK) return -1;
    }
    return 0;
}

static int db_sqlite3_connect(vox_db_conn_t* conn, const char* conninfo) {
    if (!conn || !conninfo) return -1;

    vox_db_sqlite3_native_t* n = (vox_db_sqlite3_native_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_sqlite3_native_t));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));

    /* conninfo: sqlite 文件路径。":memory:" 可用。 */
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    /* 仅当显式使用 file: URI 时开启 URI 解析，避免误伤 Windows 路径 */
    if (strncmp(conninfo, "file:", 5) == 0) {
        flags |= SQLITE_OPEN_URI;
    }

    if (sqlite3_open_v2(conninfo, &n->db, flags, NULL) != SQLITE_OK) {
        if (n->db) {
            VOX_LOG_ERROR("[db/sqlite3] open failed: %s", sqlite3_errmsg(n->db));
            sqlite3_close(n->db);
        }
        vox_mpool_free(conn->mpool, n);
        return -1;
    }

    /* 更适合并发读取的默认参数 */
    (void)sqlite3_busy_timeout(n->db, 5000);
    /* in-memory/共享内存模式不适合强制 WAL，跳过以避免奇怪行为
     * 检查：:memory: 或包含 mode=memory 或 mode=temp 的连接字符串 */
    int skip_wal = 0;
    if (strcmp(conninfo, ":memory:") == 0) {
        skip_wal = 1;
    } else {
        const char* mem_mode = strstr(conninfo, "mode=memory");
        const char* temp_mode = strstr(conninfo, "mode=temp");
        if (mem_mode != NULL || temp_mode != NULL) {
            skip_wal = 1;
        }
    }
    if (!skip_wal) {
        (void)sqlite3_exec(n->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    }

    conn->native = n;
    return 0;
}

static void db_sqlite3_disconnect(vox_db_conn_t* conn) {
    vox_db_sqlite3_native_t* n = get_native(conn);
    if (!n) return;
    if (n->db) {
        sqlite3_close(n->db);
        n->db = NULL;
    }
    vox_mpool_free(conn->mpool, n);
    conn->native = NULL;
}

static int db_sqlite3_exec(vox_db_conn_t* conn,
                        const char* sql,
                        const vox_db_value_t* params,
                        size_t nparams,
                        int64_t* out_affected_rows) {
    vox_db_sqlite3_native_t* n = get_native(conn);
    if (!n || !n->db || !sql) return -1;

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(n->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    if (params && nparams > 0) {
        if (bind_params(stmt, params, nparams) != 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }

    if (out_affected_rows) {
        *out_affected_rows = (int64_t)sqlite3_changes(n->db);
    }

    sqlite3_finalize(stmt);
    return 0;
}

static vox_db_type_t map_sqlite_col_type(int t) {
    switch (t) {
        case SQLITE_INTEGER: return VOX_DB_TYPE_I64;
        case SQLITE_FLOAT: return VOX_DB_TYPE_F64;
        case SQLITE_TEXT: return VOX_DB_TYPE_TEXT;
        case SQLITE_BLOB: return VOX_DB_TYPE_BLOB;
        case SQLITE_NULL: default: return VOX_DB_TYPE_NULL;
    }
}

static int db_sqlite3_query(vox_db_conn_t* conn,
                         const char* sql,
                         const vox_db_value_t* params,
                         size_t nparams,
                         vox_db_row_cb row_cb,
                         void* row_user_data,
                         int64_t* out_row_count) {
    vox_db_sqlite3_native_t* n = get_native(conn);
    if (!n || !n->db || !sql) return -1;

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(n->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        /* 记录错误信息（如果可能） */
        const char* err_msg = sqlite3_errmsg(n->db);
        if (err_msg) {
            VOX_LOG_ERROR("[db/sqlite3] prepare failed: %s (code=%d)", err_msg, rc);
        }
        return -1;
    }

    if (params && nparams > 0) {
        if (bind_params(stmt, params, nparams) != 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }

    int cols = sqlite3_column_count(stmt);
    if (cols < 0) cols = 0;

    /* 列名指针数组：SQLite 返回的名字指针在 stmt 有效期间可用 */
    const char** col_names = NULL;
    if (cols > 0) {
        col_names = (const char**)vox_mpool_alloc(conn->mpool, (size_t)cols * sizeof(char*));
        if (!col_names) {
            sqlite3_finalize(stmt);
            return -1;
        }
        for (int i = 0; i < cols; i++) {
            col_names[i] = sqlite3_column_name(stmt, i);
        }
    }

    vox_db_value_t* values = NULL;
    if (cols > 0) {
        values = (vox_db_value_t*)vox_mpool_alloc(conn->mpool, (size_t)cols * sizeof(vox_db_value_t));
        if (!values) {
            if (col_names) vox_mpool_free(conn->mpool, col_names);
            sqlite3_finalize(stmt);
            return -1;
        }
    }

    int64_t row_count = 0;
    for (;;) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            if (values) vox_mpool_free(conn->mpool, values);
            if (col_names) vox_mpool_free(conn->mpool, col_names);
            sqlite3_finalize(stmt);
            return -1;
        }

        for (int i = 0; i < cols; i++) {
            int ct = sqlite3_column_type(stmt, i);
            vox_db_type_t vt = map_sqlite_col_type(ct);
            values[i].type = vt;
            switch (vt) {
                case VOX_DB_TYPE_I64:
                    values[i].u.i64 = (int64_t)sqlite3_column_int64(stmt, i);
                    break;
                case VOX_DB_TYPE_F64:
                    values[i].u.f64 = sqlite3_column_double(stmt, i);
                    break;
                case VOX_DB_TYPE_TEXT: {
                    const unsigned char* p = sqlite3_column_text(stmt, i);
                    int len = sqlite3_column_bytes(stmt, i);
                    values[i].u.text.ptr = (const char*)p;
                    values[i].u.text.len = (size_t)((len > 0) ? len : 0);
                    break;
                }
                case VOX_DB_TYPE_BLOB: {
                    const void* p = sqlite3_column_blob(stmt, i);
                    int len = sqlite3_column_bytes(stmt, i);
                    values[i].u.blob.data = p;
                    values[i].u.blob.len = (size_t)((len > 0) ? len : 0);
                    break;
                }
                case VOX_DB_TYPE_NULL:
                default:
                    /* nothing */
                    break;
            }
        }

        if (row_cb) {
            vox_db_row_t row = {
                .column_count = (size_t)cols,
                .column_names = (const char* const*)col_names,
                .values = values
            };
            row_cb(conn, &row, row_user_data);
        }
        row_count++;
    }

    if (out_row_count) *out_row_count = row_count;

    if (values) vox_mpool_free(conn->mpool, values);
    if (col_names) vox_mpool_free(conn->mpool, col_names);
    sqlite3_finalize(stmt);
    return 0;
}

static int db_sqlite3_ping(vox_db_conn_t* conn) {
    /* SQLite3 是文件数据库，只要连接对象存在就认为连接有效 */
    if (!conn || !conn->native) return -1;
    return 0;
}

static int db_sqlite3_begin_transaction(vox_db_conn_t* conn) {
    vox_db_sqlite3_native_t* n = get_native(conn);
    if (!n || !n->db) return -1;
    
    char* errmsg = NULL;
    int rc = sqlite3_exec(n->db, "BEGIN TRANSACTION;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg) {
            VOX_LOG_ERROR("[db/sqlite3] begin transaction failed: %s", errmsg);
            sqlite3_free(errmsg);
        }
        return -1;
    }
    return 0;
}

static int db_sqlite3_commit(vox_db_conn_t* conn) {
    vox_db_sqlite3_native_t* n = get_native(conn);
    if (!n || !n->db) return -1;
    
    char* errmsg = NULL;
    int rc = sqlite3_exec(n->db, "COMMIT;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg) {
            VOX_LOG_ERROR("[db/sqlite3] commit failed: %s", errmsg);
            sqlite3_free(errmsg);
        }
        return -1;
    }
    return 0;
}

static int db_sqlite3_rollback(vox_db_conn_t* conn) {
    vox_db_sqlite3_native_t* n = get_native(conn);
    if (!n || !n->db) return -1;
    
    char* errmsg = NULL;
    int rc = sqlite3_exec(n->db, "ROLLBACK;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg) {
            VOX_LOG_ERROR("[db/sqlite3] rollback failed: %s", errmsg);
            sqlite3_free(errmsg);
        }
        return -1;
    }
    return 0;
}

static const vox_db_driver_vtbl_t g_sqlite3_vtbl = {
    .name = "sqlite3",
    .use_loop_thread_for_async = 1,  /* SQLite 连接需在创建线程使用，避免跨线程崩溃 */
    .connect = db_sqlite3_connect,
    .disconnect = db_sqlite3_disconnect,
    .ping = db_sqlite3_ping,
    .exec = db_sqlite3_exec,
    .query = db_sqlite3_query,
    .begin_transaction = db_sqlite3_begin_transaction,
    .commit = db_sqlite3_commit,
    .rollback = db_sqlite3_rollback,
    .last_error = db_sqlite3_last_error
};

const vox_db_driver_vtbl_t* vox_db_sqlite3_vtbl(void) {
    return &g_sqlite3_vtbl;
}

#endif /* VOX_USE_SQLITE3 */

