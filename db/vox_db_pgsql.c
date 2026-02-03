/*
 * vox_db_pgsql.c - PostgreSQL (libpq) driver
 *
 * conninfo: 原生 libpq conninfo（例如 "host=127.0.0.1 port=5432 user=... dbname=... password=..."）
 *
 * 参数绑定策略：
 * - text param：NULL/BOOL/I64/U64/F64/TEXT
 * - binary param：BLOB（使用 bytea 二进制格式，OID=17）
 *
 * 结果映射策略：
 * - 默认全部以 TEXT 形式返回（零拷贝 view 指向 libpq result 缓冲区，row_cb 期间有效）
 */

#include "vox_db_internal.h"

#ifdef VOX_USE_PGSQL

#include "../vox_log.h"
#include "../vox_os.h"

#include <libpq-fe.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    PGconn* conn;
    char last_error[256];  /* 存储错误信息的缓冲区 */
} vox_db_pgsql_native_t;

static vox_db_pgsql_native_t* get_native(vox_db_conn_t* c) {
    return c ? (vox_db_pgsql_native_t*)c->native : NULL;
}

static const char* db_pgsql_last_error(vox_db_conn_t* conn) {
    vox_db_pgsql_native_t* n = get_native(conn);
    if (!n) return NULL;
    /* 如果设置了错误信息，优先返回它 */
    if (n->last_error[0]) {
        return n->last_error;
    }
    /* 否则从连接获取最新的错误信息 */
    if (n->conn) {
        const char* err = PQerrorMessage(n->conn);
        if (err && err[0]) {
            return err;
        }
    }
    return NULL;
}

static int db_pgsql_connect(vox_db_conn_t* conn, const char* conninfo) {
    if (!conn || !conninfo) return -1;
    vox_db_pgsql_native_t* n = (vox_db_pgsql_native_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_pgsql_native_t));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));

    n->conn = PQconnectdb(conninfo);
    if (!n->conn || PQstatus(n->conn) != CONNECTION_OK) {
        const char* err = n->conn ? PQerrorMessage(n->conn) : "PQconnectdb failed";
        strncpy(n->last_error, err, sizeof(n->last_error) - 1);
        n->last_error[sizeof(n->last_error) - 1] = '\0';
        if (n->conn) {
            PQfinish(n->conn);
            n->conn = NULL;
        }
        /* 先挂到 conn 上，便于调用方 last_error(conn) 读到错误；由 disconnect 负责释放 n */
        conn->native = n;
        return -1;
    }
    n->last_error[0] = '\0';  /* 初始化错误信息为空 */

    conn->native = n;
    return 0;
}

static void db_pgsql_disconnect(vox_db_conn_t* conn) {
    vox_db_pgsql_native_t* n = get_native(conn);
    conn->native = NULL;  /* 先清空，避免重复 disconnect 时再次 PQfinish */
    if (!n) return;
    if (n->conn) {
        PGconn* pg = n->conn;
        n->conn = NULL;
        PQfinish(pg);
    }
    vox_mpool_free(conn->mpool, n);
}

static int db_pgsql_ping(vox_db_conn_t* conn) {
    vox_db_pgsql_native_t* n = get_native(conn);
    if (!n || !n->conn) return -1;
    
    /* 检查连接状态，CONNECTION_OK表示正常，其他值表示异常 */
    ConnStatusType status = PQstatus(n->conn);
    if (status == CONNECTION_OK) return 0;
    return -1;
}

