/*
 * redis_pool_comparison.c - Redis 连接池对比示例
 * 对比固定连接池和动态连接池的性能和资源占用
 *
 * 新版示例只使用“纯连接池”：acquire -> 用 client 发命令 -> release。
 */

#include "../redis/vox_redis_pool.h"
#include "../vox_loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ===== 测试配置 ===== */

#define TEST_REQUESTS 100
#define TEST_CONCURRENT 50

/* ===== 测试上下文 ===== */

typedef struct {
    vox_loop_t* loop;
    vox_redis_pool_t* pool;
    const char* pool_name;
    int total_requests;
    int completed_requests;
    int failed_requests;
    clock_t start_time;
    
    /* 统计信息 */
    size_t max_connections_used;
    size_t total_connections_created;
} test_context_t;

/* 每个请求的上下文：保存 key/value 和指向测试上下文的指针 */
typedef struct {
    test_context_t* ctx;
    char key[64];
    char value[64];
} pool_request_t;

/* ===== 统计 & 回调 ===== */

static void test_response(vox_redis_client_t* client,
                         const vox_redis_response_t* response,
                         void* user_data) {
    (void)client;
    test_context_t* ctx = (test_context_t*)user_data;
    
    if (response->type == VOX_REDIS_RESPONSE_ERROR) {
        ctx->failed_requests++;
    } else {
        ctx->completed_requests++;
    }
    
    /* 更新统计信息：使用 current_size / available 估算在用连接数 */
    size_t current = vox_redis_pool_current_size(ctx->pool);
    size_t available = vox_redis_pool_available(ctx->pool);
    size_t in_use = (current > available) ? (current - available) : 0;
    if (in_use > ctx->max_connections_used) {
        ctx->max_connections_used = in_use;
    }
    
    /* 所有请求完成 */
    if (ctx->completed_requests + ctx->failed_requests >= ctx->total_requests) {
        clock_t end_time = clock();
        double elapsed = (double)(end_time - ctx->start_time) / CLOCKS_PER_SEC;
        
        size_t init_sz = vox_redis_pool_initial_size(ctx->pool);
        size_t max_sz = vox_redis_pool_max_size(ctx->pool);
        size_t final_sz = vox_redis_pool_current_size(ctx->pool);
        
        printf("\n=== %s 测试结果 ===\n", ctx->pool_name);
        printf("总请求数: %d\n", ctx->total_requests);
        printf("成功: %d\n", ctx->completed_requests);
        printf("失败: %d\n", ctx->failed_requests);
        printf("耗时: %.2f 秒\n", elapsed);
        printf("吞吐量: %.2f 请求/秒\n", ctx->total_requests / elapsed);
        printf("\n连接池统计:\n");
        printf("  初始连接数: %zu\n", init_sz);
        printf("  最大连接数: %zu\n", max_sz);
        printf("  最大使用连接: %zu\n", ctx->max_connections_used);
        printf("  最终连接数: %zu\n", final_sz);
        printf("  可用连接数: %zu\n", vox_redis_pool_available(ctx->pool));
        printf("\n资源效率:\n");
        printf("  连接利用率: %.1f%%\n",
               (ctx->max_connections_used * 100.0) / (double)(max_sz ? max_sz : 1));
        printf("  空闲连接数: %zu\n", vox_redis_pool_available(ctx->pool));

        /* 连接池会保持 TCP 连接为“活跃句柄”，VOX_RUN_DEFAULT 不会自动退出 */
        if (ctx->loop) {
            vox_loop_stop(ctx->loop);
        }
    }
}

static void test_error(vox_redis_client_t* client,
                      const char* message,
                      void* user_data) {
    (void)client;
    test_context_t* ctx = (test_context_t*)user_data;
    ctx->failed_requests++;
    printf("Error: %s\n", message);
}

/* 单个请求的命令响应回调：转调统计回调并归还连接 */
static void pool_request_response_cb(vox_redis_client_t* client,
                                     const vox_redis_response_t* response,
                                     void* user_data) {
    pool_request_t* req = (pool_request_t*)user_data;
    test_response(client, response, req->ctx);
    vox_redis_pool_release(req->ctx->pool, client);
    free(req);
}

/* 单个请求的错误回调：转调统计回调并归还连接 */
static void pool_request_error_cb(vox_redis_client_t* client,
                                  const char* message,
                                  void* user_data) {
    pool_request_t* req = (pool_request_t*)user_data;
    test_error(client, message, req->ctx);
    if (client) {
        vox_redis_pool_release(req->ctx->pool, client);
    }
    free(req);
}

