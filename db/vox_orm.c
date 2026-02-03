/*
 * vox_orm.c - ORM 层实现（基于 vox_db）
 */

#include "vox_orm.h"

#include "../vox_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define VOX_ORM_SQL_BUF_SIZE 2048

/* 按驱动生成占位符：PG 用 $1,$2,...，其余用 ? */
static int orm_placeholder_style(vox_db_driver_t driver) {
    return (driver == VOX_DB_DRIVER_PGSQL) ? 1 : 0;
}

/* 从结构体按描述符填充 params（用于 INSERT/UPDATE），跳过 auto_gen 的字段（仅 INSERT 时） */
static void orm_struct_to_params(const void* row_struct,
                                  const vox_orm_field_t* fields,
                                  size_t nfields,
                                  vox_db_value_t* params,
                                  size_t* out_nparams,
                                  int skip_auto_gen)
{
    size_t n = 0;
    const char* base = (const char*)row_struct;
    for (size_t i = 0; i < nfields; i++) {
        if (skip_auto_gen && fields[i].auto_gen)
            continue;
        vox_db_value_t* v = &params[n];
        v->type = fields[i].type;
        switch (fields[i].type) {
            case VOX_DB_TYPE_NULL:
                break;
            case VOX_DB_TYPE_I64:
                v->u.i64 = *(int64_t*)(base + fields[i].offset);
                break;
            case VOX_DB_TYPE_U64:
                v->u.u64 = *(uint64_t*)(base + fields[i].offset);
                break;
            case VOX_DB_TYPE_F64:
                v->u.f64 = *(double*)(base + fields[i].offset);
                break;
            case VOX_DB_TYPE_BOOL:
                v->u.boolean = *(bool*)(base + fields[i].offset);
                break;
            case VOX_DB_TYPE_TEXT: {
                const char* p = (const char*)(base + fields[i].offset);
                v->u.text.ptr = p;
                v->u.text.len = p ? strlen(p) : 0;
                break;
            }
            case VOX_DB_TYPE_BLOB: {
                const vox_db_blob_t* b = (const vox_db_blob_t*)(base + fields[i].offset);
                v->u.blob.data = b->data;
                v->u.blob.len = b->len;
                break;
            }
            default:
                v->type = VOX_DB_TYPE_NULL;
                break;
        }
        n++;
    }
    *out_nparams = n;
}

/* 按列名在 row 中找下标，未找到返回 (size_t)-1 */
static size_t orm_column_index(const vox_db_row_t* row, const char* name) {
    if (!row->column_names) return (size_t)-1;
    for (size_t i = 0; i < row->column_count; i++) {
        if (row->column_names[i] && strcmp(row->column_names[i], name) == 0)
            return i;
    }
    return (size_t)-1;
}

/* 解析 TEXT 为布尔：1/true/yes/on/t 为 true，0/false/no/off/f 为 false（PostgreSQL 返回 't'/'f'） */
static int orm_text_to_bool(const char* ptr, size_t len) {
    if (!ptr || len == 0) return 0;
    if (len == 1) {
        char c = (char)tolower((unsigned char)*ptr);
        if (c == '1' || c == 'y' || c == 't') return 1;
        if (c == '0' || c == 'n' || c == 'f') return 0;
    }
    if (len == 4 && tolower((unsigned char)ptr[0]) == 't' && tolower((unsigned char)ptr[1]) == 'r'
        && tolower((unsigned char)ptr[2]) == 'u' && tolower((unsigned char)ptr[3]) == 'e') return 1;
    if (len == 3 && tolower((unsigned char)ptr[0]) == 'y' && tolower((unsigned char)ptr[1]) == 'e'
        && tolower((unsigned char)ptr[2]) == 's') return 1;
    if (len == 2 && tolower((unsigned char)ptr[0]) == 'o' && tolower((unsigned char)ptr[1]) == 'n') return 1;
    return 0;
}

/* 将 row 的一列写入结构体字段（按 field 类型写，兼容 row 为 I64 时 BOOL 列；MySQL 等驱动返回 TEXT 时解析） */
static void orm_value_to_field(void* struct_base,
                               const vox_orm_field_t* field,
                               const vox_db_value_t* val)
{
    char* dst = (char*)struct_base + field->offset;
    size_t buf_size = field->buffer_size ? field->buffer_size : 256;

    /* 驱动返回 TEXT 时解析为数值/布尔（如 MySQL 当前全部以 TEXT 返回） */
    if (val->type == VOX_DB_TYPE_TEXT && val->u.text.ptr && val->u.text.len > 0) {
        char num_buf[32];
        size_t tlen = val->u.text.len;
        if (tlen >= sizeof(num_buf)) tlen = sizeof(num_buf) - 1;
        memcpy(num_buf, val->u.text.ptr, tlen);
        num_buf[tlen] = '\0';

        switch (field->type) {
            case VOX_DB_TYPE_I64:
                *(int64_t*)dst = (int64_t)strtoll(num_buf, NULL, 10);
                return;
            case VOX_DB_TYPE_U64:
                *(uint64_t*)dst = (uint64_t)strtoull(num_buf, NULL, 10);
                return;
            case VOX_DB_TYPE_F64:
                *(double*)dst = strtod(num_buf, NULL);
                return;
            case VOX_DB_TYPE_BOOL:
                *(bool*)dst = (bool)orm_text_to_bool(val->u.text.ptr, val->u.text.len);
                return;
            default:
                break;
        }
    }

    /* 按目标字段类型写，避免 SQLite 等把 BOOL 列当 I64 返回时写成 8 字节导致越界 */
    switch (field->type) {
        case VOX_DB_TYPE_I64:
            if (val->type == VOX_DB_TYPE_I64) *(int64_t*)dst = val->u.i64;
            else if (val->type == VOX_DB_TYPE_U64) *(int64_t*)dst = (int64_t)val->u.u64;
            else if (val->type == VOX_DB_TYPE_NULL) *(int64_t*)dst = 0;
            break;
        case VOX_DB_TYPE_U64:
            if (val->type == VOX_DB_TYPE_U64) *(uint64_t*)dst = val->u.u64;
            else if (val->type == VOX_DB_TYPE_I64) *(uint64_t*)dst = (uint64_t)val->u.i64;
            else if (val->type == VOX_DB_TYPE_NULL) *(uint64_t*)dst = 0;
            break;
        case VOX_DB_TYPE_F64:
            if (val->type == VOX_DB_TYPE_F64) *(double*)dst = val->u.f64;
            else if (val->type == VOX_DB_TYPE_I64) *(double*)dst = (double)val->u.i64;
            else if (val->type == VOX_DB_TYPE_NULL) *(double*)dst = 0.0;
            break;
        case VOX_DB_TYPE_BOOL:
            if (val->type == VOX_DB_TYPE_BOOL) *(bool*)dst = val->u.boolean;
            else if (val->type == VOX_DB_TYPE_I64) *(bool*)dst = (val->u.i64 != 0);
            else if (val->type == VOX_DB_TYPE_U64) *(bool*)dst = (val->u.u64 != 0);
            else if (val->type == VOX_DB_TYPE_NULL) *(bool*)dst = false;
            break;
        case VOX_DB_TYPE_TEXT: {
            if (val->type != VOX_DB_TYPE_TEXT) break;
            size_t len = val->u.text.len;
            if (len >= buf_size) len = buf_size - 1;
            if (val->u.text.ptr && len > 0)
                memcpy(dst, val->u.text.ptr, len);
            dst[len] = '\0';
            break;
        }
        case VOX_DB_TYPE_BLOB: {
            if (val->type != VOX_DB_TYPE_BLOB) break;
            vox_db_blob_t* b = (vox_db_blob_t*)dst;
            b->data = val->u.blob.data;
            b->len = val->u.blob.len < buf_size ? val->u.blob.len : buf_size;
            break;
        }
        case VOX_DB_TYPE_NULL:
        default:
            break;
    }
}

