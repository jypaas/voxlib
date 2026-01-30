/*
 * vox_db_mysql.c - MySQL (libmysqlclient) driver
 *
 * conninfo: 简化 DSN（示例）：
 *   "host=127.0.0.1;port=3306;user=root;password=xxx;db=test;charset=utf8mb4"
 *
 * 说明：
 * - 当前实现优先保证“可用/可编译”，参数化查询（prepared statement）后续可扩展
 * - 目前：params != NULL 时返回错误（避免拼接 SQL 带来注入风险）
 * - 结果：全部以 TEXT 返回（零拷贝 view 指向 MYSQL_RES 缓冲区，row_cb 期间有效）
 */

#include "vox_db_internal.h"

#ifdef VOX_USE_MYSQL

#include "../vox_log.h"

#include <mysql.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    MYSQL* mysql;
    char last_error[256];
} vox_db_mysql_native_t;

static vox_db_mysql_native_t* get_native(vox_db_conn_t* c) {
    return c ? (vox_db_mysql_native_t*)c->native : NULL;
}

static void set_err(vox_db_mysql_native_t* n, const char* msg) {
    if (!n) return;
    if (!msg) msg = "";
    strncpy(n->last_error, msg, sizeof(n->last_error) - 1);
    n->last_error[sizeof(n->last_error) - 1] = '\0';
}

static const char* db_mysql_last_error(vox_db_conn_t* conn) {
    vox_db_mysql_native_t* n = get_native(conn);
    if (!n) return NULL;
    if (n->mysql) {
        const char* e = mysql_error(n->mysql);
        if (e && e[0]) return e;
    }
    return n->last_error[0] ? n->last_error : NULL;
}

/* 极简 key=value;key=value 解析（只用于示例连接） */
static const char* dsn_get(const char* dsn, const char* key, vox_mpool_t* mpool) {
    VOX_UNUSED(mpool);
    if (!dsn || !key) return NULL;
    size_t klen = strlen(key);
    const char* p = dsn;
    while (*p) {
        /* 跳过分隔符 */
        while (*p == ';' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            return p;
        }
        /* 跳过到下一个分号 */
        while (*p && *p != ';') p++;
    }
    return NULL;
}

static char* dsn_dup_value(vox_mpool_t* mpool, const char* start) {
    if (!mpool || !start) return NULL;
    const char* end = start;
    while (*end && *end != ';') end++;
    size_t len = (size_t)(end - start);
    char* out = (char*)vox_mpool_alloc(mpool, len + 1);
    if (!out) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static int db_mysql_connect(vox_db_conn_t* conn, const char* conninfo) {
    if (!conn || !conninfo) return -1;

    vox_db_mysql_native_t* n = (vox_db_mysql_native_t*)vox_mpool_alloc(conn->mpool, sizeof(vox_db_mysql_native_t));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));

    n->mysql = mysql_init(NULL);
    if (!n->mysql) {
        vox_mpool_free(conn->mpool, n);
        return -1;
    }

    const char* host_s = dsn_get(conninfo, "host", conn->mpool);
    const char* port_s = dsn_get(conninfo, "port", conn->mpool);
    const char* user_s = dsn_get(conninfo, "user", conn->mpool);
    const char* pass_s = dsn_get(conninfo, "password", conn->mpool);
    const char* db_s = dsn_get(conninfo, "db", conn->mpool);
    const char* charset_s = dsn_get(conninfo, "charset", conn->mpool);

    char* host = host_s ? dsn_dup_value(conn->mpool, host_s) : NULL;
    char* user = user_s ? dsn_dup_value(conn->mpool, user_s) : NULL;
    char* pass = pass_s ? dsn_dup_value(conn->mpool, pass_s) : NULL;
    char* db = db_s ? dsn_dup_value(conn->mpool, db_s) : NULL;
    char* charset = charset_s ? dsn_dup_value(conn->mpool, charset_s) : NULL;
    unsigned int port = 0;
    if (port_s) port = (unsigned int)atoi(port_s);

    if (charset && charset[0]) {
        (void)mysql_options(n->mysql, MYSQL_SET_CHARSET_NAME, charset);
    }

    if (!mysql_real_connect(n->mysql,
                            (host && host[0]) ? host : NULL,
                            (user && user[0]) ? user : NULL,
                            (pass && pass[0]) ? pass : NULL,
                            (db && db[0]) ? db : NULL,
                            port,
                            NULL,
                            0)) {
        set_err(n, mysql_error(n->mysql));
        mysql_close(n->mysql);
        n->mysql = NULL;
        /* host/user/pass/db/charset 是 mpool 分配，随连接销毁不再单独释放 */
        vox_mpool_free(conn->mpool, n);
        return -1;
    }

    conn->native = n;
    return 0;
}

