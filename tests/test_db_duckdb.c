/* ============================================================
 * test_db_duckdb.c - DuckDB DB 抽象层测试
 * ============================================================ */

#include "test_runner.h"

#ifdef VOX_USE_DUCKDB

#include "../vox_loop.h"
#include "../vox_thread.h"
#include "../db/vox_db.h"

typedef struct {
    volatile int done;
    volatile int status;
    int64_t rows;
} wait_t;

static void exec_cb(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    (void)conn;
    (void)affected_rows;
    wait_t* w = (wait_t*)user_data;
    w->status = status;
    w->done = 1;
}

static void row_cb(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    wait_t* w = (wait_t*)user_data;
    w->rows++;

    TEST_ASSERT(row->column_count == 2, "列数应为2");
    /* DuckDB driver 对整型通常映射为 I64 */
    TEST_ASSERT(row->values[0].type == VOX_DB_TYPE_I64 || row->values[0].type == VOX_DB_TYPE_U64, "id 类型应为 I64/U64");
    TEST_ASSERT(row->values[1].type == VOX_DB_TYPE_TEXT, "name 类型应为 TEXT");
}

static void done_cb(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    (void)conn;
    wait_t* w = (wait_t*)user_data;
    w->status = status;
    w->rows = row_count;
    w->done = 1;
}

static int wait_until(wait_t* w, uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (!w->done && waited < timeout_ms) {
        vox_thread_sleep(1);
        waited++;
    }
    return w->done ? 0 : -1;
}

static void test_duckdb_basic(vox_mpool_t* mpool) {
    (void)mpool;
    vox_loop_t* loop = vox_loop_create();
    TEST_ASSERT_NOT_NULL(loop, "vox_loop_create failed");

    vox_db_conn_t* db = vox_db_connect(loop, VOX_DB_DRIVER_DUCKDB, ":memory:");
    TEST_ASSERT_NOT_NULL(db, "vox_db_connect(duckdb) failed");

    /* create */
    {
        wait_t w = {0};
        TEST_ASSERT_EQ(vox_db_exec_async(db, "CREATE TABLE t(id BIGINT, name VARCHAR);", NULL, 0, exec_cb, &w), 0, "create failed");
        TEST_ASSERT_EQ(wait_until(&w, 5000), 0, "wait create timeout");
        TEST_ASSERT_EQ(w.status, 0, "create status should be 0");
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
        TEST_ASSERT_EQ(vox_db_exec_async(db, "INSERT INTO t VALUES(?, ?);", params, 2, exec_cb, &w), 0, "insert failed");
        TEST_ASSERT_EQ(wait_until(&w, 5000), 0, "wait insert timeout");
        TEST_ASSERT_EQ(w.status, 0, "insert status should be 0");
    }

    /* query */
    {
        wait_t w = {0};
        TEST_ASSERT_EQ(vox_db_query_async(db, "SELECT id, name FROM t;", NULL, 0, row_cb, done_cb, &w), 0, "query failed");
        TEST_ASSERT_EQ(wait_until(&w, 5000), 0, "wait query timeout");
        TEST_ASSERT_EQ(w.status, 0, "query status should be 0");
        TEST_ASSERT_EQ(w.rows, 1, "row_count should be 1");
    }

    vox_db_disconnect(db);
    vox_loop_destroy(loop);
}

test_case_t test_db_duckdb_cases[] = {
    {"basic", test_duckdb_basic},
};

test_suite_t test_db_duckdb_suite = {
    "db_duckdb",
    test_db_duckdb_cases,
    sizeof(test_db_duckdb_cases) / sizeof(test_db_duckdb_cases[0])
};

#endif /* VOX_USE_DUCKDB */

