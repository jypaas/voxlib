/*
 * db_pool_async_mysql_example.c - MySQL 连接池 + 异步示例（模拟多"请求"并发）
 *
 * 关键点：
 * - 使用 vox_db_pool_t 避免单连接 busy
 * - 这里演示 VOX_DB_CALLBACK_LOOP：回调切回 loop，便于与上层事件驱动系统集成
 * - MySQL 连接字符串格式：host=127.0.0.1;port=3306;user=root;password=xxx;db=testdb;charset=utf8mb4
 *
 * 编译要求：
 * - 需要启用 VOX_USE_MYSQL=ON
 * - 需要安装 libmysqlclient 开发库
 *
 * 使用前准备：
 * 1. 确保 MySQL 服务正在运行
 * 2. 创建测试数据库：CREATE DATABASE testdb;
 * 3. 修改下面的连接字符串（host, port, user, password, db）
 */

#include "../vox_loop.h"
#include "../vox_log.h"
#include "../vox_mpool.h"
#include "../db/vox_db.h"
#include "../db/vox_db_pool.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    vox_loop_t* loop;
    vox_db_pool_t* pool;
    int total;
    int done;
    int failed;
    int query_total;
    int query_done;
    int query_failed;
} app_t;

typedef struct {
    app_t* app;
    vox_db_value_t params[2];
    char* name;
} insert_req_t;

typedef struct {
    app_t* app;
    int query_id;
    vox_db_value_t params[1];  /* 持久化参数，避免传栈上局部变量导致异步执行时读到无效值 */
} query_req_t;

/* 前向声明 */
static void on_query_row(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data);
static void on_query_done(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data);

static void on_exec(vox_db_conn_t* conn, int status, int64_t affected_rows, void* user_data) {
    (void)conn;
    insert_req_t* req = (insert_req_t*)user_data;
    if (!req || !req->app) {
        VOX_LOG_ERROR("on_exec: invalid req or app");
        return;
    }
    app_t* app = req->app;
    app->done++;
    if (status != 0) {
        app->failed++;
        const char* err = vox_db_last_error(conn);
        VOX_LOG_WARN("on_exec: operation failed (done=%d/%d, failed=%d, error=%s)", 
                     app->done, app->total, app->failed, err ? err : "unknown");
    } else {
        if (app->done % 10 == 0 || app->done == app->total) {
            VOX_LOG_INFO("on_exec: progress (done=%d/%d, failed=%d, affected=%lld)", 
                         app->done, app->total, app->failed, (long long)affected_rows);
        }
    }
    if (req->name) vox_mpool_free(vox_loop_get_mpool(app->loop), req->name);
    vox_mpool_free(vox_loop_get_mpool(app->loop), req);
    if (app->done >= app->total) {
        VOX_LOG_INFO("pool exec done: total=%d done=%d failed=%d", app->total, app->done, app->failed);
        /* 插入完成后，开始查询操作 */
        VOX_LOG_INFO("start_work: starting query operations...");
        app->query_total = 10;  /* 查询前10条记录 */
        app->query_done = 0;
        app->query_failed = 0;
        
        for (int i = 0; i < app->query_total; i++) {
            vox_mpool_t* mp = vox_loop_get_mpool(app->loop);
            query_req_t* qreq = (query_req_t*)vox_mpool_alloc(mp, sizeof(query_req_t));
            if (!qreq) {
                app->query_done++;
                app->query_failed++;
                continue;
            }
            memset(qreq, 0, sizeof(*qreq));
            qreq->app = app;
            qreq->query_id = i;
            qreq->params[0].type = VOX_DB_TYPE_I64;
            qreq->params[0].u.i64 = (int64_t)i;

            if (vox_db_pool_query_async(app->pool, "SELECT id, name FROM t WHERE id = ? LIMIT 1;",
                                        qreq->params, 1, on_query_row, on_query_done, qreq) != 0) {
                vox_mpool_free(mp, qreq);
                app->query_done++;
                app->query_failed++;
            }
        }
        
        if (app->query_done >= app->query_total) {
            VOX_LOG_INFO("pool query done: total=%d failed=%d", app->query_total, app->query_failed);
            vox_loop_stop(app->loop);
        }
    }
}