/* acquire 回调：拿到连接后发送 SET 命令 */
static void pool_acquire_cb(vox_redis_pool_t* pool,
                            vox_redis_client_t* client,
                            int status,
                            void* user_data) {
    (void)pool;
    pool_request_t* req = (pool_request_t*)user_data;
    //test_context_t* ctx = req->ctx;

    if (status != 0 || !client) {
        pool_request_error_cb(NULL, "acquire connection failed", req);
        return;
    }

    int rc = vox_redis_client_set(client,
                                  req->key,
                                  req->value,
                                  pool_request_response_cb,
                                  req);
    if (rc != 0) {
        pool_request_error_cb(client, "redis SET failed", req);
    }
}

/* ===== 测试执行 ===== */

static void run_test(test_context_t* ctx) {
    printf("\n开始测试: %s\n", ctx->pool_name);
    printf("[debug] build=%s %s\n", __DATE__, __TIME__);
    printf("[debug] ctx=%p, pool=%p, loop=%p\n", (void*)ctx, (void*)ctx->pool, (void*)ctx->loop);
    printf("配置:\n");
    size_t init_sz = vox_redis_pool_initial_size(ctx->pool);
    size_t max_sz = vox_redis_pool_max_size(ctx->pool);
    printf("  初始连接: %zu\n", init_sz);
    printf("  最大连接: %zu\n", max_sz);
    printf("  测试请求: %d\n", ctx->total_requests);
    printf("\n");

    if (ctx->pool && init_sz == 0 && max_sz == 0) {
        printf("[debug] WARNING: pool handle non-NULL but sizes are 0. This strongly suggests stale binary or memory corruption.\n");
    }
    
    ctx->start_time = clock();
    
    /* 发送测试请求：每个请求独立 acquire -> SET -> release */
    for (int i = 0; i < ctx->total_requests; i++) {
        pool_request_t* req = (pool_request_t*)malloc(sizeof(pool_request_t));
        if (!req) {
            test_error(NULL, "out of memory for request", ctx);
            continue;
        }
        req->ctx = ctx;
        snprintf(req->key, sizeof(req->key), "test_key_%d", i);
        snprintf(req->value, sizeof(req->value), "test_value_%d", i);

        if (vox_redis_pool_acquire_async(ctx->pool, pool_acquire_cb, req) != 0) {
            pool_request_error_cb(NULL, "acquire_async failed", req);
        }
    }
}

/* ===== 连接池就绪回调 ===== */

static void pool_ready_fixed(vox_redis_pool_t* pool, int status, void* user_data) {
    test_context_t* ctx = (test_context_t*)user_data;
    ctx->pool = pool;

    if (status != 0) {
        printf("\n错误: 连接池初始化失败 (%s)\n", ctx->pool_name);
        printf("提示: 请确保 Redis 服务器正在运行 (默认地址: 127.0.0.1:6379)\n");
        printf("      如果 Redis 运行在其他地址，请修改代码中的连接参数\n\n");
        if (ctx->loop) {
            vox_loop_stop(ctx->loop);
        }
        return;
    }
    
    run_test(ctx);
}

static void pool_ready_dynamic(vox_redis_pool_t* pool, int status, void* user_data) {
    test_context_t* ctx = (test_context_t*)user_data;
    ctx->pool = pool;

    if (status != 0) {
        printf("\n错误: 连接池初始化失败 (%s)\n", ctx->pool_name);
        printf("提示: 请确保 Redis 服务器正在运行 (默认地址: 127.0.0.1:6379)\n");
        printf("      如果 Redis 运行在其他地址，请修改代码中的连接参数\n\n");
        if (ctx->loop) {
            vox_loop_stop(ctx->loop);
        }
        return;
    }
    
    run_test(ctx);
}

