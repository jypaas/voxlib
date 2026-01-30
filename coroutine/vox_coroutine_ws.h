/*
 * vox_coroutine_ws.h - WebSocket协程适配器
 * 提供async/await风格的协程API，避免回调地狱
 */

#ifndef VOX_COROUTINE_WS_H
#define VOX_COROUTINE_WS_H

#include "../websocket/vox_websocket_client.h"
#include "vox_coroutine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== WebSocket客户端连接句柄 ===== */

typedef struct vox_coroutine_ws_client vox_coroutine_ws_client_t;

/* ===== WebSocket消息结构 ===== */

typedef struct {
    void* data;
    size_t len;
    bool is_text;
} vox_coroutine_ws_message_t;

/* ===== 协程适配接口 ===== */

/**
 * 在协程中连接到WebSocket服务器
 * @param co 协程指针
 * @param loop 事件循环指针
 * @param url WebSocket URL (ws:// 或 wss://)
 * @param out_client 输出客户端句柄（需要调用者使用vox_coroutine_ws_disconnect释放）
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_ws_connect_await(vox_coroutine_t* co,
                                    vox_loop_t* loop,
                                    const char* url,
                                    vox_coroutine_ws_client_t** out_client);

/**
 * 在协程中接收WebSocket消息（阻塞直到收到消息）
 * @param co 协程指针
 * @param client WebSocket客户端句柄
 * @param out_message 输出消息数据（需要调用者使用vox_coroutine_ws_message_free释放）
 * @return 成功返回0，连接关闭返回1，失败返回-1
 */
int vox_coroutine_ws_recv_await(vox_coroutine_t* co,
                                 vox_coroutine_ws_client_t* client,
                                 vox_coroutine_ws_message_t* out_message);

/**
 * 在协程中发送WebSocket文本消息
 * @param co 协程指针
 * @param client WebSocket客户端句柄
 * @param text 文本数据
 * @param len 文本长度
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_ws_send_text_await(vox_coroutine_t* co,
                                      vox_coroutine_ws_client_t* client,
                                      const char* text,
                                      size_t len);

/**
 * 在协程中发送WebSocket二进制消息
 * @param co 协程指针
 * @param client WebSocket客户端句柄
 * @param data 二进制数据
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_ws_send_binary_await(vox_coroutine_t* co,
                                        vox_coroutine_ws_client_t* client,
                                        const void* data,
                                        size_t len);

/**
 * 在协程中关闭WebSocket连接
 * @param co 协程指针
 * @param client WebSocket客户端句柄
 * @param code 关闭状态码
 * @param reason 关闭原因（可为NULL）
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_ws_close_await(vox_coroutine_t* co,
                                  vox_coroutine_ws_client_t* client,
                                  int code,
                                  const char* reason);

/**
 * 断开WebSocket连接（非协程版本，用于清理）
 * @param client WebSocket客户端句柄
 */
void vox_coroutine_ws_disconnect(vox_coroutine_ws_client_t* client);

/**
 * 释放WebSocket消息数据
 * @param message 消息数据指针
 */
void vox_coroutine_ws_message_free(vox_coroutine_ws_message_t* message);

#ifdef __cplusplus
}
#endif

#endif /* VOX_COROUTINE_WS_H */
