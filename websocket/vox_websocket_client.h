/*
 * vox_websocket_client.h - WebSocket 客户端
 * 支持 WS 和 WSS 协议
 */

#ifndef VOX_WEBSOCKET_CLIENT_H
#define VOX_WEBSOCKET_CLIENT_H

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
typedef struct vox_ws_client vox_ws_client_t;

/* WebSocket 客户端状态 */
typedef enum {
    VOX_WS_CLIENT_CONNECTING,   /* 正在连接 */
    VOX_WS_CLIENT_HANDSHAKING,  /* 正在握手 */
    VOX_WS_CLIENT_OPEN,         /* 已打开 */
    VOX_WS_CLIENT_CLOSING,      /* 正在关闭 */
    VOX_WS_CLIENT_CLOSED        /* 已关闭 */
} vox_ws_client_state_t;

/* WebSocket 客户端回调 */
typedef void (*vox_ws_client_on_connect_cb)(vox_ws_client_t* client, void* user_data);
typedef void (*vox_ws_client_on_message_cb)(vox_ws_client_t* client, const void* data,
                                            size_t len, vox_ws_message_type_t type, void* user_data);
typedef void (*vox_ws_client_on_close_cb)(vox_ws_client_t* client, uint16_t code,
                                          const char* reason, void* user_data);
typedef void (*vox_ws_client_on_error_cb)(vox_ws_client_t* client, const char* error, void* user_data);

/* WebSocket 客户端配置 */
typedef struct {
    vox_loop_t* loop;                       /* 事件循环（必需） */
    const char* url;                        /* WebSocket URL（必需） */
    const char* host;                       /* 主机名（可选，从 URL 解析） */
    const char* path;                       /* 路径（可选，从 URL 解析） */
    uint16_t port;                          /* 端口（可选，从 URL 解析） */
    bool use_ssl;                           /* 是否使用 SSL（从 URL 解析） */
    vox_ssl_context_t* ssl_ctx;             /* SSL 上下文（用于 WSS） */
    vox_ws_client_on_connect_cb on_connect; /* 连接成功回调 */
    vox_ws_client_on_message_cb on_message; /* 消息回调 */
    vox_ws_client_on_close_cb on_close;     /* 关闭回调 */
    vox_ws_client_on_error_cb on_error;     /* 错误回调 */
    void* user_data;                        /* 用户数据 */
    size_t max_message_size;                /* 最大消息大小（0表示无限制） */
} vox_ws_client_config_t;

/**
 * 创建 WebSocket 客户端
 * @param config 配置结构
 * @return 成功返回客户端指针，失败返回 NULL
 */
vox_ws_client_t* vox_ws_client_create(const vox_ws_client_config_t* config);

/**
 * 销毁 WebSocket 客户端
 * @param client 客户端指针
 */
void vox_ws_client_destroy(vox_ws_client_t* client);

/**
 * 连接到服务器
 * @param client 客户端指针
 * @return 成功返回0，失败返回-1
 */
int vox_ws_client_connect(vox_ws_client_t* client);

/**
 * 发送文本消息
 * @param client 客户端指针
 * @param text 文本数据
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int vox_ws_client_send_text(vox_ws_client_t* client, const char* text, size_t len);

/**
 * 发送二进制消息
 * @param client 客户端指针
 * @param data 二进制数据
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int vox_ws_client_send_binary(vox_ws_client_t* client, const void* data, size_t len);

/**
 * 发送 Ping 帧
 * @param client 客户端指针
 * @param data 负载数据（可选）
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int vox_ws_client_send_ping(vox_ws_client_t* client, const void* data, size_t len);

/**
 * 关闭连接
 * @param client 客户端指针
 * @param code 关闭状态码
 * @param reason 关闭原因（可选）
 * @return 成功返回0，失败返回-1
 */
int vox_ws_client_close(vox_ws_client_t* client, uint16_t code, const char* reason);

/**
 * 获取客户端状态
 * @param client 客户端指针
 * @return 客户端状态
 */
vox_ws_client_state_t vox_ws_client_get_state(const vox_ws_client_t* client);

/**
 * 获取用户数据
 * @param client 客户端指针
 * @return 用户数据指针
 */
void* vox_ws_client_get_user_data(vox_ws_client_t* client);

/**
 * 设置用户数据
 * @param client 客户端指针
 * @param user_data 用户数据
 */
void vox_ws_client_set_user_data(vox_ws_client_t* client, void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* VOX_WEBSOCKET_CLIENT_H */