/* 将整行写入结构体 */
static void orm_row_to_struct(const vox_db_row_t* row,
                             void* row_struct,
                             size_t row_size,
                             const vox_orm_field_t* fields,
                             size_t nfields)
{
    (void)row_size;
    memset(row_struct, 0, row_size);
    const char* base = (const char*)row_struct;
    for (size_t i = 0; i < nfields; i++) {
        size_t ci = orm_column_index(row, fields[i].name);
        if (ci == (size_t)-1 || ci >= row->column_count) continue;
        orm_value_to_field((void*)base, &fields[i], &row->values[ci]);
    }
}

/* 生成 INSERT SQL 与参数个数（占位符风格：0=? 1=$1,$2,...） */
static int orm_build_insert_sql(char* buf,
                                 size_t buf_size,
                                 vox_db_driver_t driver,
                                 const char* table,
                                 const vox_orm_field_t* fields,
                                 size_t nfields,
                                 int skip_auto_gen,
                                 size_t* out_nparams)
{
    int pg = orm_placeholder_style(driver);
    size_t n = 0;
    int len = snprintf(buf, buf_size, "INSERT INTO %s (", table);
    if (len < 0 || (size_t)len >= buf_size) return -1;
    size_t off = (size_t)len;

    int first = 1;
    for (size_t i = 0; i < nfields; i++) {
        if (skip_auto_gen && fields[i].auto_gen) continue;
        if (!first) { buf[off++] = ','; if (off >= buf_size) return -1; }
        len = snprintf(buf + off, buf_size - off, "%s", fields[i].name);
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
        off += (size_t)len;
        n++;
        first = 0;
    }
    if (n == 0) return -1;
    if (pg) {
        len = snprintf(buf + off, buf_size - off, ") VALUES (");
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
        off += (size_t)len;
        for (size_t k = 0; k < n; k++) {
            if (k) { buf[off++] = ','; if (off >= buf_size) return -1; }
            len = snprintf(buf + off, buf_size - off, "$%zu", k + 1);
            if (len < 0 || off + (size_t)len >= buf_size) return -1;
            off += (size_t)len;
        }
    } else {
        len = snprintf(buf + off, buf_size - off, ") VALUES (");
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
        off += (size_t)len;
        for (size_t k = 0; k < n; k++) {
            if (k) { buf[off++] = ','; if (off >= buf_size) return -1; }
            buf[off++] = '?';
            if (off >= buf_size) return -1;
        }
    }
    len = snprintf(buf + off, buf_size - off, ")");
    if (len < 0 || off + (size_t)len >= buf_size) return -1;
    *out_nparams = n;
    return 0;
}

/* 生成 UPDATE SQL：SET c1=?,c2=? WHERE ...；参数顺序：所有字段值 + where_params */
static int orm_build_update_sql(char* buf,
                                size_t buf_size,
                                vox_db_driver_t driver,
                                const char* table,
                                const vox_orm_field_t* fields,
                                size_t nfields,
                                const char* where_clause,
                                size_t n_where_params,
                                size_t* out_total_params)
{
    int pg = orm_placeholder_style(driver);
    int len = snprintf(buf, buf_size, "UPDATE %s SET ", table);
    if (len < 0 || (size_t)len >= buf_size) return -1;
    size_t off = (size_t)len;
    size_t idx = 0;
    for (size_t i = 0; i < nfields; i++) {
        if (i) { buf[off++] = ','; if (off >= buf_size) return -1; }
        if (pg) {
            len = snprintf(buf + off, buf_size - off, "%s=$%zu", fields[i].name, ++idx);
        } else {
            len = snprintf(buf + off, buf_size - off, "%s=?", fields[i].name);
        }
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
        off += (size_t)len;
    }
    /* WHERE：PG 时把 ? 写成 $nfields+1, $nfields+2, ... */
    if (pg) {
        len = snprintf(buf + off, buf_size - off, " WHERE ");
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
        off += (size_t)len;
        size_t p = nfields + 1;
        for (const char* s = where_clause; *s && off < buf_size; s++) {
            if (*s == '?') {
                len = snprintf(buf + off, buf_size - off, "$%zu", p++);
                if (len < 0 || off + (size_t)len >= buf_size) return -1;
                off += (size_t)len;
            } else {
                buf[off++] = *s;
            }
        }
    } else {
        len = snprintf(buf + off, buf_size - off, " WHERE %s", where_clause);
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
    }
    *out_total_params = nfields + n_where_params;
    return 0;
}

/* 生成 SELECT SQL：SELECT c1,c2,... FROM table WHERE ... [LIMIT 1]；PG 时 WHERE 中 ? 写成 $1,$2,... */
static int orm_build_select_sql(char* buf,
                                 size_t buf_size,
                                 vox_db_driver_t driver,
                                 const char* table,
                                 const vox_orm_field_t* fields,
                                 size_t nfields,
                                 const char* where_clause,
                                 int limit_one)
{
    int pg = orm_placeholder_style(driver);
    int len = snprintf(buf, buf_size, "SELECT ");
    if (len < 0 || (size_t)len >= buf_size) return -1;
    size_t off = (size_t)len;
    for (size_t i = 0; i < nfields; i++) {
        if (i) { buf[off++] = ','; if (off >= buf_size) return -1; }
        len = snprintf(buf + off, buf_size - off, "%s", fields[i].name);
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
        off += (size_t)len;
    }
    len = snprintf(buf + off, buf_size - off, " FROM %s", table);
    if (len < 0 || off + (size_t)len >= buf_size) return -1;
    off += (size_t)len;
    if (pg) {
        len = snprintf(buf + off, buf_size - off, " WHERE ");
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
        off += (size_t)len;
        size_t p = 1;
        for (const char* s = where_clause; *s && off < buf_size; s++) {
            if (*s == '?') {
                len = snprintf(buf + off, buf_size - off, "$%zu", p++);
                if (len < 0 || off + (size_t)len >= buf_size) return -1;
                off += (size_t)len;
            } else {
                buf[off++] = *s;
            }
        }
    } else {
        len = snprintf(buf + off, buf_size - off, " WHERE %s", where_clause);
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
        off += (size_t)len;
    }
    if (limit_one) {
        len = snprintf(buf + off, buf_size - off, " LIMIT 1");
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
    } else {
        /* pg 分支用逐字符写入 WHERE，未调用 snprintf，必须显式补 '\0' */
        if (off >= buf_size) return -1;
        buf[off] = '\0';
    }
    return 0;
}

typedef struct {
    vox_db_conn_t* conn;
    vox_orm_exec_cb cb;
    void* user_data;
    char* sql_copy;  /* 异步时 SQL 的 mpool 副本（如 drop_table_async），可为 NULL */
    vox_db_value_t* params;  /* 仅 insert/update 异步时使用，done 时释放 */
    size_t nparams;
} orm_exec_ctx_t;

/* ========== DDL：建表 SQL 生成（按驱动映射列类型与主键/自增） ========== */