static void on_query_row(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    query_req_t* req = (query_req_t*)user_data;
    if (!req || !row) return;

    /* 打印查询结果（id 可能为 INTEGER，name 为 TEXT） */
    if (row->column_count >= 2) {
        char id_buf[24];
        const char* id_str = "NULL";
        if (row->values[0].type == VOX_DB_TYPE_I64) {
            (void)snprintf(id_buf, sizeof(id_buf), "%lld", (long long)row->values[0].u.i64);
            id_buf[sizeof(id_buf) - 1] = '\0';
            id_str = id_buf;
        } else if (row->values[0].type == VOX_DB_TYPE_TEXT && row->values[0].u.text.ptr)
            id_str = row->values[0].u.text.ptr;
        const char* name_str = row->values[1].type == VOX_DB_TYPE_TEXT && row->values[1].u.text.ptr
                               ? row->values[1].u.text.ptr : "NULL";
        VOX_LOG_INFO("query[%d]: id=%s, name=%s", req->query_id, id_str, name_str);
    }
}

static void on_query_done(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    (void)conn;
    query_req_t* req = (query_req_t*)user_data;
    if (!req || !req->app) return;
    app_t* app = req->app;
    app->query_done++;
    if (status != 0) {
        app->query_failed++;
        const char* err = vox_db_last_error(conn);
        VOX_LOG_WARN("query[%d] failed: %s", req->query_id, err ? err : "unknown");
    } else {
        VOX_LOG_INFO("query[%d] done: row_count=%lld", req->query_id, (long long)row_count);
    }
    vox_mpool_free(vox_loop_get_mpool(app->loop), req);
    if (app->query_done >= app->query_total) {
        VOX_LOG_INFO("pool query done: total=%d done=%d failed=%d", app->query_total, app->query_done, app->query_failed);
        vox_loop_stop(app->loop);
    }
}

static void start_work(vox_loop_t* loop, void* user_data) {
    (void)loop;
    app_t* app = (app_t*)user_data;
    if (!app || !app->pool) {
        VOX_LOG_ERROR("start_work: invalid app or pool");
        if (app && app->loop) vox_loop_stop(app->loop);
        return;
    }

    VOX_LOG_INFO("start_work: creating table...");
    /* 建表（先同步做一次，简化流程） */
    int64_t affected = 0;
    if (vox_db_pool_exec(app->pool, "CREATE TABLE IF NOT EXISTS t(id INTEGER, name VARCHAR(64));", NULL, 0, &affected) != 0) {
        VOX_LOG_ERROR("create table failed");
        vox_loop_stop(app->loop);
        return;
    }
    VOX_LOG_INFO("start_work: table created successfully");

    /* 清空表（可选，用于重复测试） */
    vox_db_pool_exec(app->pool, "TRUNCATE TABLE t;", NULL, 0, NULL);

    /* 提交 N 次 insert */
    const int N = 100;
    app->total = N;
    app->done = 0;
    app->failed = 0;
    app->query_total = 0;
    app->query_done = 0;
    app->query_failed = 0;
    VOX_LOG_INFO("start_work: submitting %d insert operations...", N);

    for (int i = 0; i < N; i++) {
        vox_mpool_t* mp = vox_loop_get_mpool(app->loop);
        insert_req_t* req = (insert_req_t*)vox_mpool_alloc(mp, sizeof(insert_req_t));
        if (!req) {
            app->done++;
            app->failed++;
            continue;
        }
        memset(req, 0, sizeof(*req));
        req->app = app;

        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "u%03d", i);
        if (n < 0 || n >= (int)sizeof(tmp)) {
            /* snprintf 失败或字符串被截断 */
            vox_mpool_free(mp, req);
            app->done++;
            app->failed++;
            continue;
        }
        req->name = (char*)vox_mpool_alloc(mp, (size_t)n + 1);
        if (!req->name) {
            vox_mpool_free(mp, req);
            app->done++;
            app->failed++;
            continue;
        }
        memcpy(req->name, tmp, (size_t)n);
        req->name[n] = '\0';

        req->params[0].type = VOX_DB_TYPE_I64;
        req->params[0].u.i64 = i;
        req->params[1].type = VOX_DB_TYPE_TEXT;
        req->params[1].u.text.ptr = req->name;
        req->params[1].u.text.len = (size_t)n;

        if (vox_db_pool_exec_async(app->pool, "INSERT INTO t VALUES(?, ?);", req->params, 2, on_exec, req) != 0) {
            /* pool 无空闲连接时会失败：这里把失败也计入 */
            VOX_LOG_WARN("start_work: failed to submit operation %d (pool may be exhausted)", i);
            if (req->name) vox_mpool_free(mp, req->name);
            vox_mpool_free(mp, req);
            app->done++;
            app->failed++;
            if (app->done % 10 == 0) {
                VOX_LOG_WARN("start_work: %d operations failed so far (done=%d failed=%d)", 
                             app->done, app->done, app->failed);
            }
        }
    }

    VOX_LOG_INFO("start_work: submitted %d operations, done=%d failed=%d", N, app->done, app->failed);
    
    /* 如果所有操作都立即失败，需要停止循环 */
    if (app->done >= app->total) {
        VOX_LOG_INFO("pool exec done: total=%d failed=%d (all operations failed immediately)", app->total, app->failed);
        vox_loop_stop(app->loop);
    }
}