/* 构建参数：返回格式类型（0=text, 1=binary）和OID（用于binary格式） */
static int build_param(vox_mpool_t* mpool, const vox_db_value_t* v, const char** out_ptr, int* out_len, int* out_format, Oid* out_oid) {
    if (!mpool || !v || !out_ptr || !out_len || !out_format || !out_oid) return -1;
    *out_ptr = NULL;
    *out_len = 0;
    *out_format = 0; /* text */
    *out_oid = 0;

    switch (v->type) {
        case VOX_DB_TYPE_NULL:
            *out_ptr = NULL;
            *out_len = 0;
            return 0;
        case VOX_DB_TYPE_TEXT:
            *out_ptr = v->u.text.ptr;
            *out_len = (int)v->u.text.len;
            *out_format = 0; /* text */
            return 0;
        case VOX_DB_TYPE_BOOL: {
            const char* s = v->u.boolean ? "true" : "false";
            *out_ptr = s;
            *out_len = (int)strlen(s);
            *out_format = 0; /* text */
            return 0;
        }
        case VOX_DB_TYPE_I64: {
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "%lld", (long long)v->u.i64);
            if (n < 0) return -1;
            char* p = (char*)vox_mpool_alloc(mpool, (size_t)n + 1);
            if (!p) return -1;
            memcpy(p, buf, (size_t)n + 1);
            *out_ptr = p;
            *out_len = n;
            *out_format = 0; /* text */
            return 0;
        }
        case VOX_DB_TYPE_U64: {
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v->u.u64);
            if (n < 0) return -1;
            char* p = (char*)vox_mpool_alloc(mpool, (size_t)n + 1);
            if (!p) return -1;
            memcpy(p, buf, (size_t)n + 1);
            *out_ptr = p;
            *out_len = n;
            *out_format = 0; /* text */
            return 0;
        }
        case VOX_DB_TYPE_F64: {
            char buf[128];
            int n = snprintf(buf, sizeof(buf), "%.17g", v->u.f64);
            if (n < 0) return -1;
            char* p = (char*)vox_mpool_alloc(mpool, (size_t)n + 1);
            if (!p) return -1;
            memcpy(p, buf, (size_t)n + 1);
            *out_ptr = p;
            *out_len = n;
            *out_format = 0; /* text */
            return 0;
        }
        case VOX_DB_TYPE_BLOB: {
            /* BLOB使用二进制格式（format=1）和bytea类型OID */
            *out_ptr = v->u.blob.data ? (const char*)v->u.blob.data : "";
            *out_len = (int)v->u.blob.len;
            *out_format = 1; /* binary */
            *out_oid = 17; /* bytea OID */
            return 0;
        }
        default:
            return -1;
    }
}

/* 兼容旧接口 */
VOX_UNUSED_FUNC static int build_param_text(vox_mpool_t* mpool, const vox_db_value_t* v, const char** out_ptr, int* out_len) {
    int format;
    Oid oid;
    return build_param(mpool, v, out_ptr, out_len, &format, &oid);
}

static void free_param_text(vox_mpool_t* mpool, const vox_db_value_t* v, const char* ptr) {
    if (!mpool || !v || !ptr) return;
    /* TEXT/BOOL/BLOB 使用常量或外部内存不释放；数值是我们临时分配的 */
    if (v->type == VOX_DB_TYPE_I64 || v->type == VOX_DB_TYPE_U64 || v->type == VOX_DB_TYPE_F64) {
        vox_mpool_free(mpool, (void*)ptr);
    }
    /* BLOB使用外部内存，不需要释放 */
}

static int db_pgsql_exec(vox_db_conn_t* conn,
                      const char* sql,
                      const vox_db_value_t* params,
                      size_t nparams,
                      int64_t* out_affected_rows) {
    vox_db_pgsql_native_t* n = get_native(conn);
    if (!n || !n->conn || !sql) return -1;

    const char** values = NULL;
    int* lengths = NULL;
    int* formats = NULL;
    Oid* param_types = NULL;

    if (params && nparams > 0) {
        values = (const char**)vox_mpool_alloc(conn->mpool, nparams * sizeof(char*));
        lengths = (int*)vox_mpool_alloc(conn->mpool, nparams * sizeof(int));
        formats = (int*)vox_mpool_alloc(conn->mpool, nparams * sizeof(int));
        param_types = (Oid*)vox_mpool_alloc(conn->mpool, nparams * sizeof(Oid));
        if (!values || !lengths || !formats || !param_types) {
            if (values) vox_mpool_free(conn->mpool, values);
            if (lengths) vox_mpool_free(conn->mpool, lengths);
            if (formats) vox_mpool_free(conn->mpool, formats);
            if (param_types) vox_mpool_free(conn->mpool, param_types);
            return -1;
        }

        for (size_t i = 0; i < nparams; i++) {
            if (build_param(conn->mpool, &params[i], &values[i], &lengths[i], &formats[i], &param_types[i]) != 0) {
                for (size_t j = 0; j < i; j++) free_param_text(conn->mpool, &params[j], values[j]);
                vox_mpool_free(conn->mpool, values);
                vox_mpool_free(conn->mpool, lengths);
                vox_mpool_free(conn->mpool, formats);
                vox_mpool_free(conn->mpool, param_types);
                return -1;
            }
        }
    }

    PGresult* res = PQexecParams(n->conn, sql, (int)nparams, param_types, values, lengths, formats, 0);

    if (params && nparams > 0) {
        for (size_t i = 0; i < nparams; i++) free_param_text(conn->mpool, &params[i], values[i]);
        vox_mpool_free(conn->mpool, values);
        vox_mpool_free(conn->mpool, lengths);
        vox_mpool_free(conn->mpool, formats);
        vox_mpool_free(conn->mpool, param_types);
    }

    if (!res) {
        const char* err = PQerrorMessage(n->conn);
        strncpy(n->last_error, err ? err : "PQexecParams failed", sizeof(n->last_error) - 1);
        n->last_error[sizeof(n->last_error) - 1] = '\0';
        return -1;
    }

    ExecStatusType st = PQresultStatus(res);
    if (!(st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK)) {
        const char* err = PQresultErrorMessage(res);
        strncpy(n->last_error, err ? err : "query failed", sizeof(n->last_error) - 1);
        n->last_error[sizeof(n->last_error) - 1] = '\0';
        PQclear(res);
        return -1;
    }

    /* 成功时清除错误信息 */
    n->last_error[0] = '\0';

    if (out_affected_rows) {
        const char* s = PQcmdTuples(res);
        if (s && s[0]) {
            *out_affected_rows = (int64_t)atoll(s);
        } else {
            *out_affected_rows = 0;
        }
    }

    PQclear(res);
    return 0;
}

