/*
 * redis_client_example.c - Redis 客户端示例
 *
 * 用法：
 *   redis_client_example [host] [port]
 *
 * 默认连接：
 *   host: 127.0.0.1
 *   port: 6379
 */

#include "../vox_socket.h"
#include "../vox_loop.h"
#include "../vox_log.h"
#include "../redis/vox_redis_client.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int command_count = 0;
static int completed_count = 0;

/* 前向声明 */
static void on_response(vox_redis_client_t* client, const vox_redis_response_t* response, void* user_data);

static void on_connect(vox_redis_client_t* client, int status, void* user_data) {
    vox_loop_t* loop = (vox_loop_t*)user_data;
    (void)client;
    
    if (status != 0) {
        VOX_LOG_ERROR("[redis] connect failed: %d", status);
        vox_loop_stop(loop);
        return;
    }
    
    VOX_LOG_INFO("[redis] connected");
    
    /* 执行一系列测试命令 */
    command_count = 0;
    completed_count = 0;
    
    /* PING */
    if (vox_redis_client_ping(client, on_response, loop) != 0) {
        VOX_LOG_ERROR("[redis] ping failed");
    } else {
        command_count++;
    }
    
    /* SET */
    if (vox_redis_client_set(client, "test_key", "test_value", on_response, loop) != 0) {
        VOX_LOG_ERROR("[redis] set failed");
    } else {
        command_count++;
    }
    
    /* GET */
    if (vox_redis_client_get(client, "test_key", on_response, loop) != 0) {
        VOX_LOG_ERROR("[redis] get failed");
    } else {
        command_count++;
    }
    
    /* HSET */
    if (vox_redis_client_hset(client, "test_hash", "field1", "value1", on_response, loop) != 0) {
        VOX_LOG_ERROR("[redis] hset failed");
    } else {
        command_count++;
    }
    
    /* HGET */
    if (vox_redis_client_hget(client, "test_hash", "field1", on_response, loop) != 0) {
        VOX_LOG_ERROR("[redis] hget failed");
    } else {
        command_count++;
    }
    
    /* LPUSH */
    if (vox_redis_client_lpush(client, "test_list", "item1", on_response, loop) != 0) {
        VOX_LOG_ERROR("[redis] lpush failed");
    } else {
        command_count++;
    }
    
    /* SADD */
    if (vox_redis_client_sadd(client, "test_set", "member1", on_response, loop) != 0) {
        VOX_LOG_ERROR("[redis] sadd failed");
    } else {
        command_count++;
    }
    
    VOX_LOG_INFO("[redis] sent %d commands", command_count);
}

static void on_response(vox_redis_client_t* client, const vox_redis_response_t* response, void* user_data) {
    vox_loop_t* loop = (vox_loop_t*)user_data;
    (void)client;
    
    completed_count++;
    
    if (!response) {
        VOX_LOG_ERROR("[redis] response is NULL");
        if (completed_count >= command_count) {
            vox_loop_stop(loop);
        }
        return;
    }
    
    switch (response->type) {
        case VOX_REDIS_RESPONSE_SIMPLE_STRING:
            VOX_LOG_INFO("[redis] Simple String: %.*s", 
                        (int)response->u.simple_string.len, 
                        response->u.simple_string.data);
            break;
            
        case VOX_REDIS_RESPONSE_ERROR:
            VOX_LOG_ERROR("[redis] Error: %.*s", 
                         (int)response->u.error.len, 
                         response->u.error.message);
            break;
            
        case VOX_REDIS_RESPONSE_INTEGER:
            VOX_LOG_INFO("[redis] Integer: %lld", (long long)response->u.integer);
            break;
            
        case VOX_REDIS_RESPONSE_BULK_STRING:
            if (response->u.bulk_string.is_null) {
                VOX_LOG_INFO("[redis] Bulk String: (null)");
            } else {
                VOX_LOG_INFO("[redis] Bulk String: %.*s", 
                            (int)response->u.bulk_string.len, 
                            response->u.bulk_string.data);
            }
            break;
            
        case VOX_REDIS_RESPONSE_ARRAY:
            VOX_LOG_INFO("[redis] Array: count=%zu", response->u.array.count);
            for (size_t i = 0; i < response->u.array.count; i++) {
                const vox_redis_response_t* elem = &response->u.array.elements[i];
                if (elem->type == VOX_REDIS_RESPONSE_BULK_STRING && !elem->u.bulk_string.is_null) {
                    VOX_LOG_INFO("[redis]   [%zu]: %.*s", i,
                                (int)elem->u.bulk_string.len,
                                elem->u.bulk_string.data);
                }
            }
            break;
            
        case VOX_REDIS_RESPONSE_NULL:
            VOX_LOG_INFO("[redis] NULL");
            break;
    }
    
    if (completed_count >= command_count) {
        VOX_LOG_INFO("[redis] all commands completed");
        vox_loop_stop(loop);
    }
}

int main(int argc, char** argv) {

    /* 确保 WinSock 已初始化（Windows 需要） */
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return -1;
    }

    const char* host = "127.0.0.1";
    uint16_t port = 6379;
    
    if (argc >= 2 && argv[1] && argv[1][0]) {
        host = argv[1];
    }
    if (argc >= 3 && argv[2] && argv[2][0]) {
        port = (uint16_t)atoi(argv[2]);
    }
    
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }
    
    vox_log_set_level(VOX_LOG_DEBUG);
    
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "vox_loop_create failed\n");
        return 1;
    }
    
    vox_redis_client_t* client = vox_redis_client_create(loop);
    if (!client) {
        fprintf(stderr, "vox_redis_client_create failed\n");
        vox_loop_destroy(loop);
        return 1;
    }
    
    VOX_LOG_INFO("[redis] connecting to %s:%d", host, port);
    
    if (vox_redis_client_connect(client, host, port, on_connect, loop) != 0) {
        fprintf(stderr, "vox_redis_client_connect failed\n");
        vox_redis_client_destroy(client);
        vox_loop_destroy(loop);
        return 1;
    }
    
    (void)vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    vox_redis_client_destroy(client);
    vox_loop_destroy(loop);
    vox_socket_cleanup();
    
    return 0;
}