int main(void) {
    vox_log_set_level(VOX_LOG_INFO);

    VOX_LOG_INFO("main: creating event loop...");
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "vox_loop_create failed\n");
        return 1;
    }

    /* MySQL 连接字符串格式：host=127.0.0.1;port=3306;user=root;password=xxx;db=testdb;charset=utf8mb4 */
    /* 请根据实际情况修改以下连接参数 */
    const char* mysql_conninfo = "host=127.0.0.1;port=3306;user=root;password=helloworld;db=test;charset=utf8mb4";
    
    VOX_LOG_INFO("main: creating database pool (MySQL)...");
    /* 使用动态连接池：初始 8 个连接，最大 32 个连接 */
    vox_db_pool_t* pool = vox_db_pool_create_ex(loop, VOX_DB_DRIVER_MYSQL, mysql_conninfo, 8, 100);
    if (!pool) {
        VOX_LOG_ERROR("main: failed to create MySQL connection pool");
        VOX_LOG_ERROR("main: please check:");
        VOX_LOG_ERROR("  1. MySQL server is running");
        VOX_LOG_ERROR("  2. Database 'testdb' exists (CREATE DATABASE testdb;)");
        VOX_LOG_ERROR("  3. Connection string is correct (host, port, user, password, db)");
        vox_loop_destroy(loop);
        return 1;
    }

    /* 回调切回 loop（更贴近服务端使用方式） */
    vox_db_pool_set_callback_mode(pool, VOX_DB_CALLBACK_LOOP);

    app_t app = {0};
    app.loop = loop;
    app.pool = pool;

    VOX_LOG_INFO("main: queueing start_work...");
    /* 使用 queue_work 而不是 queue_work_immediate，确保在事件循环运行后再执行 */
    if (vox_loop_queue_work(loop, start_work, &app) != 0) {
        VOX_LOG_ERROR("failed to queue start_work");
        vox_db_pool_destroy(pool);
        vox_loop_destroy(loop);
        return 1;
    }
    VOX_LOG_INFO("main: running event loop...");
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    VOX_LOG_INFO("main: event loop stopped");

    vox_db_pool_destroy(pool);
    vox_loop_destroy(loop);
    return 0;
}
