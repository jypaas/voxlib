/*
 * db_async_wait_example.c - 演示如何在同步函数中等待异步数据库操作完成
 *
 * ⚠️ 重要警告：
 * 1. 不要在 loop 线程中调用等待函数！这会阻塞事件循环，严重影响性能。
 * 2. 如果使用 VOX_DB_CALLBACK_LOOP 模式，在 loop 线程中等待会导致死锁
 *    （因为回调需要 loop 运行才能执行，但 loop 被阻塞了）。
 * 3. 推荐使用 VOX_DB_CALLBACK_WORKER 模式（默认），回调在工作线程执行。
 *
 * 适用场景：
 * - 在非 loop 线程中需要同步等待异步操作（如初始化、测试代码等）
 * - 不适合在 HTTP 请求处理函数中使用（会阻塞请求处理）
 *
 * 本示例展示了三种方法：
 * 1. 使用 vox_event（推荐）- 高效的事件等待机制
 * 2. 使用轮询等待 - 简单但CPU占用较高
 * 3. 使用同步接口 - 最简单，但会阻塞线程
 */

#include "../vox_loop.h"
#include "../vox_log.h"
#include "../vox_mutex.h"
#include "../vox_thread.h"
#include "../db/vox_db.h"

#include <stdio.h>
#include <string.h>

/* ===== 方法1：使用 vox_event（推荐） ===== */

typedef struct {
    vox_event_t event;
    volatile int done;  /* 回调已触发（SQLite/DuckDB 在 loop 线程回调，需驱动 loop） */
    int status;
    int64_t affected_rows;
    int64_t row_count;
} async_wait_t;

static void on_exec_with_event(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    (void)conn;
    async_wait_t* wait = (async_wait_t*)user_data;
    wait->status = status;
    wait->affected_rows = affected_rows;
    wait->done = 1;
    vox_event_set(&wait->event);  /* 触发事件，唤醒等待的线程 */
}

static void on_row_with_event(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    async_wait_t* wait = (async_wait_t*)user_data;
    wait->row_count++;
    /* 可以在这里处理每一行数据 */
    printf("收到行 %lld: ", (long long)wait->row_count);
    for (size_t i = 0; i < row->column_count; i++) {
        if (i) printf(", ");
        const vox_db_value_t* v = &row->values[i];
        switch (v->type) {
            case VOX_DB_TYPE_I64:
                printf("%lld", (long long)v->u.i64);
                break;
            case VOX_DB_TYPE_TEXT:
                printf("%.*s", (int)v->u.text.len, v->u.text.ptr ? v->u.text.ptr : "");
                break;
            default:
                printf("?");
                break;
        }
    }
    printf("\n");
}

static void on_done_with_event(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    (void)conn;
    (void)row_count;
    async_wait_t* wait = (async_wait_t*)user_data;
    wait->status = status;
    wait->done = 1;
    vox_event_set(&wait->event);  /* 触发事件，唤醒等待的线程 */
}

/**
 * 同步等待异步执行完成（使用事件）
 * 
 * ⚠️ 警告：不要在 loop 线程中调用此函数！
 * 此函数会阻塞当前线程直到操作完成。如果在 loop 线程中调用：
 * - 会阻塞事件循环，影响所有其他异步操作
 * - 如果使用 VOX_DB_CALLBACK_LOOP 模式，会导致死锁
 * 
 * 推荐使用场景：
 * - 在非 loop 线程中（如初始化线程、测试代码）
 * - 使用 VOX_DB_CALLBACK_WORKER 模式（默认）
 * 
 * @return 成功返回0，失败返回-1
 */
static int db_exec_async_wait(vox_db_conn_t* conn,
                               const char* sql,
                               const vox_db_value_t* params,
                               size_t nparams,
                               int64_t* out_affected_rows,
                               int32_t timeout_ms) {
    async_wait_t wait;
    memset(&wait, 0, sizeof(wait));
    
    /* 创建自动重置事件（初始状态为未触发） */
    if (vox_event_create(&wait.event, false, false) != 0) {
        return -1;
    }
    
    /* 发起异步操作 */
    if (vox_db_exec_async(conn, sql, params, nparams, on_exec_with_event, &wait) != 0) {
        vox_event_destroy(&wait.event);
        return -1;
    }
    
    /* SQLite/DuckDB 使用 use_loop_thread_for_async，需驱动事件循环 */
    vox_loop_t* loop = vox_db_get_loop(conn);
    uint32_t waited = 0;
    while (!wait.done && waited < (uint32_t)timeout_ms) {
        if (loop) vox_loop_run(loop, VOX_RUN_ONCE);
        vox_event_timedwait(&wait.event, 1);
        waited++;
    }
    if (!wait.done) {
        vox_event_destroy(&wait.event);
        return -1;
    }
    
    /* 获取结果 */
    if (out_affected_rows) {
        *out_affected_rows = wait.affected_rows;
    }
    
    vox_event_destroy(&wait.event);
    return wait.status;
}

