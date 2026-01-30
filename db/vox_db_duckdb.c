/*
 * vox_db_duckdb.c - DuckDB driver
 *
 * conninfo:
 * - ":memory:" 使用内存数据库
 * - 其他值：作为数据库文件路径
 */

#include "vox_db_internal.h"

#ifdef VOX_USE_DUCKDB

#include "../vox_log.h"

#include <duckdb.h>
#include <string.h>

#define VOX_DUCKDB_LAST_ERROR_LEN 512

typedef struct {
    duckdb_database db;
    duckdb_connection conn;
    duckdb_state last_state;
    char last_error_buf[VOX_DUCKDB_LAST_ERROR_LEN]; /* 持久化错误信息，避免 result 销毁后悬垂指针 */
    const char* last_error; /* 指向 last_error_buf 或 NULL */
} vox_db_duckdb_native_t;

/* 将 result 的错误信息复制到 n 的缓冲区，再销毁 result 可安全使用 last_error */
static void duckdb_copy_result_error(vox_db_duckdb_native_t* n, duckdb_result* result) {
    const char* err = duckdb_result_error(result);
    if (err && n) {
        size_t max = sizeof(n->last_error_buf) - 1;
        size_t len = strlen(err);
        if (len > max) len = max;
        memcpy(n->last_error_buf, err, len);
        n->last_error_buf[len] = '\0';
        n->last_error = n->last_error_buf;
    } else if (n) {
        n->last_error = NULL;
    }
}

static vox_db_duckdb_native_t* get_native(vox_db_conn_t* c) {
    return c ? (vox_db_duckdb_native_t*)c->native : NULL;
}

static const char* db_duckdb_last_error(vox_db_conn_t* conn) {
    vox_db_duckdb_native_t* n = get_native(conn);
    return n ? n->last_error : NULL;
}

static int db_duckdb_connect(vox_db_conn_t* conn, const char* conninfo) {
    if (!conn || !conninfo) return -1;

    vox_db_duckdb_native_t* n = (vox_db_duckdb_native_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_duckdb_native_t));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));

    const char* path = conninfo;
    if (strcmp(conninfo, ":memory:") == 0) {
        path = NULL;
    }

    if (duckdb_open(path, &n->db) != DuckDBSuccess) {
        vox_mpool_free(conn->mpool, n);
        return -1;
    }
    if (duckdb_connect(n->db, &n->conn) != DuckDBSuccess) {
        duckdb_close(&n->db);
        vox_mpool_free(conn->mpool, n);
        return -1;
    }

    n->last_state = DuckDBSuccess;
    n->last_error = NULL;
    conn->native = n;
    return 0;
}

static void db_duckdb_disconnect(vox_db_conn_t* conn) {
    vox_db_duckdb_native_t* n = get_native(conn);
    if (!n) return;
    duckdb_disconnect(&n->conn);
    duckdb_close(&n->db);
    vox_mpool_free(conn->mpool, n);
    conn->native = NULL;
}

static int bind_params(duckdb_prepared_statement stmt, const vox_db_value_t* params, size_t nparams) {
    if (!stmt) return -1;
    for (size_t i = 0; i < nparams; i++) {
        const vox_db_value_t* v = &params[i];
        duckdb_state st = DuckDBError;
        idx_t idx = (idx_t)i + 1; /* DuckDB 参数从 1 开始 */
        switch (v->type) {
            case VOX_DB_TYPE_NULL:
                st = duckdb_bind_null(stmt, idx);
                break;
            case VOX_DB_TYPE_BOOL:
                st = duckdb_bind_boolean(stmt, idx, v->u.boolean);
                break;
            case VOX_DB_TYPE_I64:
                st = duckdb_bind_int64(stmt, idx, (int64_t)v->u.i64);
                break;
            case VOX_DB_TYPE_U64:
                st = duckdb_bind_uint64(stmt, idx, (uint64_t)v->u.u64);
                break;
            case VOX_DB_TYPE_F64:
                st = duckdb_bind_double(stmt, idx, v->u.f64);
                break;
            case VOX_DB_TYPE_TEXT: {
                const char* p = v->u.text.ptr ? v->u.text.ptr : "";
                /* duckdb_bind_varchar 需要 NUL 结尾；这里走拷贝版本 */
                st = duckdb_bind_varchar_length(stmt, idx, p, (idx_t)v->u.text.len);
                break;
            }
            case VOX_DB_TYPE_BLOB:
                st = duckdb_bind_blob(stmt, idx, v->u.blob.data, (idx_t)v->u.blob.len);
                break;
            default:
                return -1;
        }
        if (st != DuckDBSuccess) return -1;
    }
    return 0;
}

