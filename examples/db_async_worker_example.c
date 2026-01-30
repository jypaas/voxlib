/*
 * db_async_worker_example.c - 异步 DB 示例（单线程等待完成）
 *
 * 特点：
 * - SQLite/DuckDB 使用 use_loop_thread_for_async，异步任务在 loop 线程执行
 * - 等待时需驱动事件循环（vox_loop_run(loop, VOX_RUN_ONCE)），否则回调不会触发
 * - 适合：单线程脚本式流程，或测试代码
 */

#include "../vox_loop.h"
#include "../vox_log.h"
#include "../vox_thread.h"
#include "../db/vox_db.h"

#include <stdio.h>

typedef struct {
    volatile int done;
    volatile int status;
} wait_t;

static void on_exec(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    (void)conn;
    (void)affected_rows;
    wait_t* w = (wait_t*)user_data;
    w->status = status;
    w->done = 1;
}

static void on_row(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    (void)user_data;
    printf("row: ");
    for (size_t i = 0; i < row->column_count; i++) {
        const vox_db_value_t* v = &row->values[i];
        if (i) printf(", ");
        switch (v->type) {
            case VOX_DB_TYPE_I64:
                printf("%lld", (long long)v->u.i64);
                break;
            case VOX_DB_TYPE_TEXT:
                printf("%.*s", (int)v->u.text.len, v->u.text.ptr ? v->u.text.ptr : "");
                break;
            case VOX_DB_TYPE_NULL:
                printf("NULL");
                break;
            default:
                printf("?");
                break;
        }
    }
    printf("\n");
}

static void on_done(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    (void)conn;
    (void)row_count;
    wait_t* w = (wait_t*)user_data;
    w->status = status;
    w->done = 1;
}

/* SQLite/DuckDB 使用 use_loop_thread_for_async，需驱动事件循环 */
static int wait_until(vox_loop_t* loop, wait_t* w, uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (!w->done && waited < timeout_ms) {
        vox_loop_run(loop, VOX_RUN_ONCE);
        vox_thread_sleep(1);
        waited++;
    }
    return w->done ? 0 : -1;
}

int main(void) {
    vox_log_set_level(VOX_LOG_INFO);
    vox_loop_t* loop = vox_loop_create();
    if (!loop) return 1;

    vox_db_conn_t* db = vox_db_connect(loop, VOX_DB_DRIVER_SQLITE3, ":memory:");
    if (!db) db = vox_db_connect(loop, VOX_DB_DRIVER_DUCKDB, ":memory:");
    if (!db) {
        VOX_LOG_ERROR("no driver enabled or connect failed");
        vox_loop_destroy(loop);
        return 1;
    }

    /* create */
    {
        wait_t w = {0};
        if (vox_db_exec_async(db, "CREATE TABLE t(id INTEGER, name VARCHAR);", NULL, 0, on_exec, &w) != 0 ||
            wait_until(loop, &w, 5000) != 0 || w.status != 0) {
            VOX_LOG_ERROR("create failed: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
            vox_db_disconnect(db);
            vox_loop_destroy(loop);
            return 1;
        }
    }

    /* insert with params (sqlite/duckdb 支持 ? 参数) */
    {
        wait_t w = {0};
        vox_db_value_t params[2];
        params[0].type = VOX_DB_TYPE_I64;
        params[0].u.i64 = 1;
        params[1].type = VOX_DB_TYPE_TEXT;
        params[1].u.text.ptr = "alice";
        params[1].u.text.len = 5;
        if (vox_db_exec_async(db, "INSERT INTO t VALUES(?, ?);", params, 2, on_exec, &w) != 0 ||
            wait_until(loop, &w, 5000) != 0 || w.status != 0) {
            VOX_LOG_ERROR("insert failed: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
            vox_db_disconnect(db);
            vox_loop_destroy(loop);
            return 1;
        }
    }

    /* query streaming */
    {
        wait_t w = {0};
        if (vox_db_query_async(db, "SELECT id, name FROM t;", NULL, 0, on_row, on_done, &w) != 0 ||
            wait_until(loop, &w, 5000) != 0 || w.status != 0) {
            VOX_LOG_ERROR("query failed: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
            vox_db_disconnect(db);
            vox_loop_destroy(loop);
            return 1;
        }
    }

    vox_db_disconnect(db);
    vox_loop_destroy(loop);
    return 0;
}

