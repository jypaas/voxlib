/*
 * coroutine_clients_example.c - 协程客户端使用示例
 * 展示如何使用文件系统、Redis、HTTP和WebSocket的协程客户端
 */

#include "../coroutine/vox_coroutine.h"
#include "../coroutine/vox_coroutine_fs.h"
#include "../coroutine/vox_coroutine_redis.h"
#include "../coroutine/vox_coroutine_http.h"
#include "../coroutine/vox_coroutine_ws.h"
#include "../vox_loop.h"
#include "../vox_log.h"
#include "../vox_mpool.h"
#include "../vox_socket.h"
#include "../redis/vox_redis_client.h"
#include "../redis/vox_redis_pool.h"
#include <stdio.h>
#include <string.h>

/* ===== 文件系统协程示例 ===== */

VOX_COROUTINE_ENTRY(fs_example_coroutine, void* user_data) {
    vox_loop_t* loop = (vox_loop_t*)user_data;
    const char* filename = "test_file.txt";
    const char* content = "Hello from coroutine filesystem!";
    
    printf("[FS] Writing file...\n");
    if (vox_coroutine_fs_write_file_await(co, filename, content, strlen(content)) < 0) {
        printf("[FS] Failed to write file\n");
        if (loop) vox_loop_stop(loop);
        return;
    }
    printf("[FS] File written successfully\n");
    
    printf("[FS] Reading file...\n");
    void* data = NULL;
    size_t size = 0;
    if (vox_coroutine_fs_read_file_await(co, filename, &data, &size) < 0) {
        printf("[FS] Failed to read file\n");
        if (loop) vox_loop_stop(loop);
        return;
    }
    printf("[FS] File content: %.*s\n", (int)size, (char*)data);
    vox_coroutine_fs_free_file_data(co, data);
    printf("[FS] Example completed\n");
    if (loop)
        vox_loop_stop(loop);
}

/* ===== Redis协程示例 ===== */

typedef struct {
    vox_loop_t* loop;
    vox_redis_client_t* client;
} redis_example_ctx_t;

