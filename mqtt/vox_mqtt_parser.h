/*
 * vox_mqtt_parser.h - MQTT 报文解析与序列化
 * - 协议：MQTT 3.1.1（编码默认）；解析兼容 3.1/3.1.1/5；MQTT 5 支持 CONNECT/CONNACK 属性等
 * - 传输：由 client/server 层挂接 TCP / TLS / WebSocket
 */

#ifndef VOX_MQTT_PARSER_H
#define VOX_MQTT_PARSER_H

#include "../vox_os.h"
#include "../vox_mpool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 报文类型 (MQTT 3.1.1) ===== */
#define VOX_MQTT_PKT_CONNECT     1
#define VOX_MQTT_PKT_CONNACK     2
#define VOX_MQTT_PKT_PUBLISH     3
#define VOX_MQTT_PKT_PUBACK      4
#define VOX_MQTT_PKT_PUBREC      5
#define VOX_MQTT_PKT_PUBREL      6
#define VOX_MQTT_PKT_PUBCOMP     7
#define VOX_MQTT_PKT_SUBSCRIBE   8
#define VOX_MQTT_PKT_SUBACK      9
#define VOX_MQTT_PKT_UNSUBSCRIBE 10
#define VOX_MQTT_PKT_UNSUBACK    11
#define VOX_MQTT_PKT_PINGREQ     12
#define VOX_MQTT_PKT_PINGRESP    13
#define VOX_MQTT_PKT_DISCONNECT  14
#define VOX_MQTT_PKT_AUTH        15   /* MQTT 5 增强认证 */

/* CONNACK 可变头：首字节为 Session Present 等标志 */
#define VOX_MQTT_CONNACK_FLAG_SESSION_PRESENT  0x01

/* CONNACK 返回码 */
#define VOX_MQTT_CONNACK_ACCEPTED           0
#define VOX_MQTT_CONNACK_REFUSED_PROTOCOL   1
#define VOX_MQTT_CONNACK_REFUSED_ID         2
#define VOX_MQTT_CONNACK_REFUSED_UNAVAIL    3
#define VOX_MQTT_CONNACK_REFUSED_BAD_AUTH   4
#define VOX_MQTT_CONNACK_REFUSED_NOT_AUTH   5

/* CONNECT 连接标志（Connect Flags 字节） */
#define VOX_MQTT_CONNECT_FLAG_CLEAN_SESSION  0x02
#define VOX_MQTT_CONNECT_FLAG_WILL           0x04
#define VOX_MQTT_CONNECT_FLAG_WILL_QOS_SHIFT 3
#define VOX_MQTT_CONNECT_FLAG_WILL_RETAIN    0x20
#define VOX_MQTT_CONNECT_FLAG_USERNAME       0x80
#define VOX_MQTT_CONNECT_FLAG_PASSWORD       0x40

/* PUBLISH 固定头标志（低 4 位：QoS 2bit + Retain 1bit） */
#define VOX_MQTT_PUBLISH_MASK_QOS       0x03
#define VOX_MQTT_PUBLISH_MASK_RETAIN    0x01
#define VOX_MQTT_PUBLISH_RETAIN_SHIFT   4   /* 解析时从 flags 取 retain：flags >> 4 & 1 */
#define VOX_MQTT_PUBLISH_QOS_SHIFT      1   /* 编码时 QoS 在首字节的位移 */

/* SUBACK 授予失败时的返回码（3.1.1）/ 原因码（5） */
#define VOX_MQTT_SUBACK_FAILURE        0x80

/* SUBSCRIBE/UNSUBSCRIBE 固定头保留位（必须为 0x02） */
#define VOX_MQTT_SUBSCRIBE_RESERVED    0x02
#define VOX_MQTT_UNSUBSCRIBE_RESERVED  0x02
/* PUBREL 固定头保留位（必须为 0x02） */
#define VOX_MQTT_PUBREL_RESERVED       0x02

/* 协议版本在掩码中的有效位（用于 accepted_versions 位图） */
#define VOX_MQTT_VERSION_NIBBLE_MASK   0x0Fu

/* 最大剩余长度（256KB 限制，可配置） */
#define VOX_MQTT_DEFAULT_MAX_PAYLOAD (256 * 1024)

