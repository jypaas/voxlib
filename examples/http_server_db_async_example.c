/*
 * http_server_db_async_example.c - HTTP + DB（异步延迟响应）示例
 *
 * 依赖：
 * - HTTP server 已支持 defer/finish（本示例会调用 vox_http_context_defer/finish）
 * - DB 回调需切回 loop：建议对 pool 设置 VOX_DB_CALLBACK_LOOP
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
#include <string.h>

static vox_db_pool_t* g_pool = NULL;

typedef struct {
    vox_http_context_t* ctx;
    int found;
    /* 异步查询参数需要在回调结束前保持有效 */
    vox_db_value_t* params;
} req_state_t;

static void db_row_cb(vox_db_conn_t* conn, const vox_db_row_t* row, void* user_data) {
    (void)conn;
    req_state_t* st = (req_state_t*)user_data;
    if (!st || !st->ctx || !row || row->column_count < 2) {
        VOX_LOG_DEBUG("db_row_cb: skip (st=%p, ctx=%p, row=%p, column_count=%zu)", 
            st, st ? st->ctx : NULL, row, row ? row->column_count : 0);
        return;
    }
    st->found = 1;
    VOX_LOG_DEBUG("db_row_cb: found row, column_count=%zu", row->column_count);

    vox_http_context_status(st->ctx, 200);
    vox_http_context_header(st->ctx, "Content-Type", "text/plain; charset=utf-8");
    vox_http_context_write_cstr(st->ctx, "id=");
    if (row->values[0].type == VOX_DB_TYPE_I64) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", (long long)row->values[0].u.i64);
        vox_http_context_write_cstr(st->ctx, buf);
    }
    vox_http_context_write_cstr(st->ctx, " name=");
    if (row->values[1].type == VOX_DB_TYPE_TEXT) {
        vox_http_context_write(st->ctx, row->values[1].u.text.ptr, row->values[1].u.text.len);
    }
    vox_http_context_write_cstr(st->ctx, "\n");
}

