/*
 * db_async_loop_example.c - 异步 DB（回调切回 loop 线程触发）
 *
 * 特点：
 * - 设置 VOX_DB_CALLBACK_LOOP
 * - 需要跑 vox_loop_run 才能处理回调队列
 * - done_cb 在 loop 线程触发，可直接操作同线程对象（例如 HTTP 响应构建）
 */

#include "../vox_loop.h"
#include "../vox_log.h"
#include "../db/vox_db.h"

#include <stdio.h>

typedef struct {
    vox_loop_t* loop;
    vox_db_conn_t* db;
    int phase;
    /* 注意：异步提交会跨线程访问 params，必须保证生命周期足够长 */
    vox_db_value_t insert_params[2];
} app_t;

static void on_exec(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data);
static void on_row(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data);
static void on_done(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data);

static void start_work(vox_loop_t* loop, void* user_data) {
    (void)loop;
    app_t* app = (app_t*)user_data;
    if (!app || !app->db) return;

    /* phase 0: create */
    app->phase = 0;
    if (vox_db_exec_async(app->db, "CREATE TABLE t(id INTEGER, name VARCHAR);", NULL, 0, on_exec, app) != 0) {
        VOX_LOG_ERROR("exec_async(create) submit failed");
        vox_loop_stop(app->loop);
    }
}

static void on_exec(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    (void)conn;
    (void)affected_rows;
    app_t* app = (app_t*)user_data;
    if (!app) return;

    if (status != 0) {
        VOX_LOG_ERROR("exec failed: %s", vox_db_last_error(app->db) ? vox_db_last_error(app->db) : "(no error)");
        vox_loop_stop(app->loop);
        return;
    }

    if (app->phase == 0) {
        /* phase 1: insert */
        app->phase = 1;
        if (vox_db_exec_async(app->db, "INSERT INTO t VALUES(?, ?);", app->insert_params, 2, on_exec, app) != 0) {
            VOX_LOG_ERROR("exec_async(insert) submit failed");
            vox_loop_stop(app->loop);
        }
        return;
    }

    if (app->phase == 1) {
        /* phase 2: query */
        app->phase = 2;
        if (vox_db_query_async(app->db, "SELECT id, name FROM t;", NULL, 0, on_row, on_done, app) != 0) {
            VOX_LOG_ERROR("query submit failed");
            vox_loop_stop(app->loop);
        }
        return;
    }
}

static void on_row(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    (void)user_data;
    printf("row(loop): ");
    for (size_t i = 0; i < row->column_count; i++) {
        const vox_db_value_t* v = &row->values[i];
        if (i) printf(", ");
        if (v->type == VOX_DB_TYPE_I64) {
            printf("%lld", (long long)v->u.i64);
        } else if (v->type == VOX_DB_TYPE_TEXT) {
            printf("%.*s", (int)v->u.text.len, v->u.text.ptr ? v->u.text.ptr : "");
        } else {
            printf("?");
        }
    }
    printf("\n");
}

static void on_done(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    (void)conn;
    app_t* app = (app_t*)user_data;
    if (!app) return;
    if (status != 0) {
        VOX_LOG_ERROR("query failed: %s", vox_db_last_error(app->db) ? vox_db_last_error(app->db) : "(no error)");
    } else {
        VOX_LOG_INFO("done(loop): row_count=%lld", (long long)row_count);
    }
    vox_loop_stop(app->loop);
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

    /* 关键：回调切回 loop 线程 */
    vox_db_set_callback_mode(db, VOX_DB_CALLBACK_LOOP);

    app_t app = {0};
    app.loop = loop;
    app.db = db;
    app.phase = 0;
    /* INSERT 参数必须在异步任务完成前一直有效，放到 app 里 */
    app.insert_params[0].type = VOX_DB_TYPE_I64;
    app.insert_params[0].u.i64 = 1;
    app.insert_params[1].type = VOX_DB_TYPE_TEXT;
    app.insert_params[1].u.text.ptr = "alice";
    app.insert_params[1].u.text.len = 5;

    /* 在 loop 中启动一次性工作，然后跑 loop */
    vox_loop_queue_work_immediate(loop, start_work, &app);
    vox_loop_run(loop, VOX_RUN_DEFAULT);

    vox_db_disconnect(db);
    vox_loop_destroy(loop);
    return 0;
}

