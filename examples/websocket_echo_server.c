/*
 * websocket_echo_server.c - WebSocket Echo 服务器示例
 * 演示如何创建一个简单的 WebSocket 服务器，支持 WS 和 WSS
 */

#include "../websocket/vox_websocket_server.h"
#include "../vox_loop.h"
#include "../vox_socket.h"
#include "../vox_log.h"
#include <stdio.h>
#include <string.h>

/* 连接回调 */
static void on_connection(vox_ws_connection_t* conn, void* user_data) {
    (void)user_data;
    
    vox_socket_addr_t addr;
    if (vox_ws_connection_getpeername(conn, &addr) == 0) {
        char ip[64];
        vox_socket_address_to_string(&addr, ip, sizeof(ip));
        printf("New WebSocket connection from %s:%d\n", ip, vox_socket_get_port(&addr));
    }
    
    /* 发送欢迎消息 */
    const char* welcome = "Welcome to WebSocket Echo Server!";
    vox_ws_connection_send_text(conn, welcome, strlen(welcome));
}

/* 消息回调 */
static void on_message(vox_ws_connection_t* conn, const void* data, size_t len,
                       vox_ws_message_type_t type, void* user_data) {
    (void)user_data;
    
    /* Echo 回消息 */
    if (type == VOX_WS_MSG_TEXT) {
        printf("Received text message (%zu bytes): %.*s\n", len, (int)len, (const char*)data);
        vox_ws_connection_send_text(conn, (const char*)data, len);
    } else {
        printf("Received binary message (%zu bytes)\n", len);
        vox_ws_connection_send_binary(conn, data, len);
    }
}

/* 关闭回调 */
static void on_close(vox_ws_connection_t* conn, uint16_t code, const char* reason, void* user_data) {
    (void)conn;
    (void)user_data;
    
    printf("WebSocket connection closed: code=%u, reason=%s\n", code, reason);
}

/* 错误回调 */
static void on_error(vox_ws_connection_t* conn, const char* error, void* user_data) {
    (void)conn;
    (void)user_data;
    
    printf("WebSocket error: %s\n", error);
}

int main(int argc, char* argv[]) {
    /* 解析命令行参数 */
    bool use_ssl = false;
    const char* host = "0.0.0.0";
    uint16_t port = 8080;

    /* Windows 下使用网络前必须初始化 Winsock */
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ssl") == 0) {
            use_ssl = true;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        }
    }
    
    printf("Starting WebSocket Echo Server...\n");
    printf("Protocol: %s\n", use_ssl ? "WSS" : "WS");
    printf("Listening on %s:%u\n", host, port);
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        vox_socket_cleanup();
        return 1;
    }
    
    /* 配置 WebSocket 服务器 */
    vox_ws_server_config_t config = {0};
    config.loop = loop;
    config.on_connection = on_connection;
    config.on_message = on_message;
    config.on_close = on_close;
    config.on_error = on_error;
    
    /* 创建服务器 */
    vox_ws_server_t* server = vox_ws_server_create(&config);
    if (!server) {
        fprintf(stderr, "Failed to create WebSocket server\n");
        vox_loop_destroy(loop);
        vox_socket_cleanup();
        return 1;
    }
    
    /* 解析监听地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, port, &addr) != 0) {
        fprintf(stderr, "Failed to parse address\n");
        vox_ws_server_destroy(server);
        vox_loop_destroy(loop);
        vox_socket_cleanup();
        return 1;
    }
    
    /* 开始监听 */
    int ret;
    if (use_ssl) {
        /* 创建 SSL 上下文（服务器模式） */
        vox_ssl_context_t* ssl_ctx = vox_ssl_context_create(vox_loop_get_mpool(loop), VOX_SSL_MODE_SERVER);
        if (!ssl_ctx) {
            fprintf(stderr, "Failed to create SSL context\n");
            vox_ws_server_destroy(server);
            vox_loop_destroy(loop);
            vox_socket_cleanup();
            return 1;
        }
        
        /* 配置证书和私钥 */
        vox_ssl_config_t ssl_config = {
            .cert_file = "cert/server.crt",
            .key_file = "cert/server.key",
            .verify_peer = false,
            .ciphers = NULL,  /* 使用默认密码套件 */
            .protocols = NULL /* 支持所有安全协议版本（TLSv1.2+） */
        };
        
        if (vox_ssl_context_configure(ssl_ctx, &ssl_config) != 0) {
            fprintf(stderr, "Failed to configure SSL context\n");
            fprintf(stderr, "Please make sure cert/server.crt and cert/server.key exist\n");
            vox_ssl_context_destroy(ssl_ctx);
            vox_ws_server_destroy(server);
            vox_loop_destroy(loop);
            vox_socket_cleanup();
            return 1;
        }
        
        ret = vox_ws_server_listen_ssl(server, &addr, 128, ssl_ctx);
    } else {
        ret = vox_ws_server_listen(server, &addr, 128);
    }
    
    if (ret != 0) {
        fprintf(stderr, "Failed to start listening\n");
        vox_ws_server_destroy(server);
        vox_loop_destroy(loop);
        vox_socket_cleanup();
        return 1;
    }
    
    printf("Server is running. Press Ctrl+C to stop.\n");
    printf("Test with: wscat -c %s://localhost:%u\n", use_ssl ? "wss" : "ws", port);
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_ws_server_destroy(server);
    vox_loop_destroy(loop);
    
    vox_socket_cleanup();
    return 0;
}
