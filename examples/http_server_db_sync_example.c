/*
 * http_server_db_sync_example.c - HTTP + DB（同步）示例
 *
 * 重要说明：
 * - 当前 `vox_http_server` 的 handler 模型是“同步链式执行”，执行完后立即 build_response 并写回。
 *   因此：**无法在 handler 返回后再异步写响应**（除非未来扩展 http 模块支持 defer/resume）。
 * - 这个示例为了演示“在 handler 中访问数据库”，使用了 DB 同步 API，会阻塞事件循环线程：
 *   **仅用于演示/原型**，生产环境建议：
 *   - 扩展 http 模块支持异步延迟响应；或
 *   - 通过上层架构把请求转交给工作线程并使用独立协议/队列返回结果。
 */

#include "../vox_loop.h"
#include "../vox_log.h"
#include "../vox_socket.h"

#include "../http/vox_http_engine.h"
#include "../http/vox_http_server.h"
#include "../http/vox_http_context.h"

#include "../db/vox_db.h"
#include "../db/vox_db_pool.h"

#include <stdio.h>

static vox_db_pool_t* g_pool = NULL;

static void user_row_to_ctx(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    vox_http_context_t* ctx = (vox_http_context_t*)user_data;
    if (!ctx || !row || row->column_count < 2) return;

    vox_http_context_status(ctx, 200);
    vox_http_context_header(ctx, "Content-Type", "text/plain; charset=utf-8");
    vox_http_context_write_cstr(ctx, "id=");
    if (row->values[0].type == VOX_DB_TYPE_I64) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", (long long)row->values[0].u.i64);
        vox_http_context_write_cstr(ctx, buf);
    }
    vox_http_context_write_cstr(ctx, " name=");
    if (row->values[1].type == VOX_DB_TYPE_TEXT) {
        vox_http_context_write(ctx, row->values[1].u.text.ptr, row->values[1].u.text.len);
    }
    vox_http_context_write_cstr(ctx, "\n");
}

static void get_user_handler(vox_http_context_t* ctx) {
    if (!ctx || !g_pool) {
        vox_http_context_status(ctx, 500);
        vox_http_context_write_cstr(ctx, "db not ready\n");
        return;
    }

    vox_strview_t id = vox_http_context_param(ctx, "id");
    if (!id.ptr || id.len == 0) {
        vox_http_context_status(ctx, 400);
        vox_http_context_write_cstr(ctx, "bad id\n");
        return;
    }

    /* 同步查询：阻塞事件循环线程（仅演示） */
    vox_db_value_t p[1];
    p[0].type = VOX_DB_TYPE_TEXT;
    p[0].u.text.ptr = id.ptr;
    p[0].u.text.len = id.len;

    int64_t rows = 0;
    int rc = vox_db_pool_query(g_pool, "SELECT id, name FROM t WHERE id = ?;", p, 1, user_row_to_ctx, ctx, &rows);
    if (rc != 0 || rows == 0) {
        vox_http_context_status(ctx, 404);
        vox_http_context_write_cstr(ctx, "not found\n");
    }
}

int main(void) {
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }
    vox_log_set_level(VOX_LOG_INFO);

    vox_loop_t* loop = vox_loop_create();
    if (!loop) return 1;

    /* 创建 DB pool（sqlite3/duckdb 二选一） */
    g_pool = vox_db_pool_create_ex(loop, VOX_DB_DRIVER_SQLITE3, ":memory:", 1, 1);
    if (!g_pool) g_pool = vox_db_pool_create_ex(loop, VOX_DB_DRIVER_DUCKDB, ":memory:", 1, 1);
    if (!g_pool) {
        VOX_LOG_ERROR("no driver enabled or pool create failed");
        vox_loop_destroy(loop);
        return 1;
    }

    /* 初始化表数据（同步） */
    if (vox_db_pool_exec(g_pool, "CREATE TABLE t(id INTEGER, name VARCHAR);", NULL, 0, NULL) != 0) {
        VOX_LOG_ERROR("create table failed");
        return 1;
    }
    {
        vox_db_value_t p[2];
        p[0].type = VOX_DB_TYPE_I64;
        p[0].u.i64 = 1;
        p[1].type = VOX_DB_TYPE_TEXT;
        p[1].u.text.ptr = "alice";
        p[1].u.text.len = 5;
        (void)vox_db_pool_exec(g_pool, "INSERT INTO t VALUES(?, ?);", p, 2, NULL);
    }

    vox_http_engine_t* engine = vox_http_engine_create(loop);
    if (!engine) return 1;

    vox_http_group_t* api = vox_http_engine_group(engine, "/api");
    if (api) {
        vox_http_handler_cb hs[] = { get_user_handler };
        vox_http_group_get(api, "/user/:id", hs, sizeof(hs) / sizeof(hs[0]));
    }

    vox_http_server_t* server = vox_http_server_create(engine);
    if (!server) return 1;

    vox_socket_addr_t addr;
    if (vox_socket_parse_address("0.0.0.0", 8081, &addr) != 0) return 1;
    if (vox_http_server_listen_tcp(server, &addr, 128) != 0) return 1;

    VOX_LOG_INFO("HTTP+DB(sync) server listening on 0.0.0.0:8081");
    return vox_loop_run(loop, VOX_RUN_DEFAULT);
}