static int orm_append_column_type(char* buf, size_t buf_size, size_t* off,
                                   vox_db_driver_t driver,
                                   const vox_orm_field_t* field)
{
    int len;
    size_t n = field->buffer_size ? field->buffer_size : 255;
    switch (field->type) {
        case VOX_DB_TYPE_I64:
            if (driver == VOX_DB_DRIVER_SQLITE3)
                len = snprintf(buf + *off, buf_size - *off, "INTEGER");
            else if (driver == VOX_DB_DRIVER_PGSQL)
                len = snprintf(buf + *off, buf_size - *off, "BIGINT");
            else if (driver == VOX_DB_DRIVER_MYSQL)
                len = snprintf(buf + *off, buf_size - *off, "BIGINT");
            else
                len = snprintf(buf + *off, buf_size - *off, "BIGINT");
            break;
        case VOX_DB_TYPE_U64:
            if (driver == VOX_DB_DRIVER_SQLITE3)
                len = snprintf(buf + *off, buf_size - *off, "INTEGER");
            else if (driver == VOX_DB_DRIVER_PGSQL)
                len = snprintf(buf + *off, buf_size - *off, "BIGINT");
            else if (driver == VOX_DB_DRIVER_DUCKDB)
                len = snprintf(buf + *off, buf_size - *off, "UBIGINT");
            else
                len = snprintf(buf + *off, buf_size - *off, "BIGINT");
            break;
        case VOX_DB_TYPE_F64:
            if (driver == VOX_DB_DRIVER_SQLITE3)
                len = snprintf(buf + *off, buf_size - *off, "REAL");
            else if (driver == VOX_DB_DRIVER_PGSQL)
                len = snprintf(buf + *off, buf_size - *off, "DOUBLE PRECISION");
            else
                len = snprintf(buf + *off, buf_size - *off, "DOUBLE");
            break;
        case VOX_DB_TYPE_BOOL:
            if (driver == VOX_DB_DRIVER_SQLITE3)
                len = snprintf(buf + *off, buf_size - *off, "INTEGER");
            else if (driver == VOX_DB_DRIVER_MYSQL)
                len = snprintf(buf + *off, buf_size - *off, "TINYINT(1)");
            else
                len = snprintf(buf + *off, buf_size - *off, "BOOLEAN");
            break;
        case VOX_DB_TYPE_TEXT:
            if (driver == VOX_DB_DRIVER_MYSQL && n > 0 && n < 65535)
                len = snprintf(buf + *off, buf_size - *off, "VARCHAR(%zu)", n);
            else if (driver == VOX_DB_DRIVER_DUCKDB && n > 0)
                len = snprintf(buf + *off, buf_size - *off, "VARCHAR");
            else
                len = snprintf(buf + *off, buf_size - *off, "TEXT");
            break;
        case VOX_DB_TYPE_BLOB:
            if (driver == VOX_DB_DRIVER_PGSQL)
                len = snprintf(buf + *off, buf_size - *off, "BYTEA");
            else
                len = snprintf(buf + *off, buf_size - *off, "BLOB");
            break;
        case VOX_DB_TYPE_NULL:
        default:
            len = snprintf(buf + *off, buf_size - *off, "TEXT");
            break;
    }
    if (len < 0 || *off + (size_t)len >= buf_size) return -1;
    *off += (size_t)len;
    return 0;
}

static int orm_build_create_table_sql(char* buf, size_t buf_size,
                                     vox_db_driver_t driver,
                                     const char* table,
                                     const vox_orm_field_t* fields,
                                     size_t nfields)
{
    int len = snprintf(buf, buf_size, "CREATE TABLE IF NOT EXISTS %s (", table);
    if (len < 0 || (size_t)len >= buf_size) return -1;
    size_t off = (size_t)len;

    /* 收集主键列名（用于多列主键时末尾 PRIMARY KEY (a,b)） */
    size_t pk_count = 0;
    for (size_t i = 0; i < nfields; i++)
        if (fields[i].is_primary_key) pk_count++;

    for (size_t i = 0; i < nfields; i++) {
        if (i) { buf[off++] = ','; if (off >= buf_size) return -1; }

        const vox_orm_field_t* f = &fields[i];
        int is_pk = f->is_primary_key;
        int is_auto = f->auto_gen;

        if (driver == VOX_DB_DRIVER_PGSQL && is_pk && is_auto && f->type == VOX_DB_TYPE_I64) {
            len = snprintf(buf + off, buf_size - off, "%s BIGSERIAL PRIMARY KEY", f->name);
            if (len < 0 || off + (size_t)len >= buf_size) return -1;
            off += (size_t)len;
            continue;
        }

        /* DuckDB 无内置 AUTO_INCREMENT，用 SEQUENCE + DEFAULT nextval 实现自增主键 */
        if (driver == VOX_DB_DRIVER_DUCKDB && is_pk && is_auto && f->type == VOX_DB_TYPE_I64) {
            len = snprintf(buf + off, buf_size - off, "%s BIGINT PRIMARY KEY DEFAULT nextval('seq_%s_%s')", f->name, table, f->name);
            if (len < 0 || off + (size_t)len >= buf_size) return -1;
            off += (size_t)len;
            continue;
        }

        len = snprintf(buf + off, buf_size - off, "%s ", f->name);
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
        off += (size_t)len;
        if (orm_append_column_type(buf, buf_size, &off, driver, f) != 0) return -1;

        if (pk_count <= 1 && is_pk) {
            if (driver == VOX_DB_DRIVER_SQLITE3) {
                len = snprintf(buf + off, buf_size - off, " PRIMARY KEY%s", is_auto ? " AUTOINCREMENT" : "");
            } else if (driver == VOX_DB_DRIVER_MYSQL) {
                len = snprintf(buf + off, buf_size - off, "%s PRIMARY KEY", is_auto ? " AUTO_INCREMENT" : "");
            } else if (driver == VOX_DB_DRIVER_DUCKDB || driver == VOX_DB_DRIVER_PGSQL) {
                len = snprintf(buf + off, buf_size - off, " PRIMARY KEY");
            } else {
                len = snprintf(buf + off, buf_size - off, " PRIMARY KEY");
            }
            if (len < 0 || off + (size_t)len >= buf_size) return -1;
            off += (size_t)len;
        }
    }

    if (pk_count > 1) {
        len = snprintf(buf + off, buf_size - off, ", PRIMARY KEY (");
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
        off += (size_t)len;
        int first = 1;
        for (size_t i = 0; i < nfields; i++) {
            if (!fields[i].is_primary_key) continue;
            if (!first) { buf[off++] = ','; if (off >= buf_size) return -1; }
            len = snprintf(buf + off, buf_size - off, "%s", fields[i].name);
            if (len < 0 || off + (size_t)len >= buf_size) return -1;
            off += (size_t)len;
            first = 0;
        }
        buf[off++] = ')';
        if (off >= buf_size) return -1;
    }

    len = snprintf(buf + off, buf_size - off, ")");
    if (len < 0 || off + (size_t)len >= buf_size) return -1;
    return 0;
}

/* DuckDB 自增主键：返回唯一的 auto_gen I64 主键列名，若无则返回 NULL */
static const char* orm_duckdb_auto_pk_column(const char* table,
                                              const vox_orm_field_t* fields,
                                              size_t nfields)
{
    const char* found = NULL;
    (void)table;
    for (size_t i = 0; i < nfields; i++) {
        if (fields[i].is_primary_key && fields[i].auto_gen && fields[i].type == VOX_DB_TYPE_I64) {
            if (found != NULL) return NULL; /* 多个自增主键列时不使用 sequence */
            found = fields[i].name;
        }
    }
    return found;
}