typedef struct vox_mqtt_parser vox_mqtt_parser_t;

/* ===== 解析回调 ===== */

/** 协议版本（CONNECT 中的 Protocol Level）：Broker 据此屏蔽版本差异、按版本回包 */
#define VOX_MQTT_VERSION_3_1    3   /* MQTT 3.1 (MQIsdp) */
#define VOX_MQTT_VERSION_3_1_1 4   /* MQTT 3.1.1 (MQTT) */
#define VOX_MQTT_VERSION_5      5   /* MQTT 5（解析与编码已支持，含 CONNECT/CONNACK 属性等） */

/** CONNECT: 收到客户端连接请求；protocol_version 为 CONNECT 中的版本字节（3/4/5）；v5 时 session_expiry_interval/receive_maximum 来自连接属性，否则为 0 */
typedef int (*vox_mqtt_on_connect_cb)(void* user_data, const char* client_id, size_t client_id_len,
    uint8_t protocol_version,
    uint16_t keepalive, uint8_t flags, const char* will_topic, size_t will_topic_len,
    const void* will_msg, size_t will_msg_len, const char* username, size_t username_len,
    const char* password, size_t password_len,
    uint32_t session_expiry_interval, uint16_t receive_maximum);

/** CONNACK: 连接确认 */
typedef int (*vox_mqtt_on_connack_cb)(void* user_data, uint8_t session_present, uint8_t return_code);

/** PUBLISH: 收到发布（topic/payload 在回调有效期内有效，需拷贝则用 mpool） */
typedef int (*vox_mqtt_on_publish_cb)(void* user_data, uint8_t qos, bool retain, uint16_t packet_id,
    const char* topic, size_t topic_len, const void* payload, size_t payload_len);

/** PUBACK */
typedef int (*vox_mqtt_on_puback_cb)(void* user_data, uint16_t packet_id);
/** PUBREC（QoS 2：收到后应回 PUBREL） */
typedef int (*vox_mqtt_on_pubrec_cb)(void* user_data, uint16_t packet_id);
/** PUBREL（QoS 2：收到后应回 PUBCOMP） */
typedef int (*vox_mqtt_on_pubrel_cb)(void* user_data, uint16_t packet_id);
/** PUBCOMP（QoS 2：完成） */
typedef int (*vox_mqtt_on_pubcomp_cb)(void* user_data, uint16_t packet_id);

/** SUBSCRIBE: 订阅请求，返回授予的 qos (0/1/2)，失败返回 -1（将发 VOX_MQTT_SUBACK_FAILURE） */
typedef int (*vox_mqtt_on_subscribe_cb)(void* user_data, uint16_t packet_id,
    const char* topic_filter, size_t topic_len, uint8_t qos);

/** SUBACK（服务端发） */
typedef int (*vox_mqtt_on_suback_cb)(void* user_data, uint16_t packet_id, const uint8_t* return_codes, size_t count);

/** SUBSCRIBE 解析完成（服务端用：所有 topic 已解析，可发一个 SUBACK） */
typedef int (*vox_mqtt_on_subscribe_done_cb)(void* user_data, uint16_t packet_id, const uint8_t* return_codes, size_t count);

/** UNSUBSCRIBE */
typedef int (*vox_mqtt_on_unsubscribe_cb)(void* user_data, uint16_t packet_id,
    const char* topic_filter, size_t topic_len);

/** UNSUBACK */
typedef int (*vox_mqtt_on_unsuback_cb)(void* user_data, uint16_t packet_id);

/** PINGREQ / PINGRESP */
typedef int (*vox_mqtt_on_ping_cb)(void* user_data);

/** DISCONNECT */
typedef int (*vox_mqtt_on_disconnect_cb)(void* user_data);

/** 解析错误 */
typedef int (*vox_mqtt_on_error_cb)(void* user_data, const char* message);