/* ===== 主程序 ===== */

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    printf("=== Redis 连接池性能对比测试 ===\n\n");
    printf("测试配置:\n");
    printf("  总请求数: %d\n", TEST_REQUESTS);
    printf("  预期并发: %d\n\n", TEST_CONCURRENT);

    /* 确保 WinSock 已初始化（Windows 需要） */
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return -1;
    }
    
    /* ===== 测试 1: 固定大小连接池 ===== */
    printf("===== 测试 1: 固定大小连接池 =====\n");
    
    test_context_t ctx_fixed = {0};
    ctx_fixed.pool_name = "固定连接池（50个连接）";
    ctx_fixed.total_requests = TEST_REQUESTS;
    
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "无法创建事件循环\n");
        return 1;
    }
    ctx_fixed.loop = loop;

    /* 创建固定大小连接池 */
    ctx_fixed.pool = vox_redis_pool_create(
        loop,
        "localhost",
        6379,
        50,   /* initial_size */
        50,   /* max_size 固定 50 个连接 */
        pool_ready_fixed,
        &ctx_fixed
    );
    
    if (!ctx_fixed.pool) {
        fprintf(stderr, "无法创建固定连接池\n");
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 运行测试 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    vox_redis_pool_destroy(ctx_fixed.pool);
    vox_loop_destroy(loop);
    
    printf("\n等待 2 秒后开始下一个测试...\n");
    /* 这里应该使用定时器，简化为直接运行 */
    
    /* ===== 测试 2: 动态连接池（相同最大连接数） ===== */
    printf("\n===== 测试 2: 动态连接池（初始10，最大50） =====\n");
    
    test_context_t ctx_dynamic = {0};
    ctx_dynamic.pool_name = "动态连接池（初始10，最大50）";
    ctx_dynamic.total_requests = TEST_REQUESTS;

    loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "无法创建事件循环\n");
        return 1;
    }
    ctx_dynamic.loop = loop;
    
    /* 创建动态连接池 */
    ctx_dynamic.pool = vox_redis_pool_create(
        loop,
        "127.0.0.1",
        6379,
        10,   /* initial_size */
        50,   /* max_size */
        pool_ready_dynamic,
        &ctx_dynamic
    );
    
    if (!ctx_dynamic.pool) {
        fprintf(stderr, "无法创建动态连接池\n");
        vox_loop_destroy(loop);
        return 1;
    }
    printf("[debug] created dynamic pool=%p (loop=%p)\n", (void*)ctx_dynamic.pool, (void*)loop);
    
    /* 运行测试 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    vox_redis_pool_destroy(ctx_dynamic.pool);
    vox_loop_destroy(loop);
    
    /* ===== 测试 3: 小型动态连接池 ===== */
    printf("\n===== 测试 3: 小型动态连接池（初始3，最大100） =====\n");
    
    test_context_t ctx_small = {0};
    ctx_small.pool_name = "小型动态连接池（初始3，最大100）";
    ctx_small.total_requests = TEST_REQUESTS;

    loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "无法创建事件循环\n");
        return 1;
    }
    ctx_small.loop = loop;
    
    /* 创建小型动态连接池 */
    ctx_small.pool = vox_redis_pool_create(
        loop,
        "127.0.0.1",
        6379,
        3,    /* initial_size */
        100,  /* max_size 高弹性 */
        pool_ready_dynamic,
        &ctx_small
    );
    
    if (!ctx_small.pool) {
        fprintf(stderr, "无法创建小型动态连接池\n");
        vox_loop_destroy(loop);
        return 1;
    }
    printf("[debug] created small pool=%p (loop=%p)\n", (void*)ctx_small.pool, (void*)loop);
    
    /* 运行测试 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    vox_redis_pool_destroy(ctx_small.pool);
    vox_loop_destroy(loop);
    
    /* ===== 总结对比 ===== */
    printf("\n===== 性能对比总结 =====\n\n");
    
    printf("%-30s | %8s | %8s | %10s | %8s\n",
           "连接池类型", "初始", "最大", "实际峰值", "利用率");
    printf("-----------------------------------------------------------------------\n");
    printf("%-30s | %8zu | %8zu | %10zu | %7.1f%%\n",
           ctx_fixed.pool_name,
           (size_t)50, (size_t)50,
           ctx_fixed.max_connections_used,
           (ctx_fixed.max_connections_used * 100.0) / 50);
    printf("%-30s | %8zu | %8zu | %10zu | %7.1f%%\n",
           ctx_dynamic.pool_name,
           (size_t)10, (size_t)50,
           ctx_dynamic.max_connections_used,
           (ctx_dynamic.max_connections_used * 100.0) / 50);
    printf("%-30s | %8zu | %8zu | %10zu | %7.1f%%\n",
           ctx_small.pool_name,
           (size_t)3, (size_t)100,
           ctx_small.max_connections_used,
           (ctx_small.max_connections_used * 100.0) / 100);
    
    printf("\n结论:\n");
    printf("1. 固定连接池: 资源占用固定，适合并发量稳定的场景\n");
    printf("2. 动态连接池: 平衡资源占用和性能，适合大多数场景\n");
    printf("3. 小型动态池: 最小资源占用，高弹性，适合突发流量\n");
    
    vox_socket_cleanup();
    
    printf("\n测试完成\n");
    return 0;
}