/* 前向声明：索引 SQL 生成（定义在“索引”节） */
static int orm_build_create_index_sql(char* buf, size_t buf_size,
                                       vox_db_driver_t driver,
                                       const char* table,
                                       const char* index_name,
                                       const char* const* columns,
                                       size_t ncols,
                                       int is_unique);

/* ========== DDL API ========== */

/* 建表后按描述符自动建单列索引（indexed / unique_index）；索引名 idx_表名_列名 */
static int orm_create_indexes_for_table(vox_db_conn_t* conn,
                                          const char* table,
                                          const vox_orm_field_t* fields,
                                          size_t nfields)
{
    vox_db_driver_t driver = vox_db_get_driver(conn);
    char sql[VOX_ORM_SQL_BUF_SIZE];
    char index_name[256];
    for (size_t i = 0; i < nfields; i++) {
        if (!fields[i].indexed && !fields[i].unique_index) continue;
        int len = snprintf(index_name, sizeof(index_name), "idx_%s_%s", table, fields[i].name);
        if (len < 0 || (size_t)len >= sizeof(index_name)) return -1;
        const char* col = fields[i].name;
        const char* const cols[] = { col };
        if (orm_build_create_index_sql(sql, sizeof(sql), driver, table, index_name, cols, 1, fields[i].unique_index ? 1 : 0) != 0)
            return -1;
        int64_t aff = 0;
        if (vox_db_exec(conn, sql, NULL, 0, &aff) != 0) return -1;
    }
    return 0;
}

int vox_orm_create_table(vox_db_conn_t* conn,
                         const char* table,
                         const vox_orm_field_t* fields,
                         size_t nfields)
{
    if (!conn || !table || !fields || nfields == 0) return -1;
    vox_db_driver_t driver = vox_db_get_driver(conn);
    /* DuckDB 自增主键：先建 SEQUENCE，CREATE TABLE 中已含 DEFAULT nextval(...) */
    if (driver == VOX_DB_DRIVER_DUCKDB) {
        const char* col = orm_duckdb_auto_pk_column(table, fields, nfields);
        if (col) {
            char sql[VOX_ORM_SQL_BUF_SIZE];
            int len = snprintf(sql, sizeof(sql), "DROP SEQUENCE IF EXISTS seq_%s_%s", table, col);
            if (len > 0 && (size_t)len < sizeof(sql)) {
                int64_t aff = 0;
                if (vox_db_exec(conn, sql, NULL, 0, &aff) != 0) return -1;
            }
            len = snprintf(sql, sizeof(sql), "CREATE SEQUENCE seq_%s_%s START 1", table, col);
            if (len > 0 && (size_t)len < sizeof(sql)) {
                int64_t aff = 0;
                if (vox_db_exec(conn, sql, NULL, 0, &aff) != 0) return -1;
            }
        }
    }
    char sql[VOX_ORM_SQL_BUF_SIZE];
    if (orm_build_create_table_sql(sql, sizeof(sql), driver, table, fields, nfields) != 0)
        return -1;
    int64_t aff = 0;
    if (vox_db_exec(conn, sql, NULL, 0, &aff) != 0) return -1;
    if (orm_create_indexes_for_table(conn, table, fields, nfields) != 0) return -1;
    return 0;
}

typedef struct {
    vox_db_conn_t* conn;
    const char* table;
    const vox_orm_field_t* fields;
    size_t nfields;
    size_t next_index;
    int status;
    vox_orm_exec_cb cb;
    void* user_data;
    char* sql_copy;  /* 异步时 SQL 的 mpool 副本，避免栈上 buffer 在任务执行前失效 */
} orm_create_table_ctx_t;

/* DuckDB 异步建表：先 DROP SEQUENCE -> CREATE SEQUENCE -> CREATE TABLE -> 索引 */
typedef struct {
    orm_create_table_ctx_t* create_ctx;
    const char* table;
    const char* col;
    char* drop_seq_sql;    /* DROP SEQUENCE ... 的 mpool 副本（栈上 SQL 在回调返回后失效） */
    char* create_seq_sql; /* CREATE SEQUENCE ... 的 mpool 副本 */
    int step;              /* 0=刚完成 DROP SEQUENCE, 1=刚完成 CREATE SEQUENCE */
} orm_duckdb_seq_ctx_t;

static void orm_run_next_index_or_done(orm_create_table_ctx_t* ctx);
static void orm_create_table_then_index_done(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data);

static void orm_duckdb_seq_step_done(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    orm_duckdb_seq_ctx_t* dctx = (orm_duckdb_seq_ctx_t*)user_data;
    (void)affected_rows;
    /* DROP/CREATE SEQUENCE 失败时直接回调用户并释放，不进入索引流程（表未建） */
    if (status != 0 && dctx->create_ctx) {
        orm_create_table_ctx_t* ctx = dctx->create_ctx;
        vox_orm_exec_cb cb = ctx->cb;
        void* ud = ctx->user_data;
        ctx->status = status;
        vox_mpool_t* mp = vox_db_get_mpool(conn);
        if (dctx->drop_seq_sql) vox_mpool_free(mp, dctx->drop_seq_sql);
        if (dctx->create_seq_sql) vox_mpool_free(mp, dctx->create_seq_sql);
        vox_mpool_free(mp, ctx->sql_copy);
        ctx->sql_copy = NULL;
        vox_mpool_free(mp, ctx);
        vox_mpool_free(mp, dctx);
        if (cb) cb(conn, status, 0, ud);
        return;
    }
    if (!dctx->create_ctx) {
        vox_mpool_t* mp = vox_db_get_mpool(conn);
        if (dctx->drop_seq_sql) vox_mpool_free(mp, dctx->drop_seq_sql);
        if (dctx->create_seq_sql) vox_mpool_free(mp, dctx->create_seq_sql);
        vox_mpool_free(mp, dctx);
        return;
    }
    if (dctx->step == 0) {
        dctx->step = 1;
        vox_db_exec_async(conn, dctx->create_seq_sql, NULL, 0, orm_duckdb_seq_step_done, dctx);
        return;
    }
    /* step == 1: 执行 CREATE TABLE，然后走索引流程 */
    vox_db_exec_async(conn, dctx->create_ctx->sql_copy, NULL, 0, orm_create_table_then_index_done, dctx->create_ctx);
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (dctx->drop_seq_sql) vox_mpool_free(mp, dctx->drop_seq_sql);
    if (dctx->create_seq_sql) vox_mpool_free(mp, dctx->create_seq_sql);
    vox_mpool_free(mp, dctx);
}

static void orm_create_index_step_done(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    orm_create_table_ctx_t* ctx = (orm_create_table_ctx_t*)user_data;
    if (status != 0) ctx->status = status;
    orm_run_next_index_or_done(ctx);
    (void)conn;
    (void)affected_rows;
}

