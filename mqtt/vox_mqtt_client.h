/*
 * vox_mqtt_client.h - MQTT 异步客户端
 * - 协议：MQTT 3.1.1 与 MQTT 5（options.use_mqtt5 选择）
 * - 传输：TCP(1883) / TLS(8883) / WS / WSS；ssl_ctx 与 ws_path 在连接选项中指定
 */

#ifndef VOX_MQTT_CLIENT_H
#define VOX_MQTT_CLIENT_H

#include "../vox_os.h"
#include "../vox_loop.h"
#include "../vox_mpool.h"
#include "../vox_socket.h"
#include "../vox_tcp.h"
#include "../vox_dns.h"
#include "vox_mqtt_parser.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(VOX_USE_SSL) && VOX_USE_SSL
#include "../vox_tls.h"
#include "../ssl/vox_ssl.h"
#endif
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
#include "../websocket/vox_websocket_client.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vox_mqtt_client vox_mqtt_client_t;

/** 连接选项 */
typedef struct {
    const char* client_id;       /* 必需 */
    size_t client_id_len;        /* 0 表示 strlen(client_id) */
    uint16_t keepalive;          /* 秒，0 表示默认 60 */
    bool clean_session;          /* 默认 true */
    const char* username;        /* 可选 */
    size_t username_len;
    const char* password;       /* 可选 */
    size_t password_len;
    const char* will_topic;      /* 可选 */
    size_t will_topic_len;
    const void* will_msg;        /* 可选 */
    size_t will_msg_len;
    uint8_t will_qos;
    bool will_retain;
    bool use_mqtt5;            /* true 则使用 MQTT 5（CONNECT v5、CONNACK/SUBACK v5 解析、v5 编码发布/订阅） */
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    vox_ssl_context_t* ssl_ctx; /* 非 NULL 则 MQTT over TLS（通常端口 8883） */
#endif
    /* 固定布局：始终保留字段，避免与未定义 VOX_USE_WEBSOCKET 的调用方 ABI 不一致导致越界访问 */
    const char* ws_path;         /* 非 NULL 则 MQTT over WebSocket（如 "/mqtt"）；需 VOX_USE_WEBSOCKET 才生效 */
    size_t ws_path_len;         /* 0 表示 strlen(ws_path) */

    /* 自动重连配置 */
    bool enable_auto_reconnect;        /* 是否启用自动重连（默认 false） */
    uint32_t max_reconnect_attempts;   /* 最大重连次数，0 表示无限重试（默认 0） */
    uint32_t initial_reconnect_delay_ms; /* 初始重连延迟（默认 1000ms） */
    uint32_t max_reconnect_delay_ms;   /* 最大重连延迟（默认 60000ms） */
} vox_mqtt_connect_options_t;

/** 连接结果回调：status 0=成功，非0=失败（见 CONNACK 或连接错误） */
typedef void (*vox_mqtt_connect_cb)(vox_mqtt_client_t* client, int status, void* user_data);

/** 收到服务端发布的报文 */
typedef void (*vox_mqtt_message_cb)(vox_mqtt_client_t* client,
    const char* topic, size_t topic_len,
    const void* payload, size_t payload_len,
    uint8_t qos, bool retain, void* user_data);

/** 订阅枚举回调：遍历当前所有订阅时调用 */
typedef void (*vox_mqtt_subscription_cb)(const char* topic_filter, size_t topic_filter_len,
    uint8_t qos, void* user_data);

/** 订阅确认：packet_id 与 subscribe 时一致，return_codes 数组长度为订阅数 */
typedef void (*vox_mqtt_suback_cb)(vox_mqtt_client_t* client, uint16_t packet_id,
    const uint8_t* return_codes, size_t count, void* user_data);

/** 断开/错误 */
typedef void (*vox_mqtt_disconnect_cb)(vox_mqtt_client_t* client, void* user_data);
typedef void (*vox_mqtt_error_cb)(vox_mqtt_client_t* client, const char* message, void* user_data);

/** 创建客户端（使用 loop 的 mpool） */
vox_mqtt_client_t* vox_mqtt_client_create(vox_loop_t* loop);

/** 销毁客户端。若刚调用过 disconnect，应在 disconnect 回调或下一次 loop 跑完后再 destroy。 */
void vox_mqtt_client_destroy(vox_mqtt_client_t* client);

/** 连接：host/port + options + 回调；client_id 在 options 中 */
int vox_mqtt_client_connect(vox_mqtt_client_t* client,
    const char* host, uint16_t port,
    const vox_mqtt_connect_options_t* options,
    vox_mqtt_connect_cb cb, void* user_data);

/** 断开连接。建议在 disconnect 回调被调用或下一次 loop 迭代之后再调用 destroy，避免未完成的写回调用时 client 已销毁导致 use-after-free。 */
void vox_mqtt_client_disconnect(vox_mqtt_client_t* client);

/** 是否已连接（已收到 CONNACK 且未断开） */
bool vox_mqtt_client_is_connected(vox_mqtt_client_t* client);

/** 发布：qos 0/1/2；QoS 2 仅支持单路 in-flight，返回 0 成功，-1 失败；payload 可为 NULL（len=0） */
int vox_mqtt_client_publish(vox_mqtt_client_t* client,
    const char* topic, size_t topic_len,
    const void* payload, size_t payload_len,
    uint8_t qos, bool retain);

/** 订阅：topic_filter 可含通配符；qos 0/1/2；返回 0 成功，-1 失败；on_suback 可选 */
int vox_mqtt_client_subscribe(vox_mqtt_client_t* client,
    const char* topic_filter, size_t topic_filter_len,
    uint8_t qos,
    vox_mqtt_suback_cb on_suback, void* user_data);

/** 取消订阅 */
int vox_mqtt_client_unsubscribe(vox_mqtt_client_t* client,
    const char* topic_filter, size_t topic_filter_len);

/** 遍历当前所有订阅：对每个订阅调用回调函数 */
void vox_mqtt_client_foreach_subscription(vox_mqtt_client_t* client,
    vox_mqtt_subscription_cb cb, void* user_data);

/** 设置回调（可在 connect 前或后调用） */
void vox_mqtt_client_set_message_cb(vox_mqtt_client_t* client, vox_mqtt_message_cb cb, void* user_data);
void vox_mqtt_client_set_disconnect_cb(vox_mqtt_client_t* client, vox_mqtt_disconnect_cb cb, void* user_data);
void vox_mqtt_client_set_error_cb(vox_mqtt_client_t* client, vox_mqtt_error_cb cb, void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* VOX_MQTT_CLIENT_H */
