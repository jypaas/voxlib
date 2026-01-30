/*
 * websocket_echo_client.c - WebSocket Echo 客户端示例
 * 演示如何创建一个简单的 WebSocket 客户端，支持 WS 和 WSS
 */

#include "../websocket/vox_websocket_client.h"
#include "../vox_loop.h"
#include "../vox_log.h"
#include <stdio.h>
#include <string.h>

static vox_loop_t* g_loop = NULL;
static int g_message_count = 0;

/* 连接成功回调 */
static void on_connect(vox_ws_client_t* client, void* user_data) {
    (void)user_data;
    
    printf("Connected to WebSocket server\n");
    
    /* 发送测试消息 */
    const char* message = "Hello, WebSocket!";
    vox_ws_client_send_text(client, message, strlen(message));
    printf("Sent: %s\n", message);
}

/* 消息回调 */
static void on_message(vox_ws_client_t* client, const void* data, size_t len,
                       vox_ws_message_type_t type, void* user_data) {
    (void)user_data;
    
    if (type == VOX_WS_MSG_TEXT) {
        printf("Received text message: %.*s\n", (int)len, (const char*)data);
    } else {
        printf("Received binary message (%zu bytes)\n", len);
    }
    
    g_message_count++;
    
    /* 收到 5 条消息后关闭 */
    if (g_message_count >= 5) {
        printf("Closing connection...\n");
        vox_ws_client_close(client, VOX_WS_CLOSE_NORMAL, "Test completed");
    } else {
        /* 继续发送消息 */
        char msg[64];
        snprintf(msg, sizeof(msg), "Message #%d", g_message_count + 1);
        vox_ws_client_send_text(client, msg, strlen(msg));
        printf("Sent: %s\n", msg);
    }
}

/* 关闭回调 */
static void on_close(vox_ws_client_t* client, uint16_t code, const char* reason, void* user_data) {
    (void)client;
    (void)user_data;
    
    printf("Connection closed: code=%u, reason=%s\n", code, reason);
    
    /* 停止事件循环 */
    if (g_loop) {
        vox_loop_stop(g_loop);
    }
}

/* 错误回调 */
static void on_error(vox_ws_client_t* client, const char* error, void* user_data) {
    (void)client;
    (void)user_data;
    
    fprintf(stderr, "WebSocket error: %s\n", error);
    
    /* 停止事件循环 */
    if (g_loop) {
        vox_loop_stop(g_loop);
    }
}

int main(int argc, char* argv[]) {
    /* 解析命令行参数 */
    const char* url = "ws://127.0.0.1:8080";
    
    if (argc > 1) {
        url = argv[1];
    }

    /* Windows 下使用网络前必须初始化 Winsock */
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }
    
    printf("Connecting to %s\n", url);
    
    /* 创建事件循环 */
    g_loop = vox_loop_create();
    if (!g_loop) {
        fprintf(stderr, "Failed to create event loop\n");
        vox_socket_cleanup();
        return 1;
    }
    
    /* 配置 WebSocket 客户端 */
    vox_ws_client_config_t config = {0};
    config.loop = g_loop;
    config.url = url;
    config.on_connect = on_connect;
    config.on_message = on_message;
    config.on_close = on_close;
    config.on_error = on_error;
    
    /* 创建 SSL 上下文（用于 WSS） */
    if (strncmp(url, "wss://", 6) == 0) {
        config.ssl_ctx = vox_ssl_context_create(vox_loop_get_mpool(g_loop), VOX_SSL_MODE_CLIENT);
        if (!config.ssl_ctx) {
            fprintf(stderr, "Failed to create SSL context\n");
            vox_loop_destroy(g_loop);
            vox_socket_cleanup();
            return 1;
        }
        
        /* 配置 SSL（开发环境可以不验证证书） */
        vox_ssl_config_t ssl_config = {
            .verify_peer = false,      /* 生产环境应设为 true */
            .verify_hostname = false   /* 生产环境应设为 true */
        };
        
        if (vox_ssl_context_configure(config.ssl_ctx, &ssl_config) != 0) {
            fprintf(stderr, "Failed to configure SSL context\n");
            vox_ssl_context_destroy(config.ssl_ctx);
            vox_loop_destroy(g_loop);
            vox_socket_cleanup();
            return 1;
        }
    }
    
    /* 创建客户端 */
    vox_ws_client_t* client = vox_ws_client_create(&config);
    if (!client) {
        fprintf(stderr, "Failed to create WebSocket client\n");
        if (config.ssl_ctx) {
            vox_ssl_context_destroy(config.ssl_ctx);
        }
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return 1;
    }
    
    /* 连接到服务器 */
    if (vox_ws_client_connect(client) != 0) {
        fprintf(stderr, "Failed to connect to server\n");
        vox_ws_client_destroy(client);
        if (config.ssl_ctx) {
            vox_ssl_context_destroy(config.ssl_ctx);
        }
        vox_loop_destroy(g_loop);
        vox_socket_cleanup();
        return 1;
    }
    
    printf("Client started. Waiting for connection...\n");
    
    /* 运行事件循环 */
    vox_loop_run(g_loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_ws_client_destroy(client);
    if (config.ssl_ctx) {
        vox_ssl_context_destroy(config.ssl_ctx);
    }
    vox_loop_destroy(g_loop);
    
    printf("Client exited\n");
    vox_socket_cleanup();
    return 0;
}