static void orm_run_next_index_or_done(orm_create_table_ctx_t* ctx) {
    vox_db_conn_t* conn = ctx->conn;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    for (; ctx->next_index < ctx->nfields; ctx->next_index++) {
        size_t i = ctx->next_index;
        if (!ctx->fields[i].indexed && !ctx->fields[i].unique_index) continue;
        char sql[VOX_ORM_SQL_BUF_SIZE];
        char index_name[256];
        int len = snprintf(index_name, sizeof(index_name), "idx_%s_%s", ctx->table, ctx->fields[i].name);
        if (len < 0 || (size_t)len >= sizeof(index_name)) {
            ctx->status = -1;
            break;
        }
        const char* col = ctx->fields[i].name;
        const char* const cols[] = { col };
        if (orm_build_create_index_sql(sql, sizeof(sql), vox_db_get_driver(conn), ctx->table, index_name, cols, 1, ctx->fields[i].unique_index ? 1 : 0) != 0) {
            ctx->status = -1;
            break;
        }
        size_t sql_len = strlen(sql) + 1;
        if (ctx->sql_copy) vox_mpool_free(mp, ctx->sql_copy);
        ctx->sql_copy = (char*)vox_mpool_alloc(mp, sql_len);
        if (!ctx->sql_copy) {
            ctx->status = -1;
            break;
        }
        memcpy(ctx->sql_copy, sql, sql_len);
        ctx->next_index++;
        vox_db_exec_async(conn, ctx->sql_copy, NULL, 0, orm_create_index_step_done, ctx);
        return;
    }
    vox_orm_exec_cb cb = ctx->cb;
    void* ud = ctx->user_data;
    if (ctx->sql_copy) {
        vox_mpool_free(mp, ctx->sql_copy);
        ctx->sql_copy = NULL;
    }
    vox_mpool_free(mp, ctx);
    if (cb) cb(conn, ctx->status, 0, ud);
}

static void orm_create_table_then_index_done(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    orm_create_table_ctx_t* ctx = (orm_create_table_ctx_t*)user_data;
    ctx->status = status;
    orm_run_next_index_or_done(ctx);
    (void)conn;
    (void)affected_rows;
}