VOX_COROUTINE_ENTRY(redis_example_coroutine, void* user_data) {
    redis_example_ctx_t* ctx = (redis_example_ctx_t*)user_data;
    vox_redis_client_t* client = ctx->client;
    vox_redis_response_t response = {0};
    
    /* 连接到Redis */
    printf("[Redis] Connecting...\n");
    if (vox_coroutine_redis_connect_await(co, client, "127.0.0.1", 6379) < 0) {
        printf("[Redis] Failed to connect\n");
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    printf("[Redis] Connected successfully\n");
    
    /* PING测试 */
    printf("[Redis] Sending PING...\n");
    if (vox_coroutine_redis_ping_await(co, client, &response) < 0) {
        printf("[Redis] PING failed\n");
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    printf("[Redis] PONG received\n");
    
    /* SET操作 */
    printf("[Redis] Setting key 'mykey'...\n");
    if (vox_coroutine_redis_set_await(co, client, "mykey", "Hello Redis", &response) < 0) {
        printf("[Redis] SET failed\n");
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    printf("[Redis] SET successful\n");
    
    /* GET操作 */
    printf("[Redis] Getting key 'mykey'...\n");
    if (vox_coroutine_redis_get_await(co, client, "mykey", &response) < 0) {
        printf("[Redis] GET failed\n");
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    
    if (response.type == VOX_REDIS_RESPONSE_BULK_STRING && !response.u.bulk_string.is_null) {
        printf("[Redis] Value: %.*s\n", 
               (int)response.u.bulk_string.len, 
               response.u.bulk_string.data);
    }
    
    /* 释放响应数据 */
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (mpool) {
        vox_redis_response_free(mpool, &response);
    }
    
    /* INCR操作 */
    printf("[Redis] Incrementing counter...\n");
    for (int i = 0; i < 5; i++) {
        if (vox_coroutine_redis_incr_await(co, client, "counter", &response) < 0) {
            printf("[Redis] INCR failed\n");
            if (ctx->loop) vox_loop_stop(ctx->loop);
            return;
        }
        if (response.type == VOX_REDIS_RESPONSE_INTEGER) {
            printf("[Redis] Counter value: %lld\n", (long long)response.u.integer);
        }
        /* 释放响应数据 */
        if (mpool) {
            vox_redis_response_free(mpool, &response);
        }
    }
    
    printf("[Redis] Example completed\n");
    /* 协程结束后停止事件循环，否则 TCP 连接仍为活跃句柄导致 loop 不退出 */
    if (ctx->loop)
        vox_loop_stop(ctx->loop);
}

/* ===== Redis 连接池协程示例 ===== */

typedef struct {
    vox_loop_t* loop;
    vox_redis_pool_t* pool;
} redis_pool_example_ctx_t;

VOX_COROUTINE_ENTRY(redis_pool_example_coroutine, void* user_data) {
    redis_pool_example_ctx_t* ctx = (redis_pool_example_ctx_t*)user_data;
    vox_redis_response_t response = {0};
    vox_loop_t* loop = ctx->loop;
    vox_mpool_t* mpool = loop ? vox_loop_get_mpool(loop) : NULL;

    /* 通过连接池 PING */
    printf("[Redis Pool] PING...\n");
    if (vox_coroutine_redis_pool_ping_await(co, ctx->pool, &response) < 0) {
        printf("[Redis Pool] PING failed\n");
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    printf("[Redis Pool] PONG\n");
    if (mpool) vox_redis_response_free(mpool, &response);

    /* 通过连接池 SET */
    printf("[Redis Pool] SET pool_key = Hello Pool\n");
    if (vox_coroutine_redis_pool_set_await(co, ctx->pool, "pool_key", "Hello Pool", &response) < 0) {
        printf("[Redis Pool] SET failed\n");
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    if (mpool) vox_redis_response_free(mpool, &response);

    /* 通过连接池 GET */
    printf("[Redis Pool] GET pool_key...\n");
    if (vox_coroutine_redis_pool_get_await(co, ctx->pool, "pool_key", &response) < 0) {
        printf("[Redis Pool] GET failed\n");
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    if (response.type == VOX_REDIS_RESPONSE_BULK_STRING && !response.u.bulk_string.is_null) {
        printf("[Redis Pool] Value: %.*s\n",
               (int)response.u.bulk_string.len,
               response.u.bulk_string.data);
    }
    if (mpool) vox_redis_response_free(mpool, &response);

    /* 演示：取连接后执行多条命令再归还 */
    printf("[Redis Pool] 取连接执行多条命令...\n");
    memset(&response, 0, sizeof(response));
    vox_redis_client_t* client = NULL;
    if (vox_coroutine_redis_pool_acquire_await(co, ctx->pool, &client) < 0) {
        printf("[Redis Pool] acquire failed\n");
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    if (vox_coroutine_redis_incr_await(co, client, "pool_counter", &response) < 0) {
        vox_redis_pool_release(ctx->pool, client);
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    printf("[Redis Pool] INCR pool_counter => %lld\n", (long long)response.u.integer);
    if (mpool) vox_redis_response_free(mpool, &response);
    vox_redis_pool_release(ctx->pool, client);

    printf("[Redis Pool] Example completed\n");
    if (ctx->loop)
        vox_loop_stop(ctx->loop);
}

static void on_redis_pool_ready(vox_redis_pool_t* pool, int status, void* user_data) {
    redis_pool_example_ctx_t* ctx = (redis_pool_example_ctx_t*)user_data;
    ctx->pool = pool;  /* 可能在 create 返回前即调用，故从参数取得 pool */
    if (status != 0) {
        printf("[Redis Pool] 连接池初始化失败\n");
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    printf("[Redis Pool] 连接池已就绪 (初始: %zu, 最大: %zu)\n",
           vox_redis_pool_initial_size(pool),
           vox_redis_pool_max_size(pool));
    VOX_COROUTINE_START(ctx->loop, redis_pool_example_coroutine, ctx);
}

/* ===== HTTP协程示例 ===== */

typedef struct {
    vox_loop_t* loop;
    vox_http_client_t* client;
} http_example_ctx_t;

VOX_COROUTINE_ENTRY(http_example_coroutine, void* user_data) {
    http_example_ctx_t* ctx = (http_example_ctx_t*)user_data;
    vox_http_client_t* client = ctx->client;
    vox_coroutine_http_response_t response = {0};
    
    printf("[HTTP] Sending GET request...\n");
    if (vox_coroutine_http_get_await(co, client, "http://httpbin.org/get", &response) < 0) {
        printf("[HTTP] GET request failed: %s\n", 
               response.error_message ? response.error_message : "unknown error");
        vox_coroutine_http_response_free(&response);
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    
    printf("[HTTP] Status: %d\n", response.status_code);
    printf("[HTTP] Headers:\n");
    for (size_t i = 0; i < response.header_count; i++) {
        printf("  %s: %s\n", response.headers[i].name, response.headers[i].value);
    }
    printf("[HTTP] Body length: %zu bytes\n", response.body_len);
    
    vox_coroutine_http_response_free(&response);
    
    printf("[HTTP] Sending POST JSON request...\n");
    const char* json = "{\"message\":\"Hello from coroutine\",\"value\":42}";
    if (vox_coroutine_http_post_json_await(co, client, "http://httpbin.org/post", json, &response) < 0) {
        printf("[HTTP] POST request failed\n");
        vox_coroutine_http_response_free(&response);
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    
    printf("[HTTP] POST Status: %d\n", response.status_code);
    printf("[HTTP] POST Body length: %zu bytes\n", response.body_len);
    
    vox_coroutine_http_response_free(&response);
    
    printf("[HTTP] Example completed\n");
    if (ctx->loop)
        vox_loop_stop(ctx->loop);
}

/* ===== WebSocket协程示例 ===== */

VOX_COROUTINE_ENTRY(websocket_example_coroutine, void* user_data) {
    vox_loop_t* loop = (vox_loop_t*)user_data;
    vox_coroutine_ws_client_t* ws_client = NULL;
    
    /* 使用 127.0.0.1 避免 Windows 上 localhost 解析为 IPv6 导致连不上 IPv4 监听 */
    printf("[WebSocket] Connecting to echo server...\n");
    if (vox_coroutine_ws_connect_await(co, loop, "ws://127.0.0.1:8080", &ws_client) < 0) {
        printf("[WebSocket] Connection failed (ensure websocket_echo_server is running, e.g. .\\bin\\Debug\\websocket_echo_server.exe)\n");
        if (loop) vox_loop_stop(loop);
        return;
    }
    printf("[WebSocket] Connected successfully\n");
    
    /* 发送消息 */
    const char* messages[] = {
        "Hello WebSocket",
        "This is a test",
        "From coroutine client"
    };
    
    for (int i = 0; i < 3; i++) {
        printf("[WebSocket] Sending: %s\n", messages[i]);
        if (vox_coroutine_ws_send_text_await(co, ws_client, messages[i], strlen(messages[i])) < 0) {
            printf("[WebSocket] Send failed\n");
            break;
        }
        vox_coroutine_ws_message_t msg = {0};
        int ret = vox_coroutine_ws_recv_await(co, ws_client, &msg);
        if (ret == 0) {
            printf("[WebSocket] Received: %.*s\n", (int)msg.len, (char*)msg.data);
            vox_coroutine_ws_message_free(&msg);
        } else if (ret == 1) {
            printf("[WebSocket] Connection closed\n");
            break;
        } else {
            printf("[WebSocket] Receive failed\n");
            break;
        }
    }
    printf("[WebSocket] Closing connection...\n");
    vox_coroutine_ws_close_await(co, ws_client, 1000, "Normal closure");
    vox_coroutine_ws_disconnect(ws_client);
    printf("[WebSocket] Example completed\n");
    if (loop)
        vox_loop_stop(loop);
}

/* ===== 组合示例：在一个协程中使用多个客户端 ===== */

typedef struct {
    vox_loop_t* loop;
    vox_redis_client_t* redis;
    vox_http_client_t* http;
} combined_ctx_t;

VOX_COROUTINE_ENTRY(combined_example_coroutine, void* user_data) {
    combined_ctx_t* ctx = (combined_ctx_t*)user_data;
    
    printf("\n[Combined] Starting combined example...\n");
    
    /* 0. 先连接Redis */
    printf("[Combined] Step 0: Connecting to Redis...\n");
    if (vox_coroutine_redis_connect_await(co, ctx->redis, "127.0.0.1", 6379) < 0) {
        printf("[Combined] Failed to connect to Redis\n");
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    printf("[Combined] Redis connected successfully\n");
    fflush(stdout);
    
    /* 1. 从HTTP获取数据 */
    printf("[Combined] Step 1: Fetching data from HTTP API...\n");
    fflush(stdout);
    vox_coroutine_http_response_t http_response = {0};
    int http_ret = vox_coroutine_http_get_await(co, ctx->http, "http://httpbin.org/uuid", &http_response);
    printf("[Combined] HTTP GET await returned, ret=%d\n", http_ret);
    fflush(stdout);
    if (http_ret != 0) {
        printf("[Combined] HTTP GET failed: %s\n",
               http_response.error_message ? http_response.error_message : "unknown error (e.g. timeout or no network)");
        fflush(stdout);
        vox_coroutine_http_response_free(&http_response);
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    printf("[Combined] HTTP Status: %d\n", http_response.status_code);
    fflush(stdout);

    /* 2. 将数据保存到Redis */
    if (!http_response.body || http_response.body_len == 0) {
        printf("[Combined] Error: HTTP body empty, nothing to save to Redis\n");
        vox_coroutine_http_response_free(&http_response);
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    printf("[Combined] Step 2: Saving to Redis...\n");
    vox_redis_response_t redis_response = {0};
    vox_mpool_t* mpool = vox_loop_get_mpool(ctx->loop);
    if (vox_coroutine_redis_set_await(co, ctx->redis, "http_data",
                                      (const char*)http_response.body,
                                      &redis_response) != 0) {
        printf("[Combined] Redis SET failed\n");
        vox_coroutine_http_response_free(&http_response);
        if (mpool) vox_redis_response_free(mpool, &redis_response);
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    printf("[Combined] Data saved to Redis\n");
    if (mpool) {
        vox_redis_response_free(mpool, &redis_response);
        memset(&redis_response, 0, sizeof(redis_response));
    }

    /* 3. 从Redis读取验证 */
    printf("[Combined] Step 3: Verifying from Redis...\n");
    if (vox_coroutine_redis_get_await(co, ctx->redis, "http_data", &redis_response) != 0) {
        printf("[Combined] Redis GET failed\n");
        vox_coroutine_http_response_free(&http_response);
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }
    if (redis_response.type == VOX_REDIS_RESPONSE_BULK_STRING &&
        redis_response.u.bulk_string.data && !redis_response.u.bulk_string.is_null) {
        const char* data = redis_response.u.bulk_string.data;
        size_t n = redis_response.u.bulk_string.len;
        if (n > 4096) n = 4096;
        size_t end = 0;
        for (size_t i = 0; i < n; i++) {
            if (data[i] == '\0') {
                end = i;
                break;
            }
            if (data[i] == '}')
                end = i + 1;
        }
        if (end == 0)
            end = n;
        printf("[Combined] Verified data from Redis: %.*s\n", (int)end, data);
    }
    if (mpool) {
        vox_redis_response_free(mpool, &redis_response);
        memset(&redis_response, 0, sizeof(redis_response));
    }
    vox_coroutine_http_response_free(&http_response);

    /* 4. 写入文件日志 */
    printf("[Combined] Step 4: Writing log file...\n");
    const char* log_content = "Combined example completed successfully";
    if (vox_coroutine_fs_write_file_await(co, "combined_log.txt", log_content, strlen(log_content)) != 0) {
        printf("[Combined] Write log file failed\n");
        if (ctx->loop) vox_loop_stop(ctx->loop);
        return;
    }

    printf("[Combined] All steps completed!\n");
    if (ctx->loop)
        vox_loop_stop(ctx->loop);
}

/* ===== 主函数 ===== */

int main(int argc, char* argv[]) {
    /* 初始化日志 */
    vox_log_set_level(VOX_LOG_INFO);
    
    /* Windows 下使用网络前必须初始化 Winsock */
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "Failed to create loop\n");
        vox_socket_cleanup();
        return 1;
    }
    
    printf("=== Coroutine Clients Example ===\n\n");
    
    /* 选择示例 */
    int example = 0;
    if (argc > 1) {
        example = atoi(argv[1]);
    }
    
    switch (example) {
        case 1:
            printf("Running File System example...\n\n");
            VOX_COROUTINE_START(loop, fs_example_coroutine, loop);
            break;
            
        case 2: {
            printf("Running Redis example...\n\n");
            redis_example_ctx_t redis_ctx = { .loop = loop, .client = NULL };
            redis_ctx.client = vox_redis_client_create(loop);
            if (redis_ctx.client) {
                VOX_COROUTINE_START(loop, redis_example_coroutine, &redis_ctx);
            }
            vox_loop_run(loop, VOX_RUN_DEFAULT);
            if (redis_ctx.client) {
                vox_redis_client_destroy(redis_ctx.client);
            }
            vox_loop_destroy(loop);
            vox_socket_cleanup();
            return 0;
        }
        
        case 3: {
            printf("Running HTTP example...\n\n");
            http_example_ctx_t http_ctx = { .loop = loop, .client = NULL };
            http_ctx.client = vox_http_client_create(loop);
            if (http_ctx.client) {
                VOX_COROUTINE_START(loop, http_example_coroutine, &http_ctx);
            }
            vox_loop_run(loop, VOX_RUN_DEFAULT);
            if (http_ctx.client) {
                vox_http_client_destroy(http_ctx.client);
            }
            vox_loop_destroy(loop);
            vox_socket_cleanup();
            return 0;
        }
        
        case 4:
            printf("Running WebSocket example...\n\n");
            printf("Note: Make sure a WebSocket echo server is running on ws://127.0.0.1:8080\n");
            printf("You can run: ./bin/websocket_echo_server (Unix) or .\\bin\\Debug\\websocket_echo_server.exe (Windows)\n\n");
            VOX_COROUTINE_START(loop, websocket_example_coroutine, loop);
            vox_loop_run(loop, VOX_RUN_DEFAULT);
            vox_loop_destroy(loop);
            vox_socket_cleanup();
            return 0;
            
        case 5: {
            printf("Running Combined example...\n\n");
            combined_ctx_t ctx;
            ctx.loop = loop;
            ctx.redis = vox_redis_client_create(loop);
            ctx.http = vox_http_client_create(loop);
            
            if (ctx.redis && ctx.http) {
                /* 在 combined_example_coroutine 中连接 Redis */
                VOX_COROUTINE_START(loop, combined_example_coroutine, &ctx);
            }
            
            vox_loop_run(loop, VOX_RUN_DEFAULT);
            
            if (ctx.redis) vox_redis_client_destroy(ctx.redis);
            if (ctx.http) vox_http_client_destroy(ctx.http);
            vox_loop_destroy(loop);
            vox_socket_cleanup();
            return 0;
        }

        case 6: {
            printf("Running Redis Pool example...\n\n");
            redis_pool_example_ctx_t pool_ctx = { .loop = loop, .pool = NULL };
            vox_redis_pool_t* pool = vox_redis_pool_create(
                loop,
                "127.0.0.1",
                6379,
                2,   /* initial_size */
                8,   /* max_size */
                on_redis_pool_ready,
                &pool_ctx
            );
            if (!pool) {
                fprintf(stderr, "Failed to create Redis pool\n");
                vox_loop_destroy(loop);
                vox_socket_cleanup();
                return 1;
            }
            pool_ctx.pool = pool;
            vox_loop_run(loop, VOX_RUN_DEFAULT);
            vox_redis_pool_destroy(pool);
            vox_loop_destroy(loop);
            vox_socket_cleanup();
            return 0;
        }
        
        default:
            printf("Usage: %s <example_number>\n", argv[0]);
            printf("  1 - File System\n");
            printf("  2 - Redis (requires Redis server on 127.0.0.1:6379)\n");
            printf("  3 - HTTP (requires internet connection)\n");
            printf("  4 - WebSocket (requires WebSocket echo server on ws://127.0.0.1:8080)\n");
            printf("  5 - Combined (requires Redis and internet connection)\n");
            printf("  6 - Redis Pool (requires Redis server on 127.0.0.1:6379)\n");
            vox_loop_destroy(loop);
            vox_socket_cleanup();
            return 0;
    }
    
    /* 选项1（文件系统）会走到这里 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    vox_loop_destroy(loop);
    vox_socket_cleanup();
    return 0;
}
