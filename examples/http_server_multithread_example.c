/*
 * http_server_multithread_example.c - 单端口多线程 HTTP Server 示例
 *
 * 架构说明：
 * - 主线程创建事件循环和 HTTP 服务器，监听单一端口（8080）
 * - 创建线程池处理 HTTP 请求（CPU 密集型操作）
 * - 效率高：单端口，异步 I/O + 线程池并行处理请求
 *
 * 测试命令：
 * - wrk -t8 -c1000 -d30s http://127.0.0.1:8080/hello
 * - ab -n 100000 -c 1000 http://127.0.0.1:8080/hello
 */

#include "../vox_loop.h"
#include "../vox_socket.h"
#include "../vox_log.h"
#include "../vox_tpool.h"
#include "../vox_mpool.h"
#include "../vox_thread.h"

#include "../http/vox_http_engine.h"
#include "../http/vox_http_server.h"
#include "../http/vox_http_context.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 线程池线程数量（处理 HTTP 请求） */
#define WORKER_THREAD_COUNT 8

/* 监听端口 */
#define LISTEN_PORT 8080

/* 全局 loop 指针（用于示例中调用 vox_loop_queue_work） */
static vox_loop_t* g_loop = NULL;

/* 使用 vox_loop_queue_work 时的任务结构 */
typedef struct {
    vox_http_context_t* ctx;
} queued_task_t;

/* queued 回调：在事件循环的下一次迭代执行，发送响应 */
static void hello_worker_cb(vox_loop_t* loop, void* user_data) {
    VOX_UNUSED(loop);
    queued_task_t* t = (queued_task_t*)user_data;
    if (!t) return;
    if (t->ctx) {
        vox_http_context_status(t->ctx, 200);
        vox_http_context_write_cstr(t->ctx, "hello from queued work");

        vox_mpool_t* mpool = vox_http_context_get_mpool(t->ctx);
        vox_mpool_free(mpool, t);
        vox_http_context_finish(t->ctx);  /* 立即发送响应 */
    }
}

/* queued 回调：在事件循环的下一次迭代执行，发送 /info 响应 */
static void info_worker_cb(vox_loop_t* loop, void* user_data) {
    VOX_UNUSED(loop);
    queued_task_t* t = (queued_task_t*)user_data;
    if (!t) return;
    if (t->ctx) {
        vox_http_context_status(t->ctx, 200);
        vox_http_context_write_cstr(t->ctx, "info from queued work");

        vox_mpool_t* mpool = vox_http_context_get_mpool(t->ctx);
        vox_mpool_free(mpool, t);
        vox_http_context_finish(t->ctx);  /* 立即发送响应 */
    }
}

/* 中间件：记录请求日志 */
static void mw_logger(vox_http_context_t* ctx) {
    const vox_http_request_t* req = vox_http_context_request(ctx);
    if (req && req->path.ptr) {
        VOX_LOG_DEBUG("[http] %.*s (thread=%llu)",
                      (int)req->path.len, req->path.ptr,
                      (unsigned long long)vox_thread_self());
    }
    vox_http_context_next(ctx);
}

/* 路由处理：/hello - 使用线程池异步处理 */
static void hello_handler(vox_http_context_t* ctx) {
    /* 将响应提交到事件循环的下一次迭代（使用 vox_loop_queue_work） */
    vox_mpool_t* mpool = vox_http_context_get_mpool(ctx);
    vox_loop_t* loop = vox_http_context_get_loop(ctx);
    queued_task_t* q = (queued_task_t*)vox_mpool_alloc(mpool, sizeof(queued_task_t));
    if (!q) {
        vox_http_context_status(ctx, 500);
        vox_http_context_write_cstr(ctx, "Memory allocation failed\n");
        return;
    }
    q->ctx = ctx;

    /* 将任务排入 loop 的待处理队列 */
    vox_http_context_defer(ctx);  /* 标记为延迟响应 */
    if (0 != vox_loop_queue_work(loop, hello_worker_cb, q)) {
        /* 回退到立即响应，并释放由 mpool 分配的内存 */
        vox_http_context_status(ctx, 200);
        vox_http_context_write_cstr(ctx, "hello from single-port multi-thread server");
        vox_mpool_free(mpool, q);
        vox_http_context_finish(ctx);  /* 立即发送响应 */
    }
}