static void db_mysql_disconnect(vox_db_conn_t* conn) {
    vox_db_mysql_native_t* n = get_native(conn);
    if (!n) return;
    if (n->mysql) {
        mysql_close(n->mysql);
        n->mysql = NULL;
    }
    vox_mpool_free(conn->mpool, n);
    conn->native = NULL;
}

static int db_mysql_ping(vox_db_conn_t* conn) {
    vox_db_mysql_native_t* n = get_native(conn);
    if (!n || !n->mysql) return -1;
    
    /* mysql_ping() 返回0表示连接正常，非0表示连接断开 */
    return mysql_ping(n->mysql);
}

static int bind_mysql_params(vox_mpool_t* mpool, MYSQL_STMT* stmt, const vox_db_value_t* params, size_t nparams) {
    if (!mpool || !stmt || !params || nparams == 0) return 0;

    MYSQL_BIND* binds = (MYSQL_BIND*)vox_mpool_alloc(mpool, nparams * sizeof(MYSQL_BIND));
    if (!binds) return -1;
    memset(binds, 0, nparams * sizeof(MYSQL_BIND));

    /* 存储需要释放的缓冲区指针 */
    void** temp_buffers = (void**)vox_mpool_alloc(mpool, nparams * sizeof(void*));
    if (!temp_buffers) {
        vox_mpool_free(mpool, binds);
        return -1;
    }
    memset(temp_buffers, 0, nparams * sizeof(void*));

    int rc = 0;
    size_t allocated_count = 0;
    for (size_t i = 0; i < nparams; i++) {
        const vox_db_value_t* v = &params[i];
        MYSQL_BIND* b = &binds[i];

        switch (v->type) {
            case VOX_DB_TYPE_NULL:
                b->buffer_type = MYSQL_TYPE_NULL;
                break;
            case VOX_DB_TYPE_BOOL: {
                my_bool* val = (my_bool*)vox_mpool_alloc(mpool, sizeof(my_bool));
                if (!val) { rc = -1; goto cleanup; }
                *val = v->u.boolean ? 1 : 0;
                b->buffer_type = MYSQL_TYPE_TINY;
                b->buffer = val;
                b->buffer_length = sizeof(my_bool);
                b->is_unsigned = 0;
                temp_buffers[allocated_count++] = val;
                break;
            }
            case VOX_DB_TYPE_I64: {
                int64_t* val = (int64_t*)vox_mpool_alloc(mpool, sizeof(int64_t));
                if (!val) { rc = -1; goto cleanup; }
                *val = v->u.i64;
                b->buffer_type = MYSQL_TYPE_LONGLONG;
                b->buffer = val;
                b->buffer_length = sizeof(int64_t);
                b->is_unsigned = 0;
                temp_buffers[allocated_count++] = val;
                break;
            }
            case VOX_DB_TYPE_U64: {
                uint64_t* val = (uint64_t*)vox_mpool_alloc(mpool, sizeof(uint64_t));
                if (!val) { rc = -1; goto cleanup; }
                *val = v->u.u64;
                b->buffer_type = MYSQL_TYPE_LONGLONG;
                b->buffer = val;
                b->buffer_length = sizeof(uint64_t);
                b->is_unsigned = 1;
                temp_buffers[allocated_count++] = val;
                break;
            }
            case VOX_DB_TYPE_F64: {
                double* val = (double*)vox_mpool_alloc(mpool, sizeof(double));
                if (!val) { rc = -1; goto cleanup; }
                *val = v->u.f64;
                b->buffer_type = MYSQL_TYPE_DOUBLE;
                b->buffer = val;
                b->buffer_length = sizeof(double);
                temp_buffers[allocated_count++] = val;
                break;
            }
            case VOX_DB_TYPE_TEXT: {
                const char* p = v->u.text.ptr ? v->u.text.ptr : "";
                size_t len = v->u.text.len;
                char* buf = (char*)vox_mpool_alloc(mpool, len > 0 ? len : 1);
                if (!buf) { rc = -1; goto cleanup; }
                if (len > 0) memcpy(buf, p, len);
                b->buffer_type = MYSQL_TYPE_STRING;
                b->buffer = buf;
                b->buffer_length = (unsigned long)len;
                b->length_value = (unsigned long)len;
                temp_buffers[allocated_count++] = buf;
                break;
            }
            case VOX_DB_TYPE_BLOB: {
                const void* p = v->u.blob.data ? v->u.blob.data : "";
                size_t len = v->u.blob.len;
                void* buf = vox_mpool_alloc(mpool, len > 0 ? len : 1);
                if (!buf) { rc = -1; goto cleanup; }
                if (len > 0) memcpy(buf, p, len);
                b->buffer_type = MYSQL_TYPE_BLOB;
                b->buffer = buf;
                b->buffer_length = (unsigned long)len;
                b->length_value = (unsigned long)len;
                temp_buffers[allocated_count++] = buf;
                break;
            }
            default:
                rc = -1;
                goto cleanup;
        }
    }

    if (mysql_stmt_bind_param(stmt, binds) != 0) {
        rc = -1;
        goto cleanup;
    }

    /* 绑定成功后立即释放（mysql_stmt_bind_param 会复制数据） */
    for (size_t i = 0; i < allocated_count; i++) {
        if (temp_buffers[i]) vox_mpool_free(mpool, temp_buffers[i]);
    }
    vox_mpool_free(mpool, temp_buffers);
    vox_mpool_free(mpool, binds);
    return 0;

cleanup:
    for (size_t i = 0; i < allocated_count; i++) {
        if (temp_buffers[i]) vox_mpool_free(mpool, temp_buffers[i]);
    }
    vox_mpool_free(mpool, temp_buffers);
    vox_mpool_free(mpool, binds);
    return rc;
}

