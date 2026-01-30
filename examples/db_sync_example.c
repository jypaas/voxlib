/*
 * db_sync_example.c - 同步 DB 示例（阻塞当前线程）
 *
 * 场景：
 * - 你在自己的工作线程里做 DB 操作
 * - 或者在工具/一次性脚本场景直接同步调用
 *
 * 注意：不要在网络 IO/事件循环线程里长时间阻塞。
 */

#include "../vox_loop.h"
#include "../vox_log.h"
#include "../db/vox_db.h"

#include <stdio.h>

static void print_row(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    (void)user_data;
    printf("row(sync): ");
    for (size_t i = 0; i < row->column_count; i++) {
        const vox_db_value_t* v = &row->values[i];
        if (i) printf(", ");
        if (v->type == VOX_DB_TYPE_I64) {
            printf("%lld", (long long)v->u.i64);
        } else if (v->type == VOX_DB_TYPE_TEXT) {
            printf("%.*s", (int)v->u.text.len, v->u.text.ptr ? v->u.text.ptr : "");
        } else if (v->type == VOX_DB_TYPE_NULL) {
            printf("NULL");
        } else {
            printf("?");
        }
    }
    printf("\n");
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

    int64_t affected = 0;
    if (vox_db_exec(db, "CREATE TABLE t(id INTEGER, name VARCHAR);", NULL, 0, &affected) != 0) {
        VOX_LOG_ERROR("create failed: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
        vox_db_disconnect(db);
        vox_loop_destroy(loop);
        return 1;
    }

    vox_db_value_t params[2];
    params[0].type = VOX_DB_TYPE_I64;
    params[0].u.i64 = 1;
    params[1].type = VOX_DB_TYPE_TEXT;
    params[1].u.text.ptr = "alice";
    params[1].u.text.len = 5;

    if (vox_db_exec(db, "INSERT INTO t VALUES(?, ?);", params, 2, &affected) != 0) {
        VOX_LOG_ERROR("insert failed: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
        vox_db_disconnect(db);
        vox_loop_destroy(loop);
        return 1;
    }

    int64_t rows = 0;
    if (vox_db_query(db, "SELECT id, name FROM t;", NULL, 0, print_row, NULL, &rows) != 0) {
        VOX_LOG_ERROR("query failed: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
        vox_db_disconnect(db);
        vox_loop_destroy(loop);
        return 1;
    }
    VOX_LOG_INFO("rows=%lld", (long long)rows);

    vox_db_disconnect(db);
    vox_loop_destroy(loop);
    return 0;
}