static void db_done_cb(vox_db_conn_t* conn, int status, int64_t row_count, void* user_data) {
    (void)conn;
    req_state_t* st = (req_state_t*)user_data;
    if (!st || !st->ctx) return;

    VOX_LOG_DEBUG("db_done_cb: status=%d, row_count=%lld, found=%d", status, (long long)row_count, st->found);

    if (status != 0) {
        vox_http_context_status(st->ctx, 500);
        vox_http_context_write_cstr(st->ctx, "db error\n");
    } else if (!st->found) {
        vox_http_context_status(st->ctx, 404);
        vox_http_context_write_cstr(st->ctx, "not found\n");
    }

    /* 关键：触发真正写回 */
    (void)vox_http_context_finish(st->ctx);
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

    /* defer：handler 返回后不立即发送响应 */
    vox_http_context_defer(ctx);

    /* req_state_t 分配在连接 mpool 上，连接关闭时统一释放
     * 注意：如果客户端在 DB 回调前断开，ctx/st 会随连接 mpool 一起销毁；
     *       当前 demo 未做“请求取消/弱引用”保护，生产环境需在库层或业务层补齐。 */
    vox_mpool_t* mp = vox_http_context_get_mpool(ctx);
    req_state_t* st = (req_state_t*)vox_mpool_alloc(mp, sizeof(req_state_t));
    if (!st) {
        vox_http_context_status(ctx, 500);
        vox_http_context_write_cstr(ctx, "oom\n");
        (void)vox_http_context_finish(ctx);
        return;
    }
    st->ctx = ctx;
    st->found = 0;
    st->params = NULL;

    /* 关键：异步 DB 会在 handler 返回后才读取 params，不能用栈内存 */
    st->params = (vox_db_value_t*)vox_mpool_alloc(mp, sizeof(vox_db_value_t));
    if (!st->params) {
        vox_http_context_status(ctx, 500);
        vox_http_context_write_cstr(ctx, "oom\n");
        (void)vox_http_context_finish(ctx);
        return;
    }
    char* id_copy = (char*)vox_mpool_alloc(mp, id.len + 1);
    if (!id_copy) {
        vox_http_context_status(ctx, 500);
        vox_http_context_write_cstr(ctx, "oom\n");
        (void)vox_http_context_finish(ctx);
        return;
    }
    memcpy(id_copy, id.ptr, id.len);
    id_copy[id.len] = '\0';

    st->params[0].type = VOX_DB_TYPE_TEXT;
    st->params[0].u.text.ptr = id_copy;
    st->params[0].u.text.len = id.len;

    VOX_LOG_DEBUG("query for id=%.*s", (int)id.len, id.ptr);
    if (vox_db_pool_query_async(g_pool, "SELECT id, name FROM t WHERE id = ?;", st->params, 1, db_row_cb, db_done_cb, st) != 0) {
        vox_http_context_status(ctx, 503);
        vox_http_context_write_cstr(ctx, "db busy\n");
        (void)vox_http_context_finish(ctx);
        return;
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

    /* 注意：sqlite3/duckdb 的 ":memory:" 对每个连接是独立 DB。
     * async 示例使用 pool=4，需要共享同一 DB：sqlite3 用共享内存 URI；duckdb 用文件 DB。 */
    g_pool = vox_db_pool_create_ex(loop, VOX_DB_DRIVER_SQLITE3, "file:vox_http_async?mode=memory&cache=shared", 4, 4);
    if (!g_pool) g_pool = vox_db_pool_create_ex(loop, VOX_DB_DRIVER_DUCKDB, "vox_http_async.duckdb", 4, 4);
    if (!g_pool) {
        VOX_LOG_ERROR("no driver enabled or pool create failed");
        vox_loop_destroy(loop);
        return 1;
    }

    /* DB 回调切回 loop，确保 finish 在 loop 线程调用 */
    vox_db_pool_set_callback_mode(g_pool, VOX_DB_CALLBACK_LOOP);

    /* 初始化表数据 */
    if (vox_db_pool_exec(g_pool, "CREATE TABLE t(id INTEGER, name VARCHAR);", NULL, 0, NULL) != 0) {
        VOX_LOG_ERROR("create table failed");
        vox_db_pool_destroy(g_pool);
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 插入测试数据 */
    typedef struct {
        int64_t id;
        const char* name;
    } test_data_t;
    
    test_data_t test_records[] = {
        {1, "alice"},
        {2, "bob"},
        {3, "charlie"},
        {4, "diana"},
        {5, "eve"},
        {6, "frank"},
        {7, "grace"},
        {8, "henry"}
    };
    
    for (int i = 0; i < (int)(sizeof(test_records) / sizeof(test_records[0])); i++) {
        vox_db_value_t p[2];
        p[0].type = VOX_DB_TYPE_I64;
        p[0].u.i64 = test_records[i].id;
        p[1].type = VOX_DB_TYPE_TEXT;
        p[1].u.text.ptr = (char*)test_records[i].name;
        p[1].u.text.len = (size_t)strlen(test_records[i].name);
        int rc = vox_db_pool_exec(g_pool, "INSERT INTO t VALUES(?, ?);", p, 2, NULL);
        if (rc != 0) {
            VOX_LOG_ERROR("insert record %lld failed (rc=%d)", (long long)test_records[i].id, rc);
        } else {
            VOX_LOG_INFO("insert record %lld success", (long long)test_records[i].id);
        }
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
    if (vox_socket_parse_address("0.0.0.0", 8082, &addr) != 0) return 1;
    if (vox_http_server_listen_tcp(server, &addr, 128) != 0) return 1;

    VOX_LOG_INFO("HTTP+DB(async) server listening on 0.0.0.0:8082");
    return vox_loop_run(loop, VOX_RUN_DEFAULT);
}