static int db_mysql_exec(vox_db_conn_t* conn,
                      const char* sql,
                      const vox_db_value_t* params,
                      size_t nparams,
                      int64_t* out_affected_rows) {
    vox_db_mysql_native_t* n = get_native(conn);
    if (!n || !n->mysql || !sql) return -1;

    MYSQL_STMT* stmt = mysql_stmt_init(n->mysql);
    if (!stmt) {
        set_err(n, mysql_error(n->mysql));
        return -1;
    }

    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) {
        set_err(n, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    if (params && nparams > 0) {
        if (bind_mysql_params(conn->mpool, stmt, params, nparams) != 0) {
            set_err(n, "mysql: failed to bind parameters");
            mysql_stmt_close(stmt);
            return -1;
        }
    }

    if (mysql_stmt_execute(stmt) != 0) {
        set_err(n, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    /* 对于 INSERT/UPDATE/DELETE 语句，不应该有结果集
     * 根据MySQL文档，应该直接获取受影响行数，不需要调用结果集相关函数
     * 注意：不要调用 mysql_stmt_result_metadata 或 mysql_stmt_free_result
     * 这些函数可能会触发某些 MySQL/MariaDB 版本在内部尝试存储结果集，导致段错误 */
    my_ulonglong affected = mysql_stmt_affected_rows(stmt);
    if (out_affected_rows) *out_affected_rows = (int64_t)affected;

    mysql_stmt_close(stmt);
    return 0;
}

static int db_mysql_query(vox_db_conn_t* conn,
                       const char* sql,
                       const vox_db_value_t* params,
                       size_t nparams,
                       vox_db_row_cb row_cb,
                       void* row_user_data,
                       int64_t* out_row_count) {
    vox_db_mysql_native_t* n = get_native(conn);
    if (!n || !n->mysql || !sql) return -1;

    /* 如果有参数，使用prepared statement；否则使用普通查询 */
    if (params && nparams > 0) {
        MYSQL_STMT* stmt = mysql_stmt_init(n->mysql);
        if (!stmt) {
            set_err(n, mysql_error(n->mysql));
            return -1;
        }

        if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) {
            set_err(n, mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            return -1;
        }

        if (bind_mysql_params(conn->mpool, stmt, params, nparams) != 0) {
            set_err(n, "mysql: failed to bind parameters");
            mysql_stmt_close(stmt);
            return -1;
        }

        if (mysql_stmt_execute(stmt) != 0) {
            set_err(n, mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            return -1;
        }

        /* 获取结果集元数据 */
        MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
        if (!meta) {
            /* 若不是查询语句，result_metadata 会返回 NULL */
            if (mysql_field_count(n->mysql) == 0) {
                if (out_row_count) *out_row_count = 0;
                mysql_stmt_close(stmt);
                return 0;
            }
            set_err(n, mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            return -1;
        }

        unsigned int cols = mysql_num_fields(meta);
        MYSQL_FIELD* fields = mysql_fetch_fields(meta);

        const char** col_names = NULL;
        vox_db_value_t* values = NULL;
        MYSQL_BIND* result_binds = NULL;
        unsigned long* lengths = NULL;
        my_bool* is_null = NULL;
        char** buffers = NULL;

        if (cols > 0) {
            col_names = (const char**)vox_mpool_alloc(conn->mpool, (size_t)cols * sizeof(char*));
            values = (vox_db_value_t*)vox_mpool_alloc(conn->mpool, (size_t)cols * sizeof(vox_db_value_t));
            result_binds = (MYSQL_BIND*)vox_mpool_alloc(conn->mpool, cols * sizeof(MYSQL_BIND));
            lengths = (unsigned long*)vox_mpool_alloc(conn->mpool, cols * sizeof(unsigned long));
            is_null = (my_bool*)vox_mpool_alloc(conn->mpool, cols * sizeof(my_bool));
            buffers = (char**)vox_mpool_alloc(conn->mpool, cols * sizeof(char*));

            if (!col_names || !values || !result_binds || !lengths || !is_null || !buffers) {
                if (col_names) vox_mpool_free(conn->mpool, col_names);
                if (values) vox_mpool_free(conn->mpool, values);
                if (result_binds) vox_mpool_free(conn->mpool, result_binds);
                if (lengths) vox_mpool_free(conn->mpool, lengths);
                if (is_null) vox_mpool_free(conn->mpool, is_null);
                if (buffers) vox_mpool_free(conn->mpool, buffers);
                mysql_free_result(meta);
                mysql_stmt_close(stmt);
                return -1;
            }
            memset(result_binds, 0, cols * sizeof(MYSQL_BIND));
            memset(lengths, 0, cols * sizeof(unsigned long));
            memset(is_null, 0, cols * sizeof(my_bool));
            memset(buffers, 0, cols * sizeof(char*));

            /* 为每列分配缓冲区并绑定 */
            for (unsigned int c = 0; c < cols; c++) {
                col_names[c] = fields[c].name;
                /* 分配足够大的缓冲区（假设最大64KB） */
                buffers[c] = (char*)vox_mpool_alloc(conn->mpool, 65536);
                if (!buffers[c]) {
                    for (unsigned int j = 0; j < c; j++) vox_mpool_free(conn->mpool, buffers[j]);
                    if (col_names) vox_mpool_free(conn->mpool, col_names);
                    if (values) vox_mpool_free(conn->mpool, values);
                    vox_mpool_free(conn->mpool, result_binds);
                    vox_mpool_free(conn->mpool, lengths);
                    vox_mpool_free(conn->mpool, is_null);
                    vox_mpool_free(conn->mpool, buffers);
                    mysql_free_result(meta);
                    mysql_stmt_close(stmt);
                    return -1;
                }
                result_binds[c].buffer_type = MYSQL_TYPE_STRING;
                result_binds[c].buffer = buffers[c];
                result_binds[c].buffer_length = 65536;
                result_binds[c].length = &lengths[c];
                result_binds[c].is_null = &is_null[c];
            }

            if (mysql_stmt_bind_result(stmt, result_binds) != 0) {
                for (unsigned int c = 0; c < cols; c++) vox_mpool_free(conn->mpool, buffers[c]);
                if (col_names) vox_mpool_free(conn->mpool, col_names);
                if (values) vox_mpool_free(conn->mpool, values);
                vox_mpool_free(conn->mpool, result_binds);
                vox_mpool_free(conn->mpool, lengths);
                vox_mpool_free(conn->mpool, is_null);
                vox_mpool_free(conn->mpool, buffers);
                mysql_free_result(meta);
                mysql_stmt_close(stmt);
                return -1;
            }

            /* 存储结果集到客户端（可选，但有助于性能） */
            (void)mysql_stmt_store_result(stmt);
        }

        int64_t row_count = 0;
        while (mysql_stmt_fetch(stmt) == 0) {
            for (unsigned int c = 0; c < cols; c++) {
                if (is_null[c]) {
                    values[c].type = VOX_DB_TYPE_NULL;
                    continue;
                }
                values[c].type = VOX_DB_TYPE_TEXT;
                values[c].u.text.ptr = buffers[c];
                values[c].u.text.len = lengths[c];
            }

            if (row_cb) {
                vox_db_row_t r = {
                    .column_count = (size_t)cols,
                    .column_names = (const char* const*)col_names,
                    .values = values
                };
                row_cb(conn, &r, row_user_data);
            }
            row_count++;
        }

        /* 清理资源 */
        if (buffers) {
            for (unsigned int c = 0; c < cols; c++) {
                if (buffers[c]) vox_mpool_free(conn->mpool, buffers[c]);
            }
            vox_mpool_free(conn->mpool, buffers);
        }
        if (result_binds) vox_mpool_free(conn->mpool, result_binds);
        if (lengths) vox_mpool_free(conn->mpool, lengths);
        if (is_null) vox_mpool_free(conn->mpool, is_null);

        if (out_row_count) *out_row_count = row_count;

        if (col_names) vox_mpool_free(conn->mpool, col_names);
        if (values) vox_mpool_free(conn->mpool, values);
        mysql_free_result(meta);
        mysql_stmt_close(stmt);
        return 0;
    }

    /* 无参数时使用普通查询（保持向后兼容） */
    if (mysql_real_query(n->mysql, sql, (unsigned long)strlen(sql)) != 0) {
        set_err(n, mysql_error(n->mysql));
        return -1;
    }

    MYSQL_RES* res = mysql_store_result(n->mysql);
    if (!res) {
        /* 若不是查询语句，store_result 会返回 NULL 且 field_count==0 */
        if (mysql_field_count(n->mysql) == 0) {
            if (out_row_count) *out_row_count = 0;
            return 0;
        }
        set_err(n, mysql_error(n->mysql));
        return -1;
    }

    unsigned int cols = mysql_num_fields(res);
    MYSQL_FIELD* fields = mysql_fetch_fields(res);

    const char** col_names = NULL;
    vox_db_value_t* values = NULL;
    if (cols > 0) {
        col_names = (const char**)vox_mpool_alloc(conn->mpool, (size_t)cols * sizeof(char*));
        values = (vox_db_value_t*)vox_mpool_alloc(conn->mpool, (size_t)cols * sizeof(vox_db_value_t));
        if (!col_names || !values) {
            if (col_names) vox_mpool_free(conn->mpool, col_names);
            if (values) vox_mpool_free(conn->mpool, values);
            mysql_free_result(res);
            return -1;
        }
        for (unsigned int c = 0; c < cols; c++) {
            col_names[c] = fields[c].name;
        }
    }

    int64_t row_count = 0;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL) {
        unsigned long* lens = mysql_fetch_lengths(res);
        for (unsigned int c = 0; c < cols; c++) {
            if (!row[c]) {
                values[c].type = VOX_DB_TYPE_NULL;
                continue;
            }
            values[c].type = VOX_DB_TYPE_TEXT;
            values[c].u.text.ptr = row[c];
            values[c].u.text.len = lens ? (size_t)lens[c] : strlen(row[c]);
        }

        if (row_cb) {
            vox_db_row_t r = {
                .column_count = (size_t)cols,
                .column_names = (const char* const*)col_names,
                .values = values
            };
            row_cb(conn, &r, row_user_data);
        }
        row_count++;
    }

    if (out_row_count) *out_row_count = row_count;

    if (col_names) vox_mpool_free(conn->mpool, col_names);
    if (values) vox_mpool_free(conn->mpool, values);
    mysql_free_result(res);
    return 0;
}

static int db_mysql_begin_transaction(vox_db_conn_t* conn) {
    vox_db_mysql_native_t* n = get_native(conn);
    if (!n || !n->mysql) return -1;
    
    if (mysql_real_query(n->mysql, "START TRANSACTION", 17) != 0) {
        set_err(n, mysql_error(n->mysql));
        return -1;
    }
    
    /* 需要读取结果以清除命令状态 */
    MYSQL_RES* res = mysql_store_result(n->mysql);
    if (res) mysql_free_result(res);
    return 0;
}

static int db_mysql_commit(vox_db_conn_t* conn) {
    vox_db_mysql_native_t* n = get_native(conn);
    if (!n || !n->mysql) return -1;
    
    if (mysql_real_query(n->mysql, "COMMIT", 6) != 0) {
        set_err(n, mysql_error(n->mysql));
        return -1;
    }
    
    /* 需要读取结果以清除命令状态 */
    MYSQL_RES* res = mysql_store_result(n->mysql);
    if (res) mysql_free_result(res);
    return 0;
}

static int db_mysql_rollback(vox_db_conn_t* conn) {
    vox_db_mysql_native_t* n = get_native(conn);
    if (!n || !n->mysql) return -1;
    
    if (mysql_real_query(n->mysql, "ROLLBACK", 8) != 0) {
        set_err(n, mysql_error(n->mysql));
        return -1;
    }
    
    /* 需要读取结果以清除命令状态 */
    MYSQL_RES* res = mysql_store_result(n->mysql);
    if (res) mysql_free_result(res);
    return 0;
}

static const vox_db_driver_vtbl_t g_mysql_vtbl = {
    .name = "mysql",
    .use_loop_thread_for_async = 1,  /* libmysqlclient 不建议跨线程共享连接，需 mysql_thread_init 或同线程使用 */
    .connect = db_mysql_connect,
    .disconnect = db_mysql_disconnect,
    .ping = db_mysql_ping,
    .exec = db_mysql_exec,
    .query = db_mysql_query,
    .begin_transaction = db_mysql_begin_transaction,
    .commit = db_mysql_commit,
    .rollback = db_mysql_rollback,
    .last_error = db_mysql_last_error
};

const vox_db_driver_vtbl_t* vox_db_mysql_vtbl(void) {
    return &g_mysql_vtbl;
}

#endif /* VOX_USE_MYSQL */