static int db_pgsql_query(vox_db_conn_t* conn,
                       const char* sql,
                       const vox_db_value_t* params,
                       size_t nparams,
                       vox_db_row_cb row_cb,
                       void* row_user_data,
                       int64_t* out_row_count) {
    vox_db_pgsql_native_t* n = get_native(conn);
    if (!n || !n->conn || !sql) return -1;

    const char** values = NULL;
    int* lengths = NULL;
    int* formats = NULL;
    Oid* param_types = NULL;

    if (params && nparams > 0) {
        values = (const char**)vox_mpool_alloc(conn->mpool, nparams * sizeof(char*));
        lengths = (int*)vox_mpool_alloc(conn->mpool, nparams * sizeof(int));
        formats = (int*)vox_mpool_alloc(conn->mpool, nparams * sizeof(int));
        param_types = (Oid*)vox_mpool_alloc(conn->mpool, nparams * sizeof(Oid));
        if (!values || !lengths || !formats || !param_types) {
            if (values) vox_mpool_free(conn->mpool, values);
            if (lengths) vox_mpool_free(conn->mpool, lengths);
            if (formats) vox_mpool_free(conn->mpool, formats);
            if (param_types) vox_mpool_free(conn->mpool, param_types);
            return -1;
        }

        for (size_t i = 0; i < nparams; i++) {
            if (build_param(conn->mpool, &params[i], &values[i], &lengths[i], &formats[i], &param_types[i]) != 0) {
                for (size_t j = 0; j < i; j++) free_param_text(conn->mpool, &params[j], values[j]);
                vox_mpool_free(conn->mpool, values);
                vox_mpool_free(conn->mpool, lengths);
                vox_mpool_free(conn->mpool, formats);
                vox_mpool_free(conn->mpool, param_types);
                return -1;
            }
        }
    }

    PGresult* res = PQexecParams(n->conn, sql, (int)nparams, param_types, values, lengths, formats, 0);

    if (params && nparams > 0) {
        for (size_t i = 0; i < nparams; i++) free_param_text(conn->mpool, &params[i], values[i]);
        vox_mpool_free(conn->mpool, values);
        vox_mpool_free(conn->mpool, lengths);
        vox_mpool_free(conn->mpool, formats);
        vox_mpool_free(conn->mpool, param_types);
    }

    if (!res) {
        const char* err = PQerrorMessage(n->conn);
        strncpy(n->last_error, err ? err : "PQexecParams failed", sizeof(n->last_error) - 1);
        n->last_error[sizeof(n->last_error) - 1] = '\0';
        return -1;
    }

    ExecStatusType st = PQresultStatus(res);
    /* PGRES_TUPLES_OK 表示成功的查询返回了结果集
     * PGRES_COMMAND_OK 表示成功执行但没有结果集的命令（如 INSERT/UPDATE/DELETE）
     * 对于 query 操作，两者都应该视为成功，但 PGRES_COMMAND_OK 不应该返回行 */
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        const char* err = PQresultErrorMessage(res);
        if (err && err[0]) {
            strncpy(n->last_error, err, sizeof(n->last_error) - 1);
        } else {
            const char* status_str = PQresStatus(st);
            snprintf(n->last_error, sizeof(n->last_error), "query failed with status: %s", status_str ? status_str : "unknown");
        }
        n->last_error[sizeof(n->last_error) - 1] = '\0';
        PQclear(res);
        return -1;
    }
    
    /* 如果是 PGRES_COMMAND_OK，表示没有结果集返回（例如执行了 INSERT 而不是 SELECT） */
    if (st == PGRES_COMMAND_OK) {
        if (out_row_count) *out_row_count = 0;
        n->last_error[0] = '\0';
        PQclear(res);
        return 0;
    }

    /* 成功时清除错误信息 */
    n->last_error[0] = '\0';

    int cols = PQnfields(res);
    int rows = PQntuples(res);

    const char** col_names = NULL;
    vox_db_value_t* row_vals = NULL;
    if (cols > 0) {
        col_names = (const char**)vox_mpool_alloc(conn->mpool, (size_t)cols * sizeof(char*));
        row_vals = (vox_db_value_t*)vox_mpool_alloc(conn->mpool, (size_t)cols * sizeof(vox_db_value_t));
        if (!col_names || !row_vals) {
            if (col_names) vox_mpool_free(conn->mpool, col_names);
            if (row_vals) vox_mpool_free(conn->mpool, row_vals);
            snprintf(n->last_error, sizeof(n->last_error), "query: out of memory for %d columns", cols);
            PQclear(res);
            return -1;
        }
        for (int c = 0; c < cols; c++) {
            col_names[c] = PQfname(res, c);
        }
    }

    int64_t row_count = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (PQgetisnull(res, r, c)) {
                row_vals[c].type = VOX_DB_TYPE_NULL;
                continue;
            }
            char* v = PQgetvalue(res, r, c);
            int len = PQgetlength(res, r, c);
            row_vals[c].type = VOX_DB_TYPE_TEXT;
            row_vals[c].u.text.ptr = v;
            row_vals[c].u.text.len = (size_t)((len > 0) ? len : 0);
        }

        if (row_cb) {
            vox_db_row_t row = {
                .column_count = (size_t)cols,
                .column_names = (const char* const*)col_names,
                .values = row_vals
            };
            row_cb(conn, &row, row_user_data);
        }
        row_count++;
    }

    if (out_row_count) *out_row_count = row_count;

    if (col_names) vox_mpool_free(conn->mpool, col_names);
    if (row_vals) vox_mpool_free(conn->mpool, row_vals);
    PQclear(res);
    return 0;
}

