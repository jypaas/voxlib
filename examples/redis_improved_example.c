/*
 * redis_improved_example.c - 改进的 Redis 客户端示例
 * 展示新增的 API 和最佳实践
 */

#include "../redis/vox_redis_client.h"
#include "../vox_loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== 示例 1: 使用 commandv API（更安全） ===== */

static void on_commandv_response(vox_redis_client_t* client, 
                                 const vox_redis_response_t* response,
                                 void* user_data) {
    (void)client;
    (void)user_data;
    
    printf("使用 commandv API 收到响应:\n");
    if (response->type == VOX_REDIS_RESPONSE_BULK_STRING) {
        if (response->u.bulk_string.is_null) {
            printf("  结果: (nil)\n");
        } else {
            printf("  结果: %.*s\n", 
                   (int)response->u.bulk_string.len,
                   response->u.bulk_string.data);
        }
    }
}

static void example_commandv(vox_redis_client_t* client) {
    printf("\n=== 示例 1: 使用 commandv API ===\n");
    
    /* 使用数组方式传递参数，更安全，不需要 NULL 结尾 */
    const char* set_args[] = {"SET", "improved_key", "improved_value"};
    vox_redis_client_commandv(client, on_commandv_response, NULL, NULL, 
                              3, set_args);
    
    const char* get_args[] = {"GET", "improved_key"};
    vox_redis_client_commandv(client, on_commandv_response, NULL, NULL,
                              2, get_args);
}

/* ===== 示例 2: 响应数据复制（在回调外部使用） ===== */

typedef struct {
    vox_redis_response_t response_copy;
    vox_mpool_t* mpool;
    int copied;
} user_context_t;

static void on_response_to_copy(vox_redis_client_t* client,
                                const vox_redis_response_t* response,
                                void* user_data) {
    (void)client;
    user_context_t* ctx = (user_context_t*)user_data;
    
    printf("复制响应数据以在回调外部使用...\n");
    
    /* 复制响应数据 */
    if (vox_redis_response_copy(ctx->mpool, response, &ctx->response_copy) == 0) {
        ctx->copied = 1;
        printf("  响应已复制\n");
    } else {
        printf("  响应复制失败\n");
    }
}

static void example_response_copy(vox_redis_client_t* client, vox_loop_t* loop) {
    printf("\n=== 示例 2: 响应数据复制 ===\n");
    
    user_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mpool = vox_loop_get_mpool(loop);
    
    /* 设置并获取数据 */
    vox_redis_client_set(client, "copy_test", "data_to_copy", 
                        on_response_to_copy, &ctx);
    
    /* 注意: 在实际应用中，这里需要运行事件循环 */
    /* 然后在循环外部可以安全访问 ctx.response_copy */
    
    /* 使用完毕后释放 */
    if (ctx.copied) {
        printf("清理复制的响应数据...\n");
        vox_redis_response_free(ctx.mpool, &ctx.response_copy);
    }
}

/* ===== 示例 3: 错误处理 ===== */

static void on_error(vox_redis_client_t* client,
                     const char* message,
                     void* user_data) {
    (void)client;
    (void)user_data;
    printf("错误回调: %s\n", message);
}

static void on_response_with_error(vox_redis_client_t* client,
                                   const vox_redis_response_t* response,
                                   void* user_data) {
    (void)client;
    (void)user_data;
    
    if (response->type == VOX_REDIS_RESPONSE_ERROR) {
        printf("Redis 错误: %.*s\n",
               (int)response->u.error.len,
               response->u.error.message);
    } else {
        printf("命令成功执行\n");
    }
}

static void example_error_handling(vox_redis_client_t* client) {
    printf("\n=== 示例 3: 错误处理 ===\n");
    
    /* 使用错误回调 */
    vox_redis_client_command(client, on_response_with_error, on_error, NULL,
                            "WRONGCMD", NULL);
}

/* ===== 示例 4: 数组响应处理 ===== */

static void on_array_response(vox_redis_client_t* client,
                              const vox_redis_response_t* response,
                              void* user_data) {
    (void)client;
    (void)user_data;
    
    printf("数组响应:\n");
    if (response->type == VOX_REDIS_RESPONSE_ARRAY) {
        printf("  元素个数: %zu\n", response->u.array.count);
        for (size_t i = 0; i < response->u.array.count; i++) {
            const vox_redis_response_t* elem = &response->u.array.elements[i];
            printf("  [%zu] ", i);
            switch (elem->type) {
                case VOX_REDIS_RESPONSE_BULK_STRING:
                    if (elem->u.bulk_string.is_null) {
                        printf("(nil)\n");
                    } else {
                        printf("%.*s\n",
                               (int)elem->u.bulk_string.len,
                               elem->u.bulk_string.data);
                    }
                    break;
                case VOX_REDIS_RESPONSE_INTEGER:
                    printf("%lld\n", (long long)elem->u.integer);
                    break;
                default:
                    printf("(其他类型)\n");
                    break;
            }
        }
    }
}

static void example_array_handling(vox_redis_client_t* client) {
    printf("\n=== 示例 4: 数组响应处理 ===\n");
    
    /* 添加集合成员 */
    vox_redis_client_sadd(client, "myset", "member1", NULL, NULL);
    vox_redis_client_sadd(client, "myset", "member2", NULL, NULL);
    vox_redis_client_sadd(client, "myset", "member3", NULL, NULL);
    
    /* 获取所有成员（返回数组） */
    vox_redis_client_smembers(client, "myset", on_array_response, NULL);
}

/* ===== 连接回调 ===== */

static void on_connect(vox_redis_client_t* client, int status, void* user_data) {
    vox_loop_t* loop = (vox_loop_t*)user_data;
    
    if (status != 0) {
        printf("连接失败!\n");
        vox_loop_stop(loop);
        return;
    }
    
    printf("已连接到 Redis 服务器\n");
    
    /* 运行所有示例 */
    example_commandv(client);
    example_response_copy(client, loop);
    example_error_handling(client);
    example_array_handling(client);
    
    /* 停止循环（实际应用中可能需要持续运行） */
    /* vox_loop_stop(loop); */
}

/* ===== 主程序 ===== */

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = 6379;
    
    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = (uint16_t)atoi(argv[2]);
    }
    
    printf("=== Redis 客户端改进示例 ===\n");
    printf("连接到 %s:%u\n", host, port);
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "无法创建事件循环\n");
        return 1;
    }
    
    /* 创建 Redis 客户端 */
    vox_redis_client_t* client = vox_redis_client_create(loop);
    if (!client) {
        fprintf(stderr, "无法创建 Redis 客户端\n");
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 连接到服务器 */
    if (vox_redis_client_connect(client, host, port, on_connect, loop) != 0) {
        fprintf(stderr, "连接失败\n");
        vox_redis_client_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 运行事件循环 */
    printf("运行事件循环...\n");
    vox_loop_run(loop);
    
    /* 清理 */
    vox_redis_client_destroy(client);
    vox_loop_destroy(loop);
    
    printf("\n程序结束\n");
    return 0;
}