static int db_duckdb_exec(vox_db_conn_t* conn,
                       const char* sql,
                       const vox_db_value_t* params,
                       size_t nparams,
                       int64_t* out_affected_rows) {
    vox_db_duckdb_native_t* n = get_native(conn);
    if (!n || !sql) return -1;

    duckdb_prepared_statement stmt = NULL;
    if (duckdb_prepare(n->conn, sql, &stmt) != DuckDBSuccess) {
        return -1;
    }

    if (params && nparams > 0) {
        if (bind_params(stmt, params, nparams) != 0) {
            duckdb_destroy_prepare(&stmt);
            return -1;
        }
    }

    duckdb_result result;
    memset(&result, 0, sizeof(result));
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_prepare(&stmt);

    n->last_state = st;
    if (st != DuckDBSuccess) {
        duckdb_copy_result_error(n, &result);
        duckdb_destroy_result(&result);
        return -1;
    }
    n->last_error = NULL;

    if (out_affected_rows) {
        /* DuckDB C API 没有直接获取变更行数的函数
         * 尝试从result中获取：对于INSERT/UPDATE/DELETE，如果result有数据，使用row_count
         * 否则返回0（表示无法确定） */
        idx_t row_count = duckdb_row_count(&result);
        idx_t col_count = duckdb_column_count(&result);
        /* 如果result有列，说明是SELECT查询，affected_rows应该为0
         * 如果是DML操作，通常result为空或只有元数据 */
        if (col_count == 0 && row_count == 0) {
            /* 可能是DML操作，但无法确定具体行数，返回0 */
            *out_affected_rows = 0;
        } else {
            /* 有结果集，可能是SELECT，affected_rows为0 */
            *out_affected_rows = 0;
        }
    }

    duckdb_destroy_result(&result);
    return 0;
}

static vox_db_type_t map_duckdb_type(duckdb_type t) {
    switch (t) {
        case DUCKDB_TYPE_BOOLEAN: return VOX_DB_TYPE_BOOL;
        case DUCKDB_TYPE_TINYINT:
        case DUCKDB_TYPE_SMALLINT:
        case DUCKDB_TYPE_INTEGER:
        case DUCKDB_TYPE_BIGINT: return VOX_DB_TYPE_I64;
        case DUCKDB_TYPE_UTINYINT:
        case DUCKDB_TYPE_USMALLINT:
        case DUCKDB_TYPE_UINTEGER:
        case DUCKDB_TYPE_UBIGINT: return VOX_DB_TYPE_U64;
        case DUCKDB_TYPE_FLOAT:
        case DUCKDB_TYPE_DOUBLE: return VOX_DB_TYPE_F64;
        case DUCKDB_TYPE_BLOB: return VOX_DB_TYPE_BLOB;
        default: return VOX_DB_TYPE_TEXT;
    }
}