typedef struct {
    vox_mqtt_on_connect_cb on_connect;
    vox_mqtt_on_connack_cb on_connack;
    vox_mqtt_on_publish_cb on_publish;
    vox_mqtt_on_puback_cb on_puback;
    vox_mqtt_on_pubrec_cb on_pubrec;
    vox_mqtt_on_pubrel_cb on_pubrel;
    vox_mqtt_on_pubcomp_cb on_pubcomp;
    vox_mqtt_on_subscribe_cb on_subscribe;
    vox_mqtt_on_subscribe_done_cb on_subscribe_done;
    vox_mqtt_on_suback_cb on_suback;
    vox_mqtt_on_unsubscribe_cb on_unsubscribe;
    vox_mqtt_on_unsuback_cb on_unsuback;
    vox_mqtt_on_ping_cb on_pingreq;
    vox_mqtt_on_ping_cb on_pingresp;
    vox_mqtt_on_disconnect_cb on_disconnect;
    vox_mqtt_on_error_cb on_error;
    void* user_data;
} vox_mqtt_parser_callbacks_t;

typedef struct {
    size_t max_payload;       /* 最大报文负载长度，0 表示默认 VOX_MQTT_DEFAULT_MAX_PAYLOAD */
    uint8_t protocol_version; /* 0=由 CONNECT 协商；5=客户端 MQTT 5，解析 CONNACK/SUBACK 等按 v5 格式 */
} vox_mqtt_parser_config_t;

/* ===== 解析器 API ===== */

vox_mqtt_parser_t* vox_mqtt_parser_create(vox_mpool_t* mpool,
    const vox_mqtt_parser_config_t* config,
    const vox_mqtt_parser_callbacks_t* callbacks);

void vox_mqtt_parser_destroy(vox_mqtt_parser_t* parser);

void vox_mqtt_parser_reset(vox_mqtt_parser_t* parser);

/** 流式解析，返回已消费字节数，错误返回 -1 */
ssize_t vox_mqtt_parser_execute(vox_mqtt_parser_t* parser, const char* data, size_t len);

bool vox_mqtt_parser_has_error(const vox_mqtt_parser_t* parser);

const char* vox_mqtt_parser_get_error(const vox_mqtt_parser_t* parser);

/* ===== 编码 API（写入 buf，返回所需/已写长度，buf 不足返回 0） ===== */

/** 编码 CONNECT，返回所需长度；buf 非 NULL 时写入。client_id 必需。 */
size_t vox_mqtt_encode_connect(uint8_t* buf, size_t buf_size,
    const char* client_id, size_t client_id_len,
    uint16_t keepalive, bool clean_session,
    const char* will_topic, size_t will_topic_len,
    const void* will_msg, size_t will_msg_len, uint8_t will_qos, bool will_retain,
    const char* username, size_t username_len,
    const char* password, size_t password_len);

/** 编码 CONNACK */
size_t vox_mqtt_encode_connack(uint8_t* buf, size_t buf_size, uint8_t session_present, uint8_t return_code);

/** 编码 PUBLISH。qos>0 时 packet_id 有效。 */
size_t vox_mqtt_encode_publish(uint8_t* buf, size_t buf_size,
    uint8_t qos, bool retain, uint16_t packet_id,
    const char* topic, size_t topic_len,
    const void* payload, size_t payload_len);

/** 编码 PUBACK / PUBREC / PUBREL / PUBCOMP */
size_t vox_mqtt_encode_puback(uint8_t* buf, size_t buf_size, uint16_t packet_id);
size_t vox_mqtt_encode_pubrec(uint8_t* buf, size_t buf_size, uint16_t packet_id);
size_t vox_mqtt_encode_pubrel(uint8_t* buf, size_t buf_size, uint16_t packet_id);
size_t vox_mqtt_encode_pubcomp(uint8_t* buf, size_t buf_size, uint16_t packet_id);

/** 编码 SUBSCRIBE：topic_filters 为 (const char* topic, size_t len, uint8_t qos) 重复，count 为订阅数 */
size_t vox_mqtt_encode_subscribe(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const char* const* topic_filters, const size_t* topic_lens, const uint8_t* qos_list, size_t count);

/** 编码 SUBACK */
size_t vox_mqtt_encode_suback(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const uint8_t* return_codes, size_t count);

/** 编码 UNSUBSCRIBE */
size_t vox_mqtt_encode_unsubscribe(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const char* const* topic_filters, const size_t* topic_lens, size_t count);