/* 路由处理：/info */
static void info_handler(vox_http_context_t* ctx) {
    /* 将 /info 响应提交到事件循环的下一次迭代 */
    vox_mpool_t* mpool = vox_http_context_get_mpool(ctx);
    vox_loop_t* loop = vox_http_context_get_loop(ctx);
    queued_task_t* q = (queued_task_t*)vox_mpool_alloc(mpool, sizeof(queued_task_t));
    if (!q) {
        vox_http_context_status(ctx, 500);
        vox_http_context_write_cstr(ctx, "Memory allocation failed\n");
        return;
    }
    q->ctx = ctx;

    if (0 != vox_loop_queue_work(loop, info_worker_cb, q)) {
        /* 回退：立即响应 */
        vox_http_context_status(ctx, 200);
        vox_http_context_write_cstr(ctx, "info from single-port multi-thread server");
        vox_mpool_free(mpool, q);
    }
}

int main(void) {
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }

    vox_log_set_level(VOX_LOG_INFO);

    VOX_LOG_INFO("=== Single-Port Multi-Thread HTTP Server ===");
    VOX_LOG_INFO("Threads: %d", WORKER_THREAD_COUNT);
    VOX_LOG_INFO("Port: %d (single port)", LISTEN_PORT);

    /* 配置 backend */
    vox_backend_config_t backend_config = {0};
    backend_config.type = VOX_BACKEND_TYPE_AUTO;
    backend_config.mpool = NULL;
    backend_config.max_events = 10240;

    vox_tpool_config_t tpool_config = {0};
    tpool_config.thread_count = WORKER_THREAD_COUNT;
    tpool_config.queue_capacity = 2048;
    tpool_config.queue_type = VOX_QUEUE_TYPE_MPSC;

    /* 配置 loop */
    vox_loop_config_t loop_config = {0};
    loop_config.backend_config = &backend_config;
    loop_config.tpool_config = &tpool_config;

    /* 创建主事件循环 */
    vox_loop_t* loop = vox_loop_create_with_config(&loop_config);
    if (!loop) {
        fprintf(stderr, "Failed to create loop\n");
        vox_socket_cleanup();
        return 1;
    }
    /* 设置全局 loop 指针，供示例中使用 vox_loop_queue_work */
    g_loop = loop;

    /* 创建 HTTP engine */
    vox_http_engine_t* engine = vox_http_engine_create(loop);
    if (!engine) {
        fprintf(stderr, "Failed to create engine\n");
        vox_loop_destroy(loop);
        vox_socket_cleanup();
        return 1;
    }

    /* 注册中间件 */
    vox_http_engine_use(engine, mw_logger);

    /* 注册路由 */
    {
        vox_http_handler_cb hs[] = { hello_handler };
        vox_http_engine_get(engine, "/hello", hs, 1);
    }
    {
        vox_http_handler_cb hs[] = { info_handler };
        vox_http_engine_get(engine, "/info", hs, 1);
    }

    /* 创建 HTTP server */
    vox_http_server_t* server = vox_http_server_create(engine);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        vox_http_engine_destroy(engine);
        vox_loop_destroy(loop);
        vox_socket_cleanup();
        return 1;
    }

    /* 绑定并监听单一端口 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address("0.0.0.0", LISTEN_PORT, &addr) != 0) {
        fprintf(stderr, "Failed to parse address\n");
        vox_http_server_destroy(server);
        vox_http_engine_destroy(engine);
        vox_loop_destroy(loop);
        vox_socket_cleanup();
        return 1;
    }

    if (vox_http_server_listen_tcp(server, &addr, 2048) != 0) {
        fprintf(stderr, "Failed to listen on port %d\n", LISTEN_PORT);
        vox_http_server_destroy(server);
        vox_http_engine_destroy(engine);
        vox_loop_destroy(loop);
        vox_socket_cleanup();
        return 1;
    }

    VOX_LOG_INFO("HTTP server listening on 0.0.0.0:%d", LISTEN_PORT);
    VOX_LOG_INFO("Test: wrk -t8 -c1000 -d30s http://127.0.0.1:%d/hello", LISTEN_PORT);
    VOX_LOG_INFO("Press Ctrl+C to stop...");

    /* 创建线程池（可选：用于处理 CPU 密集型任务） */
    vox_tpool_t* tpool = vox_tpool_create();
    if (tpool) {
        VOX_LOG_INFO("Thread pool created with default settings");
    }

    /* 运行事件循环 - 在主线程中运行，处理所有连接 */
    int ret = vox_loop_run(loop, VOX_RUN_DEFAULT);

    VOX_LOG_INFO("Server stopped (ret=%d)", ret);

    /* 清理资源 */
    if (tpool) {
        vox_tpool_destroy(tpool);
    }
    vox_http_server_destroy(server);
    vox_http_engine_destroy(engine);
    vox_loop_destroy(loop);
    vox_socket_cleanup();

    return 0;
}
