/*
 * redis_pool_dynamic_example.c - Redis 动态连接池示例
 * 展示初始连接数和最大连接数的使用（纯连接管理）
 */

#include "../redis/vox_redis_pool.h"
#include "../vox_loop.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define EXAMPLE_CTX_MAGIC 0x43545854u  /* "CTXT" */

/* 共享上下文：用于在全部请求完成后停止事件循环 */
typedef struct {
    uint32_t magic;  /* EXAMPLE_CTX_MAGIC，用于校验 req->ctx 是否有效 */
    vox_loop_t* loop;
    vox_redis_pool_t* pool;
    int total_requests;
    int completed_requests;
} example_ctx_t;

/* 单个请求上下文 */
typedef struct {
    example_ctx_t* ctx;
    int index;
    char key[32];
    char value[32];
} example_req_t;

/* ===== 示例：动态连接池 ===== */

/* SET 命令的响应回调：处理成功/错误后归还连接并释放 req */
static void on_set_response(vox_redis_client_t* client,
                            const vox_redis_response_t* response,
                            void* user_data) {
    example_req_t* req = (example_req_t*)user_data;
    if (!req || !req->ctx || req->ctx->magic != EXAMPLE_CTX_MAGIC) {
        fprintf(stderr, "on_set_response: invalid req or ctx\n");
        return;
    }
    if (response->type == VOX_REDIS_RESPONSE_ERROR) {
        printf("Request %d error: %.*s\n", req->index,
               (int)response->u.error.len, response->u.error.message);
    } else if (response->type == VOX_REDIS_RESPONSE_SIMPLE_STRING) {
        printf("Response %d: %.*s\n", req->index,
               (int)response->u.simple_string.len,
               response->u.simple_string.data);
    }
    
    vox_redis_pool_release(req->ctx->pool, client);
    req->ctx->completed_requests++;
    if (req->ctx->completed_requests >= req->ctx->total_requests)
        vox_loop_stop(req->ctx->loop);
    free(req);
}

static void example_req_error(vox_redis_client_t* client,
                              const char* message,
                              void* user_data) {
    (void)client;
    example_req_t* req = (example_req_t*)user_data;
    if (!req || !req->ctx) {
        fprintf(stderr, "example_req_error: invalid req or req->ctx (user_data=%p)\n", (void*)user_data);
        return;
    }
    if (req->ctx->magic != EXAMPLE_CTX_MAGIC) {
        fprintf(stderr, "example_req_error: req->ctx invalid or use-after-free (ctx=%p, magic=0x%x)\n",
                (void*)req->ctx, (unsigned)req->ctx->magic);
        return;
    }
    printf("Request %d error: %s\n", req->index, message ? message : "unknown");
    req->ctx->completed_requests++;
    if (req->ctx->completed_requests >= req->ctx->total_requests)
        vox_loop_stop(req->ctx->loop);
}

static void example_acquire_cb(vox_redis_pool_t* pool,
                               vox_redis_client_t* client,
                               int status,
                               void* user_data) {
    example_req_t* req = (example_req_t*)user_data;
    (void)pool;

    if (!req || !req->ctx) {
        fprintf(stderr, "example_acquire_cb: invalid req or req->ctx (user_data=%p)\n", (void*)user_data);
        return;
    }
    if (req->ctx->magic != EXAMPLE_CTX_MAGIC) {
        fprintf(stderr, "example_acquire_cb: req->ctx invalid or use-after-free (ctx=%p, magic=0x%x)\n",
                (void*)req->ctx, (unsigned)req->ctx->magic);
        return;
    }

    if (status != 0 || !client) {
        example_req_error(NULL, "acquire connection failed", req);
        free(req);
        return;
    }

    int rc = vox_redis_client_set(client, req->key, req->value,
                                  on_set_response, req);
    if (rc != 0) {
        example_req_error(client, "redis SET failed", req);
        vox_redis_pool_release(req->ctx->pool, client);
        free(req);
    }
}

static void on_pool_ready(vox_redis_pool_t* pool, int status, void* user_data) {
    example_ctx_t* ctx = (example_ctx_t*)user_data;
    ctx->pool = pool;
    
    if (status != 0) {
        printf("连接池初始化失败!\n");
        return;
    }
    
    size_t init_sz = vox_redis_pool_initial_size(pool);
    size_t max_sz = vox_redis_pool_max_size(pool);
    size_t cur_sz = vox_redis_pool_current_size(pool);
    
    printf("连接池已就绪!\n");
    printf("  初始连接数: %zu\n", init_sz);
    printf("  最大连接数: %zu\n", max_sz);
    printf("  当前连接数: %zu\n", cur_sz);
    printf("  可用连接数: %zu\n", vox_redis_pool_available(pool));
    printf("\n");
    
    /* 发送多个请求，测试动态连接创建 */
    printf("发送 10 个请求（超过初始连接数）...\n");
    
    for (int i = 0; i < ctx->total_requests; i++) {
        example_req_t* req = (example_req_t*)malloc(sizeof(example_req_t));
        if (!req) {
            fprintf(stderr, "alloc example_req_t failed\n");
            continue;
        }
        req->ctx = ctx;
        req->index = i + 1;
        snprintf(req->key, sizeof(req->key), "test_key_%d", i);
        snprintf(req->value, sizeof(req->value), "value_%d", i);
        
        if (vox_redis_pool_acquire_async(pool, example_acquire_cb, req) != 0) {
            example_req_error(NULL, "acquire_async failed", req);
            free(req);
        }
    }
    
    printf("请求已发送\n");
    printf("  当前连接数: %zu\n", vox_redis_pool_current_size(pool));
    printf("  可用连接数: %zu\n", vox_redis_pool_available(pool));
}

static void example_dynamic_pool(vox_loop_t* loop) {
    printf("=== 示例 1: 动态连接池 ===\n\n");
    
    example_ctx_t ctx = {
        .magic = EXAMPLE_CTX_MAGIC,
        .loop = loop,
        .pool = NULL,
        .total_requests = 10,
        .completed_requests = 0,
    };
    
    /*
     * 创建连接池：
     * - 初始连接数: 3 (永久连接)
     * - 最大连接数: 10 (可以动态创建 7 个临时连接)
     */
    vox_redis_pool_t* pool = vox_redis_pool_create(
        loop,
        "127.0.0.1",
        6379,
        3,   /* initial_size */
        10,  /* max_size */
        on_pool_ready,
        &ctx
    );
    
    if (!pool) {
        fprintf(stderr, "创建连接池失败\n");
        return;
    }
    ctx.pool = pool;
    
    /* 运行事件循环（全部请求完成后在 on_set_response 中调用 vox_loop_stop 退出） */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_redis_pool_destroy(pool);
}

/* ===== 主程序 ===== */

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    /* 确保 WinSock 已初始化（Windows 需要） */
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return -1;
    }

    printf("=== Redis 动态连接池示例 ===\n\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "无法创建事件循环\n");
        return 1;
    }
    
    /* 运行示例 */
    example_dynamic_pool(loop);
    
    /* 清理 */
    vox_loop_destroy(loop);
    vox_socket_cleanup();

    printf("\n程序结束\n");
    return 0;
}
