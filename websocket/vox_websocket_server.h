/*
 * vox_websocket_server.h - WebSocket 服务器
 * 支持 WS 和 WSS 协议
 */

#ifndef VOX_WEBSOCKET_SERVER_H
#define VOX_WEBSOCKET_SERVER_H

#include "vox_websocket.h"
#include "../vox_loop.h"
#include "../vox_tcp.h"
#include "../vox_tls.h"
#include "../vox_socket.h"
#include "../ssl/vox_ssl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_ws_server vox_ws_server_t;
typedef struct vox_ws_connection vox_ws_connection_t;

/* WebSocket 连接回调 */
typedef void (*vox_ws_on_connection_cb)(vox_ws_connection_t* conn, void* user_data);
typedef void (*vox_ws_on_message_cb)(vox_ws_connection_t* conn, const void* data, 
                                     size_t len, vox_ws_message_type_t type, void* user_data);
typedef void (*vox_ws_on_close_cb)(vox_ws_connection_t* conn, uint16_t code, 
                                   const char* reason, void* user_data);
typedef void (*vox_ws_on_error_cb)(vox_ws_connection_t* conn, const char* error, void* user_data);

/* WebSocket 服务器配置 */
typedef struct {
    vox_loop_t* loop;                    /* 事件循环（必需） */
    vox_ssl_context_t* ssl_ctx;          /* SSL 上下文（用于 WSS，可选） */
    vox_ws_on_connection_cb on_connection; /* 新连接回调 */
    vox_ws_on_message_cb on_message;     /* 消息回调 */
    vox_ws_on_close_cb on_close;         /* 关闭回调 */
    vox_ws_on_error_cb on_error;         /* 错误回调 */
    void* user_data;                     /* 用户数据 */
    size_t max_message_size;             /* 最大消息大小（0表示无限制） */
    bool enable_compression;             /* 是否启用压缩（未实现） */
    const char* path;                    /* 可选：仅接受此 HTTP 路径的升级（如 "/mqtt"），NULL 表示接受任意路径 */
} vox_ws_server_config_t;

/**
 * 创建 WebSocket 服务器
 * @param config 配置结构
 * @return 成功返回服务器指针，失败返回 NULL
 */
vox_ws_server_t* vox_ws_server_create(const vox_ws_server_config_t* config);

/**
 * 销毁 WebSocket 服务器
 * @param server 服务器指针
 */
void vox_ws_server_destroy(vox_ws_server_t* server);

/**
 * 监听指定地址（WS）
 * @param server 服务器指针
 * @param addr 监听地址
 * @param backlog 监听队列长度
 * @return 成功返回0，失败返回-1
 */
int vox_ws_server_listen(vox_ws_server_t* server, const vox_socket_addr_t* addr, int backlog);

/**
 * 监听指定地址（WSS）
 * @param server 服务器指针
 * @param addr 监听地址
 * @param backlog 监听队列长度
 * @param ssl_ctx SSL 上下文
 * @return 成功返回0，失败返回-1
 */
int vox_ws_server_listen_ssl(vox_ws_server_t* server, const vox_socket_addr_t* addr, 
                              int backlog, vox_ssl_context_t* ssl_ctx);

/**
 * 关闭服务器
 * @param server 服务器指针
 */
void vox_ws_server_close(vox_ws_server_t* server);

/**
 * 向连接发送文本消息
 * @param conn 连接指针
 * @param text 文本数据
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int vox_ws_connection_send_text(vox_ws_connection_t* conn, const char* text, size_t len);

/**
 * 向连接发送二进制消息
 * @param conn 连接指针
 * @param data 二进制数据
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int vox_ws_connection_send_binary(vox_ws_connection_t* conn, const void* data, size_t len);

/**
 * 发送 Ping 帧
 * @param conn 连接指针
 * @param data 负载数据（可选）
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int vox_ws_connection_send_ping(vox_ws_connection_t* conn, const void* data, size_t len);

/**
 * 关闭连接
 * @param conn 连接指针
 * @param code 关闭状态码
 * @param reason 关闭原因（可选）
 * @return 成功返回0，失败返回-1
 */
int vox_ws_connection_close(vox_ws_connection_t* conn, uint16_t code, const char* reason);

/**
 * 获取连接的用户数据
 * @param conn 连接指针
 * @return 用户数据指针
 */
void* vox_ws_connection_get_user_data(vox_ws_connection_t* conn);

/**
 * 设置连接的用户数据
 * @param conn 连接指针
 * @param user_data 用户数据
 */
void vox_ws_connection_set_user_data(vox_ws_connection_t* conn, void* user_data);

/**
 * 获取连接的对端地址
 * @param conn 连接指针
 * @param addr 地址结构指针
 * @return 成功返回0，失败返回-1
 */
int vox_ws_connection_getpeername(vox_ws_connection_t* conn, vox_socket_addr_t* addr);

#ifdef __cplusplus
}
#endif

#endif /* VOX_WEBSOCKET_SERVER_H */