/**
 * 同步等待异步查询完成（使用事件）
 * 
 * ⚠️ 警告：不要在 loop 线程中调用此函数！
 * 此函数会阻塞当前线程直到操作完成。如果在 loop 线程中调用：
 * - 会阻塞事件循环，影响所有其他异步操作
 * - 如果使用 VOX_DB_CALLBACK_LOOP 模式，会导致死锁
 * 
 * 推荐使用场景：
 * - 在非 loop 线程中（如初始化线程、测试代码）
 * - 使用 VOX_DB_CALLBACK_WORKER 模式（默认）
 * 
 * @return 成功返回0，失败返回-1
 */
static int db_query_async_wait(vox_db_conn_t* conn,
                                const char* sql,
                                const vox_db_value_t* params,
                                size_t nparams,
                                int64_t* out_row_count,
                                int32_t timeout_ms) {
    async_wait_t wait;
    memset(&wait, 0, sizeof(wait));
    
    /* 创建自动重置事件（初始状态为未触发） */
    if (vox_event_create(&wait.event, false, false) != 0) {
        return -1;
    }
    
    /* 发起异步查询 */
    if (vox_db_query_async(conn, sql, params, nparams, 
                           on_row_with_event, on_done_with_event, &wait) != 0) {
        vox_event_destroy(&wait.event);
        return -1;
    }
    
    /* SQLite/DuckDB 使用 use_loop_thread_for_async，需驱动事件循环 */
    vox_loop_t* loop = vox_db_get_loop(conn);
    uint32_t waited = 0;
    while (!wait.done && waited < (uint32_t)timeout_ms) {
        if (loop) vox_loop_run(loop, VOX_RUN_ONCE);
        vox_event_timedwait(&wait.event, 1);
        waited++;
    }
    if (!wait.done) {
        vox_event_destroy(&wait.event);
        return -1;
    }
    
    /* 获取结果 */
    if (out_row_count) {
        *out_row_count = wait.row_count;
    }
    
    vox_event_destroy(&wait.event);
    return wait.status;
}

/* ===== 方法2：使用轮询等待（简单但CPU占用较高） ===== */

typedef struct {
    volatile int done;
    volatile int status;
    int64_t affected_rows;
} poll_wait_t;

static void on_exec_poll(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    (void)conn;
    poll_wait_t* wait = (poll_wait_t*)user_data;
    wait->status = status;
    wait->affected_rows = affected_rows;
    wait->done = 1;  /* 设置完成标志 */
}

static int db_exec_async_poll(vox_db_conn_t* conn,
                               const char* sql,
                               const vox_db_value_t* params,
                               size_t nparams,
                               int64_t* out_affected_rows,
                               uint32_t timeout_ms) {
    poll_wait_t wait = {0};
    
    /* 发起异步操作 */
    if (vox_db_exec_async(conn, sql, params, nparams, on_exec_poll, &wait) != 0) {
        return -1;
    }
    
    /* SQLite/DuckDB 使用 use_loop_thread_for_async，需驱动事件循环 */
    vox_loop_t* loop = vox_db_get_loop(conn);
    uint32_t waited = 0;
    while (!wait.done && waited < timeout_ms) {
        if (loop) vox_loop_run(loop, VOX_RUN_ONCE);
        vox_thread_sleep(1);
        waited++;
    }
    
    if (!wait.done) {
        return -1;  /* 超时 */
    }
    
    if (out_affected_rows) {
        *out_affected_rows = wait.affected_rows;
    }
    
    return wait.status;
}

/* ===== 方法3：直接使用同步接口（最简单） ===== */

/* 如果不需要异步特性，可以直接使用同步接口：
 * 
 * int64_t affected_rows = 0;
 * int status = vox_db_exec(conn, sql, params, nparams, &affected_rows);
 * 
 * int64_t row_count = 0;
 * int status = vox_db_query(conn, sql, params, nparams, row_cb, user_data, &row_count);
 * 
 * ⚠️ 警告：这些接口会阻塞当前线程直到操作完成。
 * - 如果在 loop 线程中调用，会阻塞事件循环
 * - 推荐在非 loop 线程中使用，或使用异步接口配合回调
 */

/* 同步查询的行回调（用于演示） */
static int sync_row_cb_called = 0;