int vox_orm_create_table_async(vox_db_conn_t* conn,
                                const char* table,
                                const vox_orm_field_t* fields,
                                size_t nfields,
                                vox_orm_exec_cb cb,
                                void* user_data)
{
    if (!conn || !table || !fields || nfields == 0) return -1;
    vox_db_driver_t driver = vox_db_get_driver(conn);
    char sql[VOX_ORM_SQL_BUF_SIZE];
    if (orm_build_create_table_sql(sql, sizeof(sql), driver, table, fields, nfields) != 0)
        return -1;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (!mp) return -1;
    size_t sql_len = strlen(sql) + 1;
    char* sql_copy = (char*)vox_mpool_alloc(mp, sql_len);
    if (!sql_copy) return -1;
    memcpy(sql_copy, sql, sql_len);
    orm_create_table_ctx_t* ctx = (orm_create_table_ctx_t*)vox_mpool_alloc(mp, sizeof(orm_create_table_ctx_t));
    if (!ctx) {
        vox_mpool_free(mp, sql_copy);
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->conn = conn;
    ctx->table = table;
    ctx->fields = fields;
    ctx->nfields = nfields;
    ctx->cb = cb;
    ctx->user_data = user_data;
    ctx->sql_copy = sql_copy;

    /* DuckDB 自增主键：先 DROP SEQUENCE -> CREATE SEQUENCE，再 CREATE TABLE；SQL 须用 mpool 副本，否则异步执行时栈已失效 */
    if (driver == VOX_DB_DRIVER_DUCKDB) {
        const char* col = orm_duckdb_auto_pk_column(table, fields, nfields);
        if (col) {
            char drop_sql[VOX_ORM_SQL_BUF_SIZE];
            char create_seq_buf[128];
            int len = snprintf(drop_sql, sizeof(drop_sql), "DROP SEQUENCE IF EXISTS seq_%s_%s", table, col);
            if (len > 0 && (size_t)len < sizeof(drop_sql)) {
                size_t drop_len = (size_t)len + 1;
                char* drop_seq_copy = (char*)vox_mpool_alloc(mp, drop_len);
                if (drop_seq_copy) {
                    memcpy(drop_seq_copy, drop_sql, drop_len);
                    int len2 = snprintf(create_seq_buf, sizeof(create_seq_buf), "CREATE SEQUENCE seq_%s_%s START 1", table, col);
                    if (len2 > 0 && (size_t)len2 < sizeof(create_seq_buf)) {
                        size_t create_seq_len = (size_t)len2 + 1;
                        char* create_seq_copy = (char*)vox_mpool_alloc(mp, create_seq_len);
                        if (create_seq_copy) {
                            memcpy(create_seq_copy, create_seq_buf, create_seq_len);
                            orm_duckdb_seq_ctx_t* dctx = (orm_duckdb_seq_ctx_t*)vox_mpool_alloc(mp, sizeof(orm_duckdb_seq_ctx_t));
                            if (dctx) {
                                memset(dctx, 0, sizeof(*dctx));
                                dctx->create_ctx = ctx;
                                dctx->table = table;
                                dctx->col = col;
                                dctx->drop_seq_sql = drop_seq_copy;
                                dctx->create_seq_sql = create_seq_copy;
                                dctx->step = 0;
                                int ret = vox_db_exec_async(conn, drop_seq_copy, NULL, 0, orm_duckdb_seq_step_done, dctx);
                                if (ret == 0) return 0;
                                vox_mpool_free(mp, dctx);
                                vox_mpool_free(mp, create_seq_copy);
                            } else {
                                vox_mpool_free(mp, create_seq_copy);
                            }
                        }
                        vox_mpool_free(mp, drop_seq_copy);
                    } else {
                        vox_mpool_free(mp, drop_seq_copy);
                    }
                }
            }
            /* 若 DuckDB sequence 路径失败则回退到直接 CREATE TABLE（会失败但保持原有错误） */
        }
    }

    int ret = vox_db_exec_async(conn, sql_copy, NULL, 0, orm_create_table_then_index_done, ctx);
    if (ret != 0) {
        vox_mpool_free(mp, sql_copy);
        vox_mpool_free(mp, ctx);
        return -1;
    }
    return 0;
}

int vox_orm_drop_table(vox_db_conn_t* conn, const char* table) {
    if (!conn || !table) return -1;
    char sql[VOX_ORM_SQL_BUF_SIZE];
    int len = snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS %s", table);
    if (len < 0 || (size_t)len >= sizeof(sql)) return -1;
    int64_t aff = 0;
    return vox_db_exec(conn, sql, NULL, 0, &aff);
}

static void orm_drop_table_done(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    orm_exec_ctx_t* ctx = (orm_exec_ctx_t*)user_data;
    vox_orm_exec_cb cb = ctx->cb;
    void* ud = ctx->user_data;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (ctx->sql_copy) {
        vox_mpool_free(mp, ctx->sql_copy);
        ctx->sql_copy = NULL;
    }
    vox_mpool_free(mp, ctx);
    if (cb) cb(conn, status, affected_rows, ud);
}

int vox_orm_drop_table_async(vox_db_conn_t* conn,
                              const char* table,
                              vox_orm_exec_cb cb,
                              void* user_data)
{
    if (!conn || !table || !cb) return -1;
    char sql[VOX_ORM_SQL_BUF_SIZE];
    int len = snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS %s", table);
    if (len < 0 || (size_t)len >= sizeof(sql)) return -1;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (!mp) return -1;
    size_t sql_len = (size_t)len + 1;
    char* sql_copy = (char*)vox_mpool_alloc(mp, sql_len);
    if (!sql_copy) return -1;
    memcpy(sql_copy, sql, sql_len);
    orm_exec_ctx_t* ctx = (orm_exec_ctx_t*)vox_mpool_alloc(mp, sizeof(orm_exec_ctx_t));
    if (!ctx) {
        vox_mpool_free(mp, sql_copy);
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->conn = conn;
    ctx->cb = cb;
    ctx->user_data = user_data;
    ctx->sql_copy = sql_copy;
    int ret = vox_db_exec_async(conn, sql_copy, NULL, 0, orm_drop_table_done, ctx);
    if (ret != 0) {
        vox_mpool_free(mp, sql_copy);
        vox_mpool_free(mp, ctx);
        return -1;
    }
    return 0;
}

/* ========== 索引：CREATE INDEX（按驱动：MySQL 无 IF NOT EXISTS） ========== */

static int orm_build_create_index_sql(char* buf, size_t buf_size,
                                      vox_db_driver_t driver,
                                      const char* table,
                                      const char* index_name,
                                      const char* const* columns,
                                      size_t ncols,
                                      int is_unique)
{
    int len;
    int use_if_not_exists = (driver != VOX_DB_DRIVER_MYSQL);
    if (use_if_not_exists)
        len = snprintf(buf, buf_size, "CREATE %sINDEX IF NOT EXISTS %s ON %s (",
                       is_unique ? "UNIQUE " : "", index_name, table);
    else
        len = snprintf(buf, buf_size, "CREATE %sINDEX %s ON %s (",
                       is_unique ? "UNIQUE " : "", index_name, table);
    if (len < 0 || (size_t)len >= buf_size) return -1;
    size_t off = (size_t)len;
    for (size_t i = 0; i < ncols; i++) {
        if (i) { buf[off++] = ','; if (off >= buf_size) return -1; }
        len = snprintf(buf + off, buf_size - off, "%s", columns[i] ? columns[i] : "");
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
        off += (size_t)len;
    }
    buf[off++] = ')';
    if (off >= buf_size) return -1;
    buf[off] = '\0';
    return 0;
}

/* ========== Insert ========== */

int vox_orm_insert(vox_db_conn_t* conn,
                   const char* table,
                   const vox_orm_field_t* fields,
                   size_t nfields,
                   const void* row_struct,
                   int64_t* out_affected)
{
    if (!conn || !table || !fields || nfields == 0 || !row_struct) return -1;

    char sql[VOX_ORM_SQL_BUF_SIZE];
    size_t nparams = 0;
    if (orm_build_insert_sql(sql, sizeof(sql), vox_db_get_driver(conn), table, fields, nfields, 1, &nparams) != 0)
        return -1;

    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (!mp) return -1;
    vox_db_value_t* params = (vox_db_value_t*)vox_mpool_alloc(mp, nparams * sizeof(vox_db_value_t));
    if (!params) return -1;

    size_t actual = 0;
    orm_struct_to_params(row_struct, fields, nfields, params, &actual, 1);
    if (actual != nparams) {
        vox_mpool_free(mp, params);
        return -1;
    }

    int64_t aff = 0;
    int ret = vox_db_exec(conn, sql, params, nparams, &aff);
    vox_mpool_free(mp, params);
    if (ret != 0) return -1;
    if (out_affected) *out_affected = aff;
    return 0;
}

static void orm_insert_done(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    orm_exec_ctx_t* ctx = (orm_exec_ctx_t*)user_data;
    vox_orm_exec_cb cb = ctx->cb;
    void* ud = ctx->user_data;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (ctx->sql_copy) { vox_mpool_free(mp, ctx->sql_copy); ctx->sql_copy = NULL; }
    if (ctx->params) { vox_mpool_free(mp, ctx->params); ctx->params = NULL; }
    vox_mpool_free(mp, ctx);
    if (cb) cb(conn, status, affected_rows, ud);
}

int vox_orm_insert_async(vox_db_conn_t* conn,
                         const char* table,
                         const vox_orm_field_t* fields,
                         size_t nfields,
                         const void* row_struct,
                         vox_orm_exec_cb cb,
                         void* user_data)
{
    if (!conn || !table || !fields || nfields == 0 || !row_struct) return -1;

    char sql[VOX_ORM_SQL_BUF_SIZE];
    size_t nparams = 0;
    if (orm_build_insert_sql(sql, sizeof(sql), vox_db_get_driver(conn), table, fields, nfields, 1, &nparams) != 0)
        return -1;

    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (!mp) return -1;
    vox_db_value_t* params = (vox_db_value_t*)vox_mpool_alloc(mp, nparams * sizeof(vox_db_value_t));
    if (!params) return -1;
    size_t actual = 0;
    orm_struct_to_params(row_struct, fields, nfields, params, &actual, 1);
    if (actual != nparams) {
        vox_mpool_free(mp, params);
        return -1;
    }

    orm_exec_ctx_t* ctx = (orm_exec_ctx_t*)vox_mpool_alloc(mp, sizeof(orm_exec_ctx_t));
    if (!ctx) {
        vox_mpool_free(mp, params);
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->conn = conn;
    ctx->cb = cb;
    ctx->user_data = user_data;
    ctx->params = params;
    ctx->nparams = nparams;
    size_t sql_len = strlen(sql) + 1;
    ctx->sql_copy = (char*)vox_mpool_alloc(mp, sql_len);
    if (!ctx->sql_copy) {
        vox_mpool_free(mp, ctx);
        vox_mpool_free(mp, params);
        return -1;
    }
    memcpy(ctx->sql_copy, sql, sql_len);

    int ret = vox_db_exec_async(conn, ctx->sql_copy, params, nparams, orm_insert_done, ctx);
    if (ret != 0) {
        vox_mpool_free(mp, ctx);
        vox_mpool_free(mp, params);
        return -1;
    }
    return 0;
}

/* ========== Update ========== */

int vox_orm_update(vox_db_conn_t* conn,
                   const char* table,
                   const vox_orm_field_t* fields,
                   size_t nfields,
                   const void* row_struct,
                   const char* where_clause,
                   const vox_db_value_t* where_params,
                   size_t n_where_params,
                   int64_t* out_affected)
{
    if (!conn || !table || !fields || nfields == 0 || !row_struct || !where_clause) return -1;

    char sql[VOX_ORM_SQL_BUF_SIZE];
    size_t total_params = 0;
    if (orm_build_update_sql(sql, sizeof(sql), vox_db_get_driver(conn), table, fields, nfields, where_clause, n_where_params, &total_params) != 0)
        return -1;

    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (!mp) return -1;
    vox_db_value_t* params = (vox_db_value_t*)vox_mpool_alloc(mp, total_params * sizeof(vox_db_value_t));
    if (!params) return -1;

    size_t nset = 0;
    orm_struct_to_params(row_struct, fields, nfields, params, &nset, 0);
    if (nset != nfields) {
        vox_mpool_free(mp, params);
        return -1;
    }
    if (where_params && n_where_params > 0)
        memcpy(params + nfields, where_params, n_where_params * sizeof(vox_db_value_t));

    int64_t aff = 0;
    int ret = vox_db_exec(conn, sql, params, total_params, &aff);
    vox_mpool_free(mp, params);
    if (ret != 0) return -1;
    if (out_affected) *out_affected = aff;
    return 0;
}

static void orm_update_done(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    orm_exec_ctx_t* ctx = (orm_exec_ctx_t*)user_data;
    vox_orm_exec_cb cb = ctx->cb;
    void* ud = ctx->user_data;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (ctx->sql_copy) { vox_mpool_free(mp, ctx->sql_copy); ctx->sql_copy = NULL; }
    if (ctx->params) { vox_mpool_free(mp, ctx->params); ctx->params = NULL; }
    vox_mpool_free(mp, ctx);
    if (cb) cb(conn, status, affected_rows, ud);
}

int vox_orm_update_async(vox_db_conn_t* conn,
                         const char* table,
                         const vox_orm_field_t* fields,
                         size_t nfields,
                         const void* row_struct,
                         const char* where_clause,
                         const vox_db_value_t* where_params,
                         size_t n_where_params,
                         vox_orm_exec_cb cb,
                         void* user_data)
{
    if (!conn || !table || !fields || nfields == 0 || !row_struct || !where_clause) return -1;

    char sql[VOX_ORM_SQL_BUF_SIZE];
    size_t total_params = 0;
    if (orm_build_update_sql(sql, sizeof(sql), vox_db_get_driver(conn), table, fields, nfields, where_clause, n_where_params, &total_params) != 0)
        return -1;

    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (!mp) return -1;
    vox_db_value_t* params = (vox_db_value_t*)vox_mpool_alloc(mp, total_params * sizeof(vox_db_value_t));
    if (!params) return -1;
    size_t nset = 0;
    orm_struct_to_params(row_struct, fields, nfields, params, &nset, 0);
    if (nset != nfields) {
        vox_mpool_free(mp, params);
        return -1;
    }
    if (where_params && n_where_params > 0)
        memcpy(params + nfields, where_params, n_where_params * sizeof(vox_db_value_t));

    orm_exec_ctx_t* ctx = (orm_exec_ctx_t*)vox_mpool_alloc(mp, sizeof(orm_exec_ctx_t));
    if (!ctx) {
        vox_mpool_free(mp, params);
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->conn = conn;
    ctx->cb = cb;
    ctx->user_data = user_data;
    ctx->params = params;
    ctx->nparams = total_params;
    size_t sql_len = strlen(sql) + 1;
    ctx->sql_copy = (char*)vox_mpool_alloc(mp, sql_len);
    if (!ctx->sql_copy) {
        vox_mpool_free(mp, ctx);
        vox_mpool_free(mp, params);
        return -1;
    }
    memcpy(ctx->sql_copy, sql, sql_len);

    int ret = vox_db_exec_async(conn, ctx->sql_copy, params, total_params, orm_update_done, ctx);
    if (ret != 0) {
        vox_mpool_free(mp, ctx);
        vox_mpool_free(mp, params);
        return -1;
    }
    return 0;
}

/* ========== Delete ========== */

/* 生成 DELETE SQL：PG 时 WHERE 中 ? 写成 $1,$2,... */
static int orm_build_delete_sql(char* buf, size_t buf_size,
                                 vox_db_driver_t driver,
                                 const char* table,
                                 const char* where_clause)
{
    int pg = orm_placeholder_style(driver);
    int len = snprintf(buf, buf_size, "DELETE FROM %s WHERE ", table);
    if (len < 0 || (size_t)len >= buf_size) return -1;
    size_t off = (size_t)len;
    if (pg) {
        size_t p = 1;
        for (const char* s = where_clause; *s && off < buf_size; s++) {
            if (*s == '?') {
                len = snprintf(buf + off, buf_size - off, "$%zu", p++);
                if (len < 0 || off + (size_t)len >= buf_size) return -1;
                off += (size_t)len;
            } else {
                buf[off++] = *s;
            }
        }
    } else {
        len = snprintf(buf + off, buf_size - off, "%s", where_clause);
        if (len < 0 || off + (size_t)len >= buf_size) return -1;
    }
    return 0;
}

int vox_orm_delete(vox_db_conn_t* conn,
                   const char* table,
                   const char* where_clause,
                   const vox_db_value_t* where_params,
                   size_t n_where_params,
                   int64_t* out_affected)
{
    if (!conn || !table || !where_clause) return -1;

    char sql[VOX_ORM_SQL_BUF_SIZE];
    if (orm_build_delete_sql(sql, sizeof(sql), vox_db_get_driver(conn), table, where_clause) != 0)
        return -1;

    int64_t aff = 0;
    int ret = vox_db_exec(conn, sql, where_params, n_where_params, &aff);
    if (ret != 0) return -1;
    if (out_affected) *out_affected = aff;
    return 0;
}

static void orm_delete_done(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    orm_exec_ctx_t* ctx = (orm_exec_ctx_t*)user_data;
    vox_orm_exec_cb cb = ctx->cb;
    void* ud = ctx->user_data;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (ctx->sql_copy) { vox_mpool_free(mp, ctx->sql_copy); ctx->sql_copy = NULL; }
    vox_mpool_free(mp, ctx);
    if (cb) cb(conn, status, affected_rows, ud);
}

int vox_orm_delete_async(vox_db_conn_t* conn,
                         const char* table,
                         const char* where_clause,
                         const vox_db_value_t* where_params,
                         size_t n_where_params,
                         vox_orm_exec_cb cb,
                         void* user_data)
{
    if (!conn || !table || !where_clause) return -1;

    char sql[VOX_ORM_SQL_BUF_SIZE];
    if (orm_build_delete_sql(sql, sizeof(sql), vox_db_get_driver(conn), table, where_clause) != 0)
        return -1;

    size_t sql_len = strlen(sql) + 1;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (!mp) return -1;
    char* sql_copy = (char*)vox_mpool_alloc(mp, sql_len);
    if (!sql_copy) return -1;
    memcpy(sql_copy, sql, sql_len);
    orm_exec_ctx_t* ctx = (orm_exec_ctx_t*)vox_mpool_alloc(mp, sizeof(orm_exec_ctx_t));
    if (!ctx) {
        vox_mpool_free(mp, sql_copy);
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->conn = conn;
    ctx->cb = cb;
    ctx->user_data = user_data;
    ctx->sql_copy = sql_copy;

    int ret = vox_db_exec_async(conn, sql_copy, where_params, n_where_params, orm_delete_done, ctx);
    if (ret != 0) {
        vox_mpool_free(mp, sql_copy);
        vox_mpool_free(mp, ctx);
        return -1;
    }
    return 0;
}

/* ========== Select 单行 ========== */

typedef struct {
    void* row_struct;
    size_t row_size;
    const vox_orm_field_t* fields;
    size_t nfields;
    int* out_found;
} orm_select_one_ctx_t;

static void orm_select_one_row_cb(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    orm_select_one_ctx_t* ctx = (orm_select_one_ctx_t*)user_data;
    if (ctx->out_found && *ctx->out_found) return; /* 已填过 */
    orm_row_to_struct(row, ctx->row_struct, ctx->row_size, ctx->fields, ctx->nfields);
    if (ctx->out_found) *ctx->out_found = 1;
    (void)conn;
}

int vox_orm_select_one(vox_db_conn_t* conn,
                       const char* table,
                       const vox_orm_field_t* fields,
                       size_t nfields,
                       void* row_struct,
                       size_t row_size,
                       const char* where_clause,
                       const vox_db_value_t* where_params,
                       size_t n_where_params,
                       int* out_found)
{
    if (!conn || !table || !fields || nfields == 0 || !row_struct || !where_clause) return -1;
    if (out_found) *out_found = 0;

    char sql[VOX_ORM_SQL_BUF_SIZE];
    if (orm_build_select_sql(sql, sizeof(sql), vox_db_get_driver(conn), table, fields, nfields, where_clause, 1) != 0)
        return -1;

    orm_select_one_ctx_t ctx = {
        .row_struct = row_struct,
        .row_size = row_size,
        .fields = fields,
        .nfields = nfields,
        .out_found = out_found,
    };
    int64_t row_count = 0;
    int ret = vox_db_query(conn, sql, where_params, n_where_params, orm_select_one_row_cb, &ctx, &row_count);
    if (ret != 0) return -1;
    return 0;
}

typedef struct {
    vox_db_conn_t* conn;
    void* row_struct;        /* 首行时从 mpool 分配 */
    size_t row_size;
    const vox_orm_field_t* fields;
    size_t nfields;
    vox_orm_select_one_cb cb;
    void* user_data;
    int filled;
    char* sql_copy;          /* 异步时 SQL 的 mpool 副本 */
} orm_select_one_async_ctx_t;

static void orm_select_one_async_row_cb(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    orm_select_one_async_ctx_t* ctx = (orm_select_one_async_ctx_t*)user_data;
    if (ctx->filled) return;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    ctx->row_struct = vox_mpool_alloc(mp, ctx->row_size);
    if (!ctx->row_struct) return;
    orm_row_to_struct(row, ctx->row_struct, ctx->row_size, ctx->fields, ctx->nfields);
    ctx->filled = 1;
}

static void orm_select_one_async_done_cb(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    orm_select_one_async_ctx_t* ctx = (orm_select_one_async_ctx_t*)user_data;
    vox_orm_select_one_cb cb = ctx->cb;
    void* ud = ctx->user_data;
    void* row = ctx->filled ? ctx->row_struct : NULL;
    /* 先回调用户，以便在 ctx 释放前拷贝 row 等；再释放 ctx（含 row_struct） */
    if (cb) cb(conn, status, row, ud);
    (void)row_count;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (ctx->sql_copy) { vox_mpool_free(mp, ctx->sql_copy); ctx->sql_copy = NULL; }
    vox_mpool_free(mp, ctx);
}

int vox_orm_select_one_async(vox_db_conn_t* conn,
                              const char* table,
                              const vox_orm_field_t* fields,
                              size_t nfields,
                              size_t row_size,
                              const char* where_clause,
                              const vox_db_value_t* where_params,
                              size_t n_where_params,
                              vox_orm_select_one_cb cb,
                              void* user_data)
{
    if (!conn || !table || !fields || nfields == 0 || !where_clause || !cb) return -1;

    char sql[VOX_ORM_SQL_BUF_SIZE];
    if (orm_build_select_sql(sql, sizeof(sql), vox_db_get_driver(conn), table, fields, nfields, where_clause, 1) != 0)
        return -1;

    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (!mp) return -1;
    orm_select_one_async_ctx_t* ctx = (orm_select_one_async_ctx_t*)vox_mpool_alloc(mp, sizeof(orm_select_one_async_ctx_t));
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->conn = conn;
    ctx->row_size = row_size;
    ctx->fields = fields;
    ctx->nfields = nfields;
    ctx->cb = cb;
    ctx->user_data = user_data;
    size_t sql_len = strlen(sql) + 1;
    ctx->sql_copy = (char*)vox_mpool_alloc(mp, sql_len);
    if (!ctx->sql_copy) {
        vox_mpool_free(mp, ctx);
        return -1;
    }
    memcpy(ctx->sql_copy, sql, sql_len);

    int ret = vox_db_query_async(conn, ctx->sql_copy, where_params, n_where_params, orm_select_one_async_row_cb, orm_select_one_async_done_cb, ctx);
    if (ret != 0) {
        vox_mpool_free(mp, ctx->sql_copy);
        vox_mpool_free(mp, ctx);
        return -1;
    }
    return 0;
}

/* ========== Select 多行 ========== */

typedef struct {
    vox_vector_t* list;
    size_t row_size;
    const vox_orm_field_t* fields;
    size_t nfields;
    vox_db_conn_t* conn;
    vox_orm_select_done_cb done_cb;
    void* user_data;
    char* sql_copy;          /* 异步时 SQL 的 mpool 副本 */
} orm_select_many_ctx_t;

static void orm_select_many_row_cb(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    orm_select_many_ctx_t* ctx = (orm_select_many_ctx_t*)user_data;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    void* one = vox_mpool_alloc(mp, ctx->row_size);
    if (!one) return;
    orm_row_to_struct(row, one, ctx->row_size, ctx->fields, ctx->nfields);
    vox_vector_push(ctx->list, one);
}

static void orm_select_many_done_cb(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    orm_select_many_ctx_t* ctx = (orm_select_many_ctx_t*)user_data;
    vox_orm_select_done_cb cb = ctx->done_cb;
    void* ud = ctx->user_data;
    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (ctx->sql_copy) { vox_mpool_free(mp, ctx->sql_copy); ctx->sql_copy = NULL; }
    vox_mpool_free(mp, ctx);
    if (cb) cb(conn, status, row_count, ud);
}

int vox_orm_select(vox_db_conn_t* conn,
                   const char* table,
                   const vox_orm_field_t* fields,
                   size_t nfields,
                   size_t row_size,
                   vox_vector_t* out_list,
                   int64_t* out_row_count,
                   const char* where_clause,
                   const vox_db_value_t* where_params,
                   size_t n_where_params)
{
    if (!conn || !table || !fields || nfields == 0 || !out_list || !where_clause) return -1;

    char sql[VOX_ORM_SQL_BUF_SIZE];
    if (orm_build_select_sql(sql, sizeof(sql), vox_db_get_driver(conn), table, fields, nfields, where_clause, 0) != 0)
        return -1;

    orm_select_many_ctx_t ctx = {
        .list = out_list,
        .row_size = row_size,
        .fields = fields,
        .nfields = nfields,
    };
    int64_t row_count = 0;
    int ret = vox_db_query(conn, sql, where_params, n_where_params, orm_select_many_row_cb, &ctx, &row_count);
    if (ret != 0) return -1;
    if (out_row_count) *out_row_count = row_count;
    return 0;
}

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
                         void* user_data)
{
    if (!conn || !table || !fields || nfields == 0 || !out_list || !where_clause || !done_cb) return -1;

    char sql[VOX_ORM_SQL_BUF_SIZE];
    if (orm_build_select_sql(sql, sizeof(sql), vox_db_get_driver(conn), table, fields, nfields, where_clause, 0) != 0)
        return -1;

    vox_mpool_t* mp = vox_db_get_mpool(conn);
    if (!mp) return -1;
    orm_select_many_ctx_t* ctx = (orm_select_many_ctx_t*)vox_mpool_alloc(mp, sizeof(orm_select_many_ctx_t));
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->list = out_list;
    ctx->row_size = row_size;
    ctx->fields = fields;
    ctx->nfields = nfields;
    ctx->conn = conn;
    ctx->done_cb = done_cb;
    ctx->user_data = user_data;
    size_t sql_len = strlen(sql) + 1;
    ctx->sql_copy = (char*)vox_mpool_alloc(mp, sql_len);
    if (!ctx->sql_copy) {
        vox_mpool_free(mp, ctx);
        return -1;
    }
    memcpy(ctx->sql_copy, sql, sql_len);

    int ret = vox_db_query_async(conn, ctx->sql_copy, where_params, n_where_params, orm_select_many_row_cb, orm_select_many_done_cb, ctx);
    if (ret != 0) {
        vox_mpool_free(mp, ctx->sql_copy);
        vox_mpool_free(mp, ctx);
        return -1;
    }
    return 0;
}
