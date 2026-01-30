/* ============================================================
 * test_db_sqlite3.c - SQLite3 DB 抽象层测试
 * ============================================================ */

#include "test_runner.h"

#ifdef VOX_USE_SQLITE3

#include "../vox_loop.h"
#include "../vox_thread.h"
#include "../db/vox_db.h"

typedef struct {
    volatile int done;
    volatile int status;
    int64_t affected;
    int64_t rows;
} wait_t;

static void exec_cb(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    (void)conn;
    wait_t* w = (wait_t*)user_data;
    w->status = status;
    w->affected = affected_rows;
    w->done = 1;
}

static void row_cb(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    wait_t* w = (wait_t*)user_data;
    w->rows++;

    TEST_ASSERT(row->column_count == 2, "列数应为2");
    TEST_ASSERT(row->values[0].type == VOX_DB_TYPE_I64, "id 类型应为 I64");
    TEST_ASSERT(row->values[0].u.i64 == 1, "id 值应为 1");
    TEST_ASSERT(row->values[1].type == VOX_DB_TYPE_TEXT, "name 类型应为 TEXT");
    TEST_ASSERT(row->values[1].u.text.len == 5, "name 长度应为 5");
    TEST_ASSERT_STR_EQ(row->values[1].u.text.ptr, "alice", "name 值应为 alice");
}

static void done_cb(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    (void)conn;
    wait_t* w = (wait_t*)user_data;
    w->status = status;
    w->rows = row_count;
    w->done = 1;
}

/* SQLite 使用 use_loop_thread_for_async，异步回调在 loop 线程执行，需驱动事件循环 */
static int wait_until(vox_loop_t* loop, wait_t* w, uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (!w->done && waited < timeout_ms) {
        vox_loop_run(loop, VOX_RUN_ONCE);
        vox_thread_sleep(1);
        waited++;
    }
    return w->done ? 0 : -1;
}

static void test_sqlite3_basic(vox_mpool_t* mpool) {
    (void)mpool;
    vox_loop_t* loop = vox_loop_create();
    TEST_ASSERT_NOT_NULL(loop, "vox_loop_create failed");

    vox_db_conn_t* db = vox_db_connect(loop, VOX_DB_DRIVER_SQLITE3, ":memory:");
    TEST_ASSERT_NOT_NULL(db, "vox_db_connect(sqlite3) failed");

    /* create table */
    {
        wait_t w = {0};
        TEST_ASSERT_EQ(vox_db_exec_async(db, "CREATE TABLE t(id INTEGER, name TEXT);", NULL, 0, exec_cb, &w), 0, "exec_async create failed");
        TEST_ASSERT_EQ(wait_until(loop, &w, 5000), 0, "wait create timeout");
        TEST_ASSERT_EQ(w.status, 0, "create status should be 0");
    }

    /* insert with params */
    {
        wait_t w = {0};
        vox_db_value_t params[2];
        params[0].type = VOX_DB_TYPE_I64;
        params[0].u.i64 = 1;
        params[1].type = VOX_DB_TYPE_TEXT;
        params[1].u.text.ptr = "alice";
        params[1].u.text.len = 5;
        TEST_ASSERT_EQ(vox_db_exec_async(db, "INSERT INTO t VALUES(?, ?);", params, 2, exec_cb, &w), 0, "exec_async insert failed");
        TEST_ASSERT_EQ(wait_until(loop, &w, 5000), 0, "wait insert timeout");
        TEST_ASSERT_EQ(w.status, 0, "insert status should be 0");
    }

    /* query */
    {
        wait_t w = {0};
        TEST_ASSERT_EQ(vox_db_query_async(db, "SELECT id, name FROM t;", NULL, 0, row_cb, done_cb, &w), 0, "query_async failed");
        TEST_ASSERT_EQ(wait_until(loop, &w, 5000), 0, "wait query timeout");
        TEST_ASSERT_EQ(w.status, 0, "query status should be 0");
        TEST_ASSERT_EQ(w.rows, 1, "row_count should be 1");
    }

    vox_db_disconnect(db);
    vox_loop_destroy(loop);
}

test_case_t test_db_sqlite3_cases[] = {
    {"basic", test_sqlite3_basic},
};

test_suite_t test_db_sqlite3_suite = {
    "db_sqlite3",
    test_db_sqlite3_cases,
    sizeof(test_db_sqlite3_cases) / sizeof(test_db_sqlite3_cases[0])
};

#endif /* VOX_USE_SQLITE3 */