static int db_pgsql_begin_transaction(vox_db_conn_t* conn) {
    vox_db_pgsql_native_t* n = get_native(conn);
    if (!n || !n->conn) return -1;
    
    PGresult* res = PQexec(n->conn, "BEGIN");
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return -1;
    }
    PQclear(res);
    return 0;
}

static int db_pgsql_commit(vox_db_conn_t* conn) {
    vox_db_pgsql_native_t* n = get_native(conn);
    if (!n || !n->conn) return -1;
    
    PGresult* res = PQexec(n->conn, "COMMIT");
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return -1;
    }
    PQclear(res);
    return 0;
}

static int db_pgsql_rollback(vox_db_conn_t* conn) {
    vox_db_pgsql_native_t* n = get_native(conn);
    if (!n || !n->conn) return -1;
    
    PGresult* res = PQexec(n->conn, "ROLLBACK");
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return -1;
    }
    PQclear(res);
    return 0;
}

static const vox_db_driver_vtbl_t g_pgsql_vtbl = {
    .name = "pgsql",
    .use_loop_thread_for_async = 1,  /* libpq/SSL 同一连接不宜跨线程使用，异步 exec/query 在 loop 线程执行 */
    .connect = db_pgsql_connect,
    .disconnect = db_pgsql_disconnect,
    .ping = db_pgsql_ping,
    .exec = db_pgsql_exec,
    .query = db_pgsql_query,
    .begin_transaction = db_pgsql_begin_transaction,
    .commit = db_pgsql_commit,
    .rollback = db_pgsql_rollback,
    .last_error = db_pgsql_last_error
};

const vox_db_driver_vtbl_t* vox_db_pgsql_vtbl(void) {
    return &g_pgsql_vtbl;
}

#endif /* VOX_USE_PGSQL */