/** 编码 UNSUBACK */
size_t vox_mqtt_encode_unsuback(uint8_t* buf, size_t buf_size, uint16_t packet_id);

/** 编码 PINGREQ / PINGRESP / DISCONNECT */
size_t vox_mqtt_encode_pingreq(uint8_t* buf, size_t buf_size);
size_t vox_mqtt_encode_pingresp(uint8_t* buf, size_t buf_size);
size_t vox_mqtt_encode_disconnect(uint8_t* buf, size_t buf_size);

/** CONNECT v5：协议名 MQTT、版本 5；session_expiry_interval/receive_maximum 为 0 则省略该属性 */
size_t vox_mqtt_encode_connect_v5(uint8_t* buf, size_t buf_size,
    const char* client_id, size_t client_id_len,
    uint16_t keepalive, bool clean_session,
    const char* will_topic, size_t will_topic_len,
    const void* will_msg, size_t will_msg_len, uint8_t will_qos, bool will_retain,
    const char* username, size_t username_len,
    const char* password, size_t password_len,
    uint32_t session_expiry_interval, uint16_t receive_maximum);

/* ===== MQTT 5 编码（Broker 向 v5 客户端回包 / 客户端 v5 连接时使用） ===== */
#define VOX_MQTT5_REASON_SUCCESS  0
#define VOX_MQTT5_REASON_REFUSED_PROTOCOL 132

/** CONNACK v5：session_present(1) + reason_code(1) + 属性；session_expiry_interval/receive_maximum 为 0 则省略该属性 */
size_t vox_mqtt_encode_connack_v5(uint8_t* buf, size_t buf_size, uint8_t session_present, uint8_t reason_code,
    uint32_t session_expiry_interval, uint16_t receive_maximum);
/** PUBLISH v5：在 packet_id 后增加属性长度 varint(0) */
size_t vox_mqtt_encode_publish_v5(uint8_t* buf, size_t buf_size,
    uint8_t qos, bool retain, uint16_t packet_id,
    const char* topic, size_t topic_len,
    const void* payload, size_t payload_len);
/** SUBACK v5：packet_id(2) + 属性长度 varint(0) + reason_codes[] */
size_t vox_mqtt_encode_suback_v5(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const uint8_t* reason_codes, size_t count);
/** UNSUBACK v5：packet_id(2) + 属性长度 varint(0) + reason_codes[]（每 topic 一个，无则 count=0） */
size_t vox_mqtt_encode_unsuback_v5(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const uint8_t* reason_codes, size_t count);
/** DISCONNECT v5：reason_code(1) + 属性长度 varint(0) */
size_t vox_mqtt_encode_disconnect_v5(uint8_t* buf, size_t buf_size, uint8_t reason_code);

/** SUBSCRIBE v5：packet_id(2) + 属性长度 varint(0) + [topic_filter + options] */
size_t vox_mqtt_encode_subscribe_v5(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const char* const* topic_filters, const size_t* topic_lens, const uint8_t* qos_list, size_t count);
/** UNSUBSCRIBE v5：packet_id(2) + 属性长度 varint(0) + topic_filters */
size_t vox_mqtt_encode_unsubscribe_v5(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const char* const* topic_filters, const size_t* topic_lens, size_t count);

/** 设置解析器协议版本（客户端在发送 CONNECT v5 后调用，以便正确解析 CONNACK/SUBACK 等） */
void vox_mqtt_parser_set_protocol_version(vox_mqtt_parser_t* parser, uint8_t version);

/** 解析 CONNACK v5 后获取服务端返回的属性（非 v5 或未包含该属性时返回 0） */
uint32_t vox_mqtt_parser_get_connack_session_expiry_interval(const vox_mqtt_parser_t* parser);
uint16_t vox_mqtt_parser_get_connack_receive_maximum(const vox_mqtt_parser_t* parser);

/** 剩余长度编码：返回所需字节数；buf 非 NULL 时写入 */
int vox_mqtt_encode_remaining_length(uint8_t* buf, size_t value);

#ifdef __cplusplus
}
#endif

#endif /* VOX_MQTT_PARSER_H */