static void sync_row_cb(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    (void)user_data;
    sync_row_cb_called++;
    printf("   同步查询行 %d: ", sync_row_cb_called);
    for (size_t i = 0; i < row->column_count; i++) {
        if (i) printf(", ");
        const vox_db_value_t* v = &row->values[i];
        switch (v->type) {
            case VOX_DB_TYPE_I64:
                printf("%lld", (long long)v->u.i64);
                break;
            case VOX_DB_TYPE_TEXT:
                printf("%.*s", (int)v->u.text.len, v->u.text.ptr ? v->u.text.ptr : "");
                break;
            default:
                printf("?");
                break;
        }
    }
    printf("\n");
}

/* ===== 主函数演示 ===== */

int main(void) {
    vox_log_set_level(VOX_LOG_INFO);
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return 1;
    }

    /* 连接数据库 */
    vox_db_conn_t* db = vox_db_connect(loop, VOX_DB_DRIVER_SQLITE3, ":memory:");
    if (!db) {
        db = vox_db_connect(loop, VOX_DB_DRIVER_DUCKDB, ":memory:");
    }
    if (!db) {
        VOX_LOG_ERROR("无法连接数据库");
        vox_loop_destroy(loop);
        return 1;
    }

    /* ⚠️ 重要：确保使用 WORKER 模式（默认），回调在工作线程执行
     * 这样在非 loop 线程中等待不会影响 loop 性能 */
    vox_db_set_callback_mode(db, VOX_DB_CALLBACK_WORKER);
    
    printf("⚠️  注意：本示例在 main 线程（非 loop 线程）中等待异步操作\n");
    printf("   如果在 loop 线程中等待，会阻塞事件循环，严重影响性能！\n\n");

    printf("=== 方法1：使用 vox_event 等待异步操作 ===\n\n");

    /* 创建表 */
    printf("1. 创建表...\n");
    int64_t affected = 0;
    if (db_exec_async_wait(db, "CREATE TABLE users(id INTEGER PRIMARY KEY, name VARCHAR(50));", 
                           NULL, 0, &affected, 5000) != 0) {
        VOX_LOG_ERROR("创建表失败: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
        goto cleanup;
    }
    printf("   成功，影响行数: %lld\n\n", (long long)affected);

    /* 插入数据 */
    printf("2. 插入数据...\n");
    vox_db_value_t params[2];
    params[0].type = VOX_DB_TYPE_I64;
    params[0].u.i64 = 1;
    params[1].type = VOX_DB_TYPE_TEXT;
    params[1].u.text.ptr = "Alice";
    params[1].u.text.len = 5;
    
    if (db_exec_async_wait(db, "INSERT INTO users VALUES(?, ?);", 
                           params, 2, &affected, 5000) != 0) {
        VOX_LOG_ERROR("插入失败: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
        goto cleanup;
    }
    printf("   成功，影响行数: %lld\n\n", (long long)affected);

    /* 查询数据 */
    printf("3. 查询数据...\n");
    int64_t row_count = 0;
    if (db_query_async_wait(db, "SELECT id, name FROM users;", 
                            NULL, 0, &row_count, 5000) != 0) {
        VOX_LOG_ERROR("查询失败: %s", vox_db_last_error(db) ? vox_db_last_error(db) : "(no error)");
        goto cleanup;
    }
    printf("   成功，共 %lld 行\n\n", (long long)row_count);

    printf("=== 方法2：使用轮询等待（演示） ===\n\n");
    
    /* 使用轮询方式插入另一条数据 */
    params[0].u.i64 = 2;
    params[1].u.text.ptr = "Bob";
    params[1].u.text.len = 3;
    
    if (db_exec_async_poll(db, "INSERT INTO users VALUES(?, ?);", 
                           params, 2, &affected, 5000) != 0) {
        VOX_LOG_ERROR("轮询方式插入失败");
        goto cleanup;
    }
    printf("   使用轮询方式插入成功，影响行数: %lld\n\n", (long long)affected);

    printf("=== 方法3：使用同步接口（演示） ===\n\n");
    
    /* 直接使用同步接口查询 */
    int64_t sync_row_count = 0;
    sync_row_cb_called = 0;
    
    if (vox_db_query(db, "SELECT id, name FROM users;", NULL, 0, 
                     sync_row_cb, NULL, &sync_row_count) != 0) {
        VOX_LOG_ERROR("同步查询失败");
        goto cleanup;
    }
    printf("   同步查询成功，共 %lld 行\n\n", (long long)sync_row_count);

    printf("=== 所有方法演示完成 ===\n");

cleanup:
    vox_db_disconnect(db);
    vox_loop_destroy(loop);
    return 0;
}
