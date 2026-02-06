/*
 * vox_mqtt_server.h - MQTT 服务端（Broker）
 * - 协议：接受 MQTT 3.1.1 与 3.1；Broker 层通过 accepted_versions 与 protocol_version 屏蔽版本差异
 * - 传输：TCP / TLS（listen_ssl）/ WebSocket（listen_ws / listen_wss）
 */

#ifndef VOX_MQTT_SERVER_H
#define VOX_MQTT_SERVER_H

#include "../vox_os.h"
#include "../vox_loop.h"
#include "../vox_mpool.h"
#include "../vox_socket.h"
#include "../vox_tcp.h"
#include "vox_mqtt_parser.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(VOX_USE_SSL) && VOX_USE_SSL
#include "../vox_tls.h"
#include "../ssl/vox_ssl.h"
#endif
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
#include "../websocket/vox_websocket_server.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vox_mqtt_server vox_mqtt_server_t;
typedef struct vox_mqtt_connection vox_mqtt_connection_t;

/** 新连接建立（已发送 CONNACK） */
typedef void (*vox_mqtt_server_on_connect_cb)(vox_mqtt_connection_t* conn, const char* client_id, size_t client_id_len, void* user_data);

/** 连接断开 */
typedef void (*vox_mqtt_server_on_disconnect_cb)(vox_mqtt_connection_t* conn, void* user_data);

/** 收到客户端 PUBLISH（可选，用于日志或鉴权） */
typedef void (*vox_mqtt_server_on_publish_cb)(vox_mqtt_connection_t* conn,
    const char* topic, size_t topic_len,
    const void* payload, size_t payload_len,
    uint8_t qos, void* user_data);

/** 可接受协议版本掩码：0=接受当前实现支持的全部（3.1+3.1.1+5），否则为 (1<<version) 组合 */
#define VOX_MQTT_ACCEPT_VERSION_3_1   (1u << VOX_MQTT_VERSION_3_1)   /* 3.1 (MQIsdp) */
#define VOX_MQTT_ACCEPT_VERSION_3_1_1 (1u << VOX_MQTT_VERSION_3_1_1) /* 3.1.1 (MQTT) */
#define VOX_MQTT_ACCEPT_VERSION_5     (1u << VOX_MQTT_VERSION_5)     /* 5 */

/** 服务器配置 */
typedef struct {
    vox_loop_t* loop;
    vox_mpool_t* mpool;           /* NULL 则服务器内部创建 */
    unsigned int accepted_versions; /* 0=接受 3.1 与 3.1.1，否则仅接受掩码中的版本，用于屏蔽不兼容版本 */
    vox_mqtt_server_on_connect_cb on_connect;
    vox_mqtt_server_on_disconnect_cb on_disconnect;
    vox_mqtt_server_on_publish_cb on_publish;
    void* user_data;
} vox_mqtt_server_config_t;

/** 创建服务器 */
vox_mqtt_server_t* vox_mqtt_server_create(const vox_mqtt_server_config_t* config);

/** 销毁服务器 */
void vox_mqtt_server_destroy(vox_mqtt_server_t* server);

/** 监听 addr（TCP，端口通常 1883） */
int vox_mqtt_server_listen(vox_mqtt_server_t* server, const vox_socket_addr_t* addr, int backlog);

#if defined(VOX_USE_SSL) && VOX_USE_SSL
/** 监听 addr（TLS，端口通常 8883） */
int vox_mqtt_server_listen_ssl(vox_mqtt_server_t* server, const vox_socket_addr_t* addr,
    int backlog, vox_ssl_context_t* ssl_ctx);
#endif

#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
/** 监听 addr，MQTT over WebSocket，path 为 HTTP 升级路径（如 "/mqtt"） */
int vox_mqtt_server_listen_ws(vox_mqtt_server_t* server, const vox_socket_addr_t* addr,
    int backlog, const char* path);
/** 监听 addr，MQTT over WSS */
int vox_mqtt_server_listen_wss(vox_mqtt_server_t* server, const vox_socket_addr_t* addr,
    int backlog, const char* path, vox_ssl_context_t* ssl_ctx);
#endif

/** 关闭监听（不断开已有连接） */
void vox_mqtt_server_close(vox_mqtt_server_t* server);

/** 向指定连接下发 PUBLISH（qos 0/1） */
int vox_mqtt_connection_publish(vox_mqtt_connection_t* conn,
    const char* topic, size_t topic_len,
    const void* payload, size_t payload_len,
    uint8_t qos, bool retain);

/** 获取连接上的用户数据 */
void* vox_mqtt_connection_get_user_data(vox_mqtt_connection_t* conn);
void vox_mqtt_connection_set_user_data(vox_mqtt_connection_t* conn, void* user_data);

/** 获取连接协商的 MQTT 协议版本（VOX_MQTT_VERSION_3_1 / 3_1_1 / 5） */
uint8_t vox_mqtt_connection_get_protocol_version(vox_mqtt_connection_t* conn);

/** MQTT 5：获取 CONNECT 中的 Session Expiry Interval（非 v5 连接返回 0） */
uint32_t vox_mqtt_connection_get_session_expiry_interval(vox_mqtt_connection_t* conn);
/** MQTT 5：获取 CONNECT 中的 Receive Maximum（非 v5 或未指定时返回 0，Broker 可回显或使用默认） */
uint16_t vox_mqtt_connection_get_receive_maximum(vox_mqtt_connection_t* conn);

#ifdef __cplusplus
}
#endif

#endif /* VOX_MQTT_SERVER_H */
