/*
 * db_example.c - Vox DB 抽象层示例
 *
 * 说明：
 * - 只依赖 `db/vox_db.h`，不直接依赖 sqlite3/duckdb 头文件
 * - 运行效果取决于你是否在构建时启用了对应驱动（VOX_USE_SQLITE3/VOX_USE_DUCKDB/VOX_USE_PGSQL/VOX_USE_MYSQL）
 */

#include "../vox_loop.h"
#include "../vox_log.h"
#include "../vox_thread.h"

#include "../db/vox_db.h"

#include <stdio.h>

typedef struct {
    volatile int done;
    volatile int status;
    int64_t rows;
} wait_t;

static void on_exec(vox_db_conn_t* conn, int status, int64_t affected, void* user_data) {
    (void)conn;
    (void)affected;
    wait_t* w = (wait_t*)user_data;
    w->status = status;
    w->done = 1;
}

static void on_row(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    (void)user_data;
    printf("row: ");
    for (size_t i = 0; i < row->column_count; i++) {
        const char* name = row->column_names ? row->column_names[i] : "?";
        const vox_db_value_t* v = &row->values[i];
        printf("%s=", name ? name : "?");
        switch (v->type) {
            case VOX_DB_TYPE_NULL:
                printf("NULL");
                break;
            case VOX_DB_TYPE_I64:
                printf("%lld", (long long)v->u.i64);
                break;
            case VOX_DB_TYPE_U64:
                printf("%llu", (unsigned long long)v->u.u64);
                break;
            case VOX_DB_TYPE_F64:
                printf("%f", v->u.f64);
                break;
            case VOX_DB_TYPE_BOOL:
                printf("%s", v->u.boolean ? "true" : "false");
                break;
            case VOX_DB_TYPE_TEXT:
                printf("%.*s", (int)v->u.text.len, v->u.text.ptr ? v->u.text.ptr : "");
                break;
            case VOX_DB_TYPE_BLOB:
                printf("<blob %zu bytes>", v->u.blob.len);
                break;
            default:
                printf("?");
                break;
        }
        if (i + 1 < row->column_count) printf(", ");
    }
    printf("\n");
}

static void on_done(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    (void)conn;
    wait_t* w = (wait_t*)user_data;
    w->status = status;
    w->rows = row_count;
    w->done = 1;
}

/* SQLite/DuckDB 等使用 use_loop_thread_for_async，回调在 loop 线程执行，需驱动事件循环 */
static int wait_until_done(vox_loop_t* loop, wait_t* w, uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (!w->done && waited < timeout_ms) {
        vox_loop_run(loop, VOX_RUN_ONCE);
        vox_thread_sleep(1);
        waited += 1;
    }
    return w->done ? 0 : -1;
}

int main(void) {
    vox_log_set_level(VOX_LOG_INFO);

    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "vox_loop_create failed\n");
        return 1;
    }

    /* 依次尝试 sqlite3 / duckdb（本地可用性取决于构建） */
    vox_db_conn_t* db = NULL;
    vox_db_driver_t driver = VOX_DB_DRIVER_SQLITE3;

    db = vox_db_connect(loop, VOX_DB_DRIVER_SQLITE3, ":memory:");
    if (!db) {
        driver = VOX_DB_DRIVER_DUCKDB;
        db = vox_db_connect(loop, VOX_DB_DRIVER_DUCKDB, ":memory:");
    }

    if (!db) {
        VOX_LOG_ERROR("no DB driver enabled or connect failed");
        vox_loop_destroy(loop);
        return 1;
    }

    VOX_LOG_INFO("connected with driver=%d", (int)driver);

    /* create table */
    {
        wait_t w = {0};
        const char* sql = "CREATE TABLE t(id INTEGER, name VARCHAR);";
        if (vox_db_exec_async(db, sql, NULL, 0, on_exec, &w) != 0 || wait_until_done(loop, &w, 5000) != 0 || w.status != 0) {
            VOX_LOG_ERROR("create table failed: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
            vox_db_disconnect(db);
            vox_loop_destroy(loop);
            return 1;
        }
    }

    /* insert */
    {
        wait_t w = {0};
        vox_db_value_t params[2];
        params[0].type = VOX_DB_TYPE_I64;
        params[0].u.i64 = 1;
        params[1].type = VOX_DB_TYPE_TEXT;
        params[1].u.text.ptr = "alice";
        params[1].u.text.len = 5;

        /* sqlite/duckdb 支持 ? 参数；其他驱动可能失败 */
        const char* sql = "INSERT INTO t VALUES(?, ?);";
        if (vox_db_exec_async(db, sql, params, 2, on_exec, &w) != 0 || wait_until_done(loop, &w, 5000) != 0 || w.status != 0) {
            VOX_LOG_ERROR("insert failed: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
            vox_db_disconnect(db);
            vox_loop_destroy(loop);
            return 1;
        }
    }

    /* query */
    {
        wait_t w = {0};
        const char* sql = "SELECT id, name FROM t;";
        if (vox_db_query_async(db, sql, NULL, 0, on_row, on_done, &w) != 0 || wait_until_done(loop, &w, 5000) != 0 || w.status != 0) {
            VOX_LOG_ERROR("query failed: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
            vox_db_disconnect(db);
            vox_loop_destroy(loop);
            return 1;
        }
        VOX_LOG_INFO("rows=%lld", (long long)w.rows);
    }

    vox_db_disconnect(db);
    vox_loop_destroy(loop);
    return 0;
}