static int db_duckdb_query(vox_db_conn_t* conn,
                        const char* sql,
                        const vox_db_value_t* params,
                        size_t nparams,
                        vox_db_row_cb row_cb,
                        void* row_user_data,
                        int64_t* out_row_count) {
    vox_db_duckdb_native_t* n = get_native(conn);
    if (!n || !sql) return -1;

    duckdb_prepared_statement stmt = NULL;
    if (duckdb_prepare(n->conn, sql, &stmt) != DuckDBSuccess) {
        return -1;
    }
    if (params && nparams > 0) {
        if (bind_params(stmt, params, nparams) != 0) {
            duckdb_destroy_prepare(&stmt);
            return -1;
        }
    }

    duckdb_result result;
    memset(&result, 0, sizeof(result));
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_prepare(&stmt);

    n->last_state = st;
    if (st != DuckDBSuccess) {
        duckdb_copy_result_error(n, &result);
        duckdb_destroy_result(&result);
        return -1;
    }
    n->last_error = NULL;

    idx_t cols = duckdb_column_count(&result);
    idx_t rows = duckdb_row_count(&result);

    const char** col_names = NULL;
    vox_db_value_t* values = NULL;
    if (cols > 0) {
        col_names = (const char**)vox_mpool_alloc(conn->mpool, (size_t)cols * sizeof(char*));
        values = (vox_db_value_t*)vox_mpool_alloc(conn->mpool, (size_t)cols * sizeof(vox_db_value_t));
        if (!col_names || !values) {
            if (col_names) vox_mpool_free(conn->mpool, col_names);
            if (values) vox_mpool_free(conn->mpool, values);
            duckdb_destroy_result(&result);
            return -1;
        }
        for (idx_t c = 0; c < cols; c++) {
            col_names[c] = duckdb_column_name(&result, c);
        }
    }

    int64_t row_count = 0;
    for (idx_t r = 0; r < rows; r++) {
        /* 每行可能产生临时分配（varchar/blob），回调返回后释放 */
        for (idx_t c = 0; c < cols; c++) {
            if (duckdb_value_is_null(&result, c, r)) {
                values[c].type = VOX_DB_TYPE_NULL;
                continue;
            }

            duckdb_type t = duckdb_column_type(&result, c);
            vox_db_type_t vt = map_duckdb_type(t);
            values[c].type = vt;

            switch (vt) {
                case VOX_DB_TYPE_BOOL:
                    values[c].u.boolean = duckdb_value_boolean(&result, c, r);
                    break;
                case VOX_DB_TYPE_I64:
                    values[c].u.i64 = duckdb_value_int64(&result, c, r);
                    break;
                case VOX_DB_TYPE_U64:
                    values[c].u.u64 = duckdb_value_uint64(&result, c, r);
                    break;
                case VOX_DB_TYPE_F64:
                    values[c].u.f64 = duckdb_value_double(&result, c, r);
                    break;
                case VOX_DB_TYPE_BLOB: {
                    duckdb_blob b = duckdb_value_blob(&result, c, r);
                    values[c].u.blob.data = b.data;
                    values[c].u.blob.len = (size_t)b.size;
                    /* b.data 需要 duckdb_free */
                    break;
                }
                case VOX_DB_TYPE_TEXT:
                default: {
                    char* s = duckdb_value_varchar(&result, c, r);
                    values[c].u.text.ptr = s;
                    values[c].u.text.len = s ? strlen(s) : 0;
                    /* s 需要 duckdb_free */
                    break;
                }
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

        /* 释放 per-row 临时内存（varchar/blob） */
        for (idx_t c = 0; c < cols; c++) {
            if (values[c].type == VOX_DB_TYPE_TEXT) {
                if (values[c].u.text.ptr) duckdb_free((void*)values[c].u.text.ptr);
                values[c].u.text.ptr = NULL;
                values[c].u.text.len = 0;
            } else if (values[c].type == VOX_DB_TYPE_BLOB) {
                if (values[c].u.blob.data) duckdb_free((void*)values[c].u.blob.data);
                values[c].u.blob.data = NULL;
                values[c].u.blob.len = 0;
            }
        }

        row_count++;
    }

    if (out_row_count) *out_row_count = row_count;

    if (col_names) vox_mpool_free(conn->mpool, col_names);
    if (values) vox_mpool_free(conn->mpool, values);
    duckdb_destroy_result(&result);
    return 0;
}

static int db_duckdb_ping(vox_db_conn_t* conn) {
    /* DuckDB 是文件数据库，只要连接对象存在就认为连接有效 */
    if (!conn || !conn->native) return -1;
    return 0;
}

static int db_duckdb_begin_transaction(vox_db_conn_t* conn) {
    vox_db_duckdb_native_t* n = get_native(conn);
    if (!n || !n->conn) return -1;
    
    duckdb_result result;
    duckdb_state state = duckdb_query(n->conn, "BEGIN TRANSACTION", &result);
    if (state != DuckDBSuccess) {
        n->last_state = state;
        duckdb_copy_result_error(n, &result);
        duckdb_destroy_result(&result);
        return -1;
    }
    n->last_error = NULL;
    duckdb_destroy_result(&result);
    return 0;
}

static int db_duckdb_commit(vox_db_conn_t* conn) {
    vox_db_duckdb_native_t* n = get_native(conn);
    if (!n || !n->conn) return -1;
    
    duckdb_result result;
    duckdb_state state = duckdb_query(n->conn, "COMMIT", &result);
    if (state != DuckDBSuccess) {
        n->last_state = state;
        duckdb_copy_result_error(n, &result);
        duckdb_destroy_result(&result);
        return -1;
    }
    n->last_error = NULL;
    duckdb_destroy_result(&result);
    return 0;
}

static int db_duckdb_rollback(vox_db_conn_t* conn) {
    vox_db_duckdb_native_t* n = get_native(conn);
    if (!n || !n->conn) return -1;
    
    duckdb_result result;
    duckdb_state state = duckdb_query(n->conn, "ROLLBACK", &result);
    if (state != DuckDBSuccess) {
        n->last_state = state;
        duckdb_copy_result_error(n, &result);
        duckdb_destroy_result(&result);
        return -1;
    }
    n->last_error = NULL;
    duckdb_destroy_result(&result);
    return 0;
}

static const vox_db_driver_vtbl_t g_duckdb_vtbl = {
    .name = "duckdb",
    .use_loop_thread_for_async = 1,  /* DuckDB 建议每线程独立连接，跨线程使用同一连接可能崩溃 */
    .connect = db_duckdb_connect,
    .disconnect = db_duckdb_disconnect,
    .ping = db_duckdb_ping,
    .exec = db_duckdb_exec,
    .query = db_duckdb_query,
    .begin_transaction = db_duckdb_begin_transaction,
    .commit = db_duckdb_commit,
    .rollback = db_duckdb_rollback,
    .last_error = db_duckdb_last_error
};

const vox_db_driver_vtbl_t* vox_db_duckdb_vtbl(void) {
    return &g_duckdb_vtbl;
}

#endif /* VOX_USE_DUCKDB */

