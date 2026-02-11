/*
 * vox_mqtt_parser.c - MQTT 3.1.1 解析与编码实现
 */

#include "vox_mqtt_parser.h"
#include "../vox_os.h"
#include <string.h>

/* ===== 解析状态 ===== */
typedef enum {
    VOX_MQTT_STATE_FIXED_HEADER,
    VOX_MQTT_STATE_REMAINING_LEN,
    VOX_MQTT_STATE_VARHEADER_PAYLOAD,
    VOX_MQTT_STATE_DONE,
    VOX_MQTT_STATE_ERROR
} vox_mqtt_parser_state_t;

#define VOX_MQTT_PARSER_INITIAL_BUF_SIZE  512   /* 小报文（PINGREQ/CONNACK 等）免去 ensure_buf */
#define VOX_MQTT_PARSER_ERROR_BUF_SIZE   64    /* 错误信息短时免 mpool_alloc */

/* MQTT 5 属性 ID（property_value_skip 跳过属性块时使用，值长度见注释） */
#define VOX_MQTT5_PROP_SESSION_EXPIRY_INTERVAL  0x11U  /* 4 字节 */
#define VOX_MQTT5_PROP_RECEIVE_MAXIMUM          0x21U  /* 2 字节 */
#define VOX_MQTT5_PROP_ASSIGNED_CLIENT_ID       0x12U  /* UTF-8 2+len */
#define VOX_MQTT5_PROP_SERVER_KEEP_ALIVE        0x13U  /* 2 字节 */
#define VOX_MQTT5_PROP_AUTH_METHOD              0x15U  /* UTF-8 2+len */
#define VOX_MQTT5_PROP_AUTH_DATA                0x16U  /* Binary 2+len */
#define VOX_MQTT5_PROP_RESPONSE_INFO            0x1AU  /* UTF-8 2+len */
#define VOX_MQTT5_PROP_SERVER_REFERENCE         0x1CU  /* UTF-8 2+len */
#define VOX_MQTT5_PROP_REASON_STRING            0x1FU  /* UTF-8 2+len */
#define VOX_MQTT5_PROP_TOPIC_ALIAS_MAXIMUM      0x22U  /* 2 字节 */
#define VOX_MQTT5_PROP_TOPIC_ALIAS              0x23U  /* 2 字节 */
#define VOX_MQTT5_PROP_RETAIN_AVAILABLE         0x25U  /* 1 字节 */
#define VOX_MQTT5_PROP_USER_PROPERTY            0x26U  /* 键值对 2+len_k+2+len_v */
#define VOX_MQTT5_PROP_MAXIMUM_QOS              0x27U  /* 1 字节 */
#define VOX_MQTT5_PROP_WILDCARD_SUB_AVAILABLE   0x28U  /* 2 字节 */
#define VOX_MQTT5_PROP_SUB_ID_AVAILABLE         0x29U  /* 2 字节 */
#define VOX_MQTT5_PROP_SHARED_SUB_AVAILABLE     0x2AU  /* 2 字节 */
#define VOX_MQTT5_PROP_MAX_PACKET_SIZE          0x2BU  /* 跳过时按 1 字节处理 */
#define VOX_MQTT5_PROP_SUBSCRIPTION_IDENTIFIER  0x2CU  /* 2 字节跳过 */
#define VOX_MQTT5_PROP_CONTENT_TYPE             0x2DU  /* 跳过时按 1 字节处理 */
#define VOX_MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL  0x2EU  /* 2 字节跳过 */
#define VOX_MQTT5_PROP_CORRELATION_DATA        0x2FU  /* 2 字节跳过 */
#define VOX_MQTT5_PROP_PAYLOAD_FORMAT_INDICATOR 0x31U  /* 1 字节 */
#define VOX_MQTT5_PROP_MAX_QOS                  0x24U  /* 2 字节跳过 */

struct vox_mqtt_parser {
    vox_mpool_t* mpool;
    vox_mqtt_parser_callbacks_t cb;
    size_t max_payload;

    vox_mqtt_parser_state_t state;
    uint8_t type;
    uint8_t flags;
    uint32_t remaining_len;
    uint32_t remaining_mult;  /* 剩余长度已读字节数 */
    uint32_t need;            /* 当前阶段还需字节数 */

    /* 可变头/负载累积 */
    uint8_t* buf;
    size_t buf_cap;
    size_t buf_len;

    bool has_error;
    char* error_message;                          /* 指向 error_buf 或 mpool 分配 */
    char error_buf[VOX_MQTT_PARSER_ERROR_BUF_SIZE]; /* 短错误信息免分配 */

    uint8_t protocol_version;                     /* 协商版本 0/3/4/5，CONNECT 解析后设置，供后续 v5 报文解析用 */
    uint32_t connack_session_expiry_interval;      /* CONNACK v5 解析出的 Session Expiry Interval */
    uint16_t connack_receive_maximum;              /* CONNACK v5 解析出的 Receive Maximum */
};

static void set_error(vox_mqtt_parser_t* p, const char* msg) {
    if (!p) return;
    p->has_error = true;
    p->state = VOX_MQTT_STATE_ERROR;
    if (p->error_message && p->error_message != p->error_buf)
        vox_mpool_free(p->mpool, p->error_message);
    p->error_message = NULL;
    size_t n = strlen(msg);
    if (n < VOX_MQTT_PARSER_ERROR_BUF_SIZE) {
        memcpy(p->error_buf, msg, n + 1);
        p->error_message = p->error_buf;
    } else {
        p->error_message = (char*)vox_mpool_alloc(p->mpool, n + 1);
        if (p->error_message) {
            memcpy(p->error_message, msg, n + 1);
        } else {
            /* 分配失败时用 error_buf 截断保存，避免悬空指针 */
            size_t cap = VOX_MQTT_PARSER_ERROR_BUF_SIZE - 1;
            memcpy(p->error_buf, msg, cap);
            p->error_buf[cap] = '\0';
            p->error_message = p->error_buf;
        }
    }
    if (p->cb.on_error) p->cb.on_error(p->cb.user_data, msg);
}

/* 属性值长度常量，便于 property_value_skip 使用 */
#define VOX_MQTT5_PROP_LEN_4  4
#define VOX_MQTT5_PROP_LEN_2  2
#define VOX_MQTT5_PROP_LEN_1  1

/* MQTT 5 可变字节整数解码：前向声明，供 property_value_skip 使用 */
static int decode_varint(const uint8_t* data, size_t len, uint32_t* value, size_t* num_bytes);

/* MQTT 5 属性值跳过：根据属性 id 返回应跳过的字节数，变长属性根据 ptr 前 2 字节；无法解析时返回 -1 */
static int property_value_skip(uint8_t id, const uint8_t* ptr, size_t len) {
    switch (id) {
        case VOX_MQTT5_PROP_SESSION_EXPIRY_INTERVAL:
            return (len >= VOX_MQTT5_PROP_LEN_4) ? VOX_MQTT5_PROP_LEN_4 : -1;
        case VOX_MQTT5_PROP_RECEIVE_MAXIMUM:
        case VOX_MQTT5_PROP_TOPIC_ALIAS_MAXIMUM:
        case VOX_MQTT5_PROP_TOPIC_ALIAS:
            return (len >= VOX_MQTT5_PROP_LEN_2) ? VOX_MQTT5_PROP_LEN_2 : -1;
        case VOX_MQTT5_PROP_MAXIMUM_QOS:
        case VOX_MQTT5_PROP_RETAIN_AVAILABLE:
        case VOX_MQTT5_PROP_WILDCARD_SUB_AVAILABLE:
        case VOX_MQTT5_PROP_SUB_ID_AVAILABLE:
        case VOX_MQTT5_PROP_SHARED_SUB_AVAILABLE:
        case VOX_MQTT5_PROP_PAYLOAD_FORMAT_INDICATOR:
            return (len >= VOX_MQTT5_PROP_LEN_1) ? VOX_MQTT5_PROP_LEN_1 : -1;
        case VOX_MQTT5_PROP_MAX_PACKET_SIZE:
        case VOX_MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL:
            return (len >= VOX_MQTT5_PROP_LEN_4) ? VOX_MQTT5_PROP_LEN_4 : -1;
        case VOX_MQTT5_PROP_CONTENT_TYPE:
        case VOX_MQTT5_PROP_ASSIGNED_CLIENT_ID:
        case VOX_MQTT5_PROP_REASON_STRING:
        case VOX_MQTT5_PROP_SERVER_KEEP_ALIVE:
        case VOX_MQTT5_PROP_RESPONSE_INFO:
        case VOX_MQTT5_PROP_SERVER_REFERENCE:
        case VOX_MQTT5_PROP_AUTH_METHOD:
            /* UTF-8 字符串：2 字节长度 + 数据 */
            if (len >= VOX_MQTT5_PROP_LEN_2) {
                uint16_t n = (uint16_t)((ptr[0] << 8) | ptr[1]);
                return (size_t)(VOX_MQTT5_PROP_LEN_2 + n) <= len ? (int)(VOX_MQTT5_PROP_LEN_2 + n) : -1;
            }
            return -1;
        case VOX_MQTT5_PROP_AUTH_DATA:
        case VOX_MQTT5_PROP_CORRELATION_DATA:
            /* 二进制：2 字节长度 + 数据 */
            if (len >= VOX_MQTT5_PROP_LEN_2) {
                uint16_t n = (uint16_t)((ptr[0] << 8) | ptr[1]);
                return (size_t)(VOX_MQTT5_PROP_LEN_2 + n) <= len ? (int)(VOX_MQTT5_PROP_LEN_2 + n) : -1;
            }
            return -1;
        case VOX_MQTT5_PROP_SUBSCRIPTION_IDENTIFIER:
            /* 可变字节整数：1-4 字节 */
            {
                uint32_t val;
                size_t num_bytes;
                int r = decode_varint(ptr, len, &val, &num_bytes);
                return (r > 0) ? (int)num_bytes : -1;
            }
        case VOX_MQTT5_PROP_USER_PROPERTY: {
            /* 一对 UTF-8 键值：2+len_k+2+len_v */
            if (len >= 4) {
                uint16_t k = (uint16_t)((ptr[0] << 8) | ptr[1]);
                if ((size_t)(4 + k) <= len) {
                    uint16_t v = (uint16_t)((ptr[2 + k] << 8) | ptr[3 + k]);
                    if ((size_t)(4 + k + v) <= len) return (int)(4 + k + v);
                }
            }
            return -1;
        }
        default:
            return -1;
    }
}

/* MQTT 5 可变字节整数：1~4 字节，返回 0=需更多数据，>0=解码得到的字节数，-1=错误 */
static int decode_varint(const uint8_t* data, size_t len, uint32_t* value, size_t* num_bytes) {
    uint32_t v = 0;
    uint32_t mult = 1;
    size_t i = 0;
    for (; i < len && i < 4; i++) {
        v += (uint32_t)(data[i] & 127) * mult;
        if (data[i] < 128) {
            *value = v;
            *num_bytes = i + 1;
            return 1;
        }
        mult *= 128;
    }
    if (i >= 4) return -1;
    return 0;
}

/* 剩余长度解码：返回 0=需要更多数据，>0=解码得到的长度字节数，-1=错误 */
static int decode_remaining_length(const uint8_t* data, size_t len, uint32_t* value, size_t* num_bytes) {
    uint32_t v = 0;
    uint32_t mult = 1;
    size_t i = 0;
    for (; i < len && i < 4; i++) {
        if (data[i] > 127) {
            v += (uint32_t)(data[i] & 127) * mult;
            mult *= 128;
        } else {
            v += (uint32_t)data[i] * mult;
            *value = v;
            *num_bytes = i + 1;
            return 1;
        }
    }
    if (i >= 4) return -1;
    return 0;
}

/* 确保有足够缓冲区（mpool 分配）；扩容采用 2 倍增长以减少大报文多次 realloc */
static int ensure_buf(vox_mqtt_parser_t* p, size_t need) {
    if (p->buf_cap >= need) return 0;
    size_t cap = p->buf_cap > 0 ? p->buf_cap : VOX_MQTT_PARSER_INITIAL_BUF_SIZE;
    while (cap < need) cap *= 2;
    if (cap > p->max_payload) cap = p->max_payload;
    if (cap < need) return -1;
    uint8_t* b = (uint8_t*)vox_mpool_alloc(p->mpool, cap);
    if (!b) return -1;
    if (p->buf && p->buf_len) memcpy(b, p->buf, p->buf_len);
    if (p->buf) vox_mpool_free(p->mpool, p->buf);
    p->buf = b;
    p->buf_cap = cap;
    return 0;
}

vox_mqtt_parser_t* vox_mqtt_parser_create(vox_mpool_t* mpool,
    const vox_mqtt_parser_config_t* config,
    const vox_mqtt_parser_callbacks_t* callbacks) {
    if (!mpool) return NULL;
    vox_mqtt_parser_t* p = (vox_mqtt_parser_t*)vox_mpool_alloc(mpool, sizeof(vox_mqtt_parser_t));
    if (!p) return NULL;
    memset(p, 0, sizeof(vox_mqtt_parser_t));
    p->mpool = mpool;
    p->max_payload = config && config->max_payload ? config->max_payload : VOX_MQTT_DEFAULT_MAX_PAYLOAD;
    if (config && config->protocol_version != 0)
        p->protocol_version = config->protocol_version;
    if (callbacks) p->cb = *callbacks;
    p->state = VOX_MQTT_STATE_FIXED_HEADER;
    /* 预分配初始 buffer，小报文（PINGREQ/CONNACK/短 PUBLISH）无需 ensure_buf，减少 alloc 与拷贝 */
    p->buf = (uint8_t*)vox_mpool_alloc(p->mpool, VOX_MQTT_PARSER_INITIAL_BUF_SIZE);
    if (p->buf) p->buf_cap = VOX_MQTT_PARSER_INITIAL_BUF_SIZE;
    return p;
}

void vox_mqtt_parser_destroy(vox_mqtt_parser_t* p) {
    if (!p) return;
    if (p->error_message && p->error_message != p->error_buf)
        vox_mpool_free(p->mpool, p->error_message);
    if (p->buf) vox_mpool_free(p->mpool, p->buf);
    vox_mpool_free(p->mpool, p);
}

void vox_mqtt_parser_reset(vox_mqtt_parser_t* p) {
    if (!p) return;
    p->state = VOX_MQTT_STATE_FIXED_HEADER;
    p->remaining_mult = 0;
    p->need = 0;
    p->buf_len = 0;
    p->protocol_version = 0;
    p->connack_session_expiry_interval = 0;
    p->connack_receive_maximum = 0;
    p->has_error = false;
    if (p->error_message) {
        if (p->error_message != p->error_buf) vox_mpool_free(p->mpool, p->error_message);
        p->error_message = NULL;
    }
}

bool vox_mqtt_parser_has_error(const vox_mqtt_parser_t* p) {
    return p && p->has_error;
}

const char* vox_mqtt_parser_get_error(const vox_mqtt_parser_t* p) {
    return p ? p->error_message : NULL;
}

void vox_mqtt_parser_set_protocol_version(vox_mqtt_parser_t* parser, uint8_t version) {
    if (parser) parser->protocol_version = version;
}

uint32_t vox_mqtt_parser_get_connack_session_expiry_interval(const vox_mqtt_parser_t* p) {
    return p ? p->connack_session_expiry_interval : 0;
}

uint16_t vox_mqtt_parser_get_connack_receive_maximum(const vox_mqtt_parser_t* p) {
    return p ? p->connack_receive_maximum : 0;
}

/* 解析 CONNECT 可变头+负载；兼容 MQTT 3.1 / 3.1.1 / 5，并设置 p->protocol_version */
static int parse_connect_payload(vox_mqtt_parser_t* p) {
    const uint8_t* d = p->buf;
    size_t len = p->buf_len;
    if (len < 8) return -1;
    uint16_t proto_len = (uint16_t)((d[0] << 8) | d[1]);
    if (proto_len != 4 && proto_len != 6) {
        set_error(p, "invalid CONNECT protocol length");
        return -1;
    }
    if (len < 2u + proto_len + 1 + 1 + 2) return -1;
    uint8_t version = d[2 + proto_len];
    if (proto_len == 4) {
        if (memcmp(d + 2, "MQTT", 4) != 0) {
            set_error(p, "invalid CONNECT protocol");
            return -1;
        }
        if (version != 4 && version != 5) {
            set_error(p, "invalid CONNECT protocol");
            return -1;
        }
    } else {
        if (memcmp(d + 2, "MQIsdp", 6) != 0 || version != 3) {
            set_error(p, "invalid CONNECT protocol");
            return -1;
        }
    }
    p->protocol_version = version;

    uint8_t flags = d[2 + proto_len + 1];
    uint16_t keepalive = (uint16_t)((d[2 + proto_len + 2] << 8) | d[2 + proto_len + 3]);
    size_t off = 2 + proto_len + 4;

    uint32_t connect_session_expiry = 0;
    uint16_t connect_receive_max = 0;
    /* MQTT 5: 可变头后为属性长度（varint），解析 0x11/0x21 后跳过整块 */
    if (version == 5) {
        uint32_t prop_len;
        size_t varint_len;
        int r = decode_varint(d + off, len - off, &prop_len, &varint_len);
        if (r <= 0) return -1;
        size_t prop_start = off + varint_len;
        size_t prop_end = prop_start + prop_len;
        if (prop_end > len) return -1;
        for (size_t pos = prop_start; pos < prop_end; ) {
            uint8_t id = d[pos++];
            int skip = property_value_skip(id, d + pos, prop_end - pos);
            if (skip <= 0) break;
            if (id == VOX_MQTT5_PROP_SESSION_EXPIRY_INTERVAL && skip >= 4)
                connect_session_expiry = (uint32_t)((d[pos] << 24) | (d[pos+1] << 16) | (d[pos+2] << 8) | d[pos+3]);
            if (id == VOX_MQTT5_PROP_RECEIVE_MAXIMUM && skip >= 2)
                connect_receive_max = (uint16_t)((d[pos] << 8) | d[pos+1]);
            pos += (size_t)skip;
        }
        off = prop_end;
    }

    const char* client_id = NULL;
    size_t client_id_len = 0;
    if (off + 2 > len) return -1;
    uint16_t n = (uint16_t)((d[off] << 8) | d[off + 1]);
    off += 2;
    if (off + n > len) return -1;
    client_id = (const char*)(d + off);
    client_id_len = n;
    off += n;

    const char* will_topic = NULL;
    size_t will_topic_len = 0;
    const void* will_msg = NULL;
    size_t will_msg_len = 0;
    if (flags & VOX_MQTT_CONNECT_FLAG_WILL) {
        if (version == 5) {
            if (off > len) return -1;
            uint32_t will_prop_len;
            size_t varint_len;
            int r = decode_varint(d + off, len - off, &will_prop_len, &varint_len);
            if (r <= 0) return -1;
            off += varint_len + will_prop_len;
        }
        if (off + 2 > len) return -1;
        n = (uint16_t)((d[off] << 8) | d[off + 1]);
        off += 2;
        if (off + n > len) return -1;
        will_topic = (const char*)(d + off);
        will_topic_len = n;
        off += n;
        if (off + 2 > len) return -1;
        n = (uint16_t)((d[off] << 8) | d[off + 1]);
        off += 2;
        if (off + n > len) return -1;
        will_msg = d + off;
        will_msg_len = n;
        off += n;
    }

    const char* username = NULL;
    size_t username_len = 0;
    if (flags & VOX_MQTT_CONNECT_FLAG_USERNAME) {
        if (off + 2 > len) return -1;
        n = (uint16_t)((d[off] << 8) | d[off + 1]);
        off += 2;
        if (off + n > len) return -1;
        username = (const char*)(d + off);
        username_len = n;
        off += n;
    }

    const char* password = NULL;
    size_t password_len = 0;
    if (flags & VOX_MQTT_CONNECT_FLAG_PASSWORD) {
        if (off + 2 > len) return -1;
        n = (uint16_t)((d[off] << 8) | d[off + 1]);
        off += 2;
        if (off + n > len) return -1;
        password = (const char*)(d + off);
        password_len = n;
    }

    if (p->cb.on_connect) {
        if (p->cb.on_connect(p->cb.user_data, client_id, client_id_len, version, keepalive, flags,
                will_topic, will_topic_len, will_msg, will_msg_len,
                username, username_len, password, password_len,
                connect_session_expiry, connect_receive_max) != 0)
            return -1;
    }
    return 0;
}

/* 解析 PUBLISH：topic(2+utf8), [packet_id if qos>0], [v5: 属性块], payload */
static int parse_publish_payload(vox_mqtt_parser_t* p) {
    const uint8_t* d = p->buf;
    size_t len = p->buf_len;
    if (len < 2) return -1;
    uint16_t topic_len = (uint16_t)((d[0] << 8) | d[1]);
    if (len < 2u + topic_len) return -1;
    const char* topic = (const char*)(d + 2);
    size_t off = 2 + topic_len;
    uint16_t packet_id = 0;
    uint8_t qos = (p->flags >> VOX_MQTT_PUBLISH_QOS_SHIFT) & VOX_MQTT_PUBLISH_MASK_QOS;
    if (qos > 0) {
        if (off + 2 > len) return -1;
        packet_id = (uint16_t)((d[off] << 8) | d[off + 1]);
        off += 2;
    }
    if (p->protocol_version == 5) {
        uint32_t prop_len;
        size_t varint_len;
        if (off >= len) return -1;
        if (decode_varint(d + off, len - off, &prop_len, &varint_len) <= 0) return -1;
        off += varint_len + prop_len;
        if (off > len) return -1;
    }
    const void* payload = off < len ? (d + off) : NULL;
    size_t payload_len = off <= len ? len - off : 0;

    if (p->cb.on_publish) {
        if (p->cb.on_publish(p->cb.user_data, qos, (p->flags >> VOX_MQTT_PUBLISH_RETAIN_SHIFT) & VOX_MQTT_PUBLISH_MASK_RETAIN, packet_id,
                topic, topic_len, payload, payload_len) != 0)
            return -1;
    }
    return 0;
}

/* 解析 SUBSCRIBE：packet_id(2) + [v5: 属性块] + (topic_filter(2+utf8) + qos/options(1)) * n */
static int parse_subscribe_payload(vox_mqtt_parser_t* p) {
    const uint8_t* d = p->buf;
    size_t len = p->buf_len;
    if (len < 2) return -1;
    uint16_t packet_id = (uint16_t)((d[0] << 8) | d[1]);
    size_t off = 2;
    if (p->protocol_version == 5) {
        uint32_t prop_len;
        size_t varint_len;
        if (off >= len) return -1;
        if (decode_varint(d + off, len - off, &prop_len, &varint_len) <= 0) return -1;
        off += varint_len + prop_len;
        if (off > len) return -1;
    }
    size_t count = 0;
    size_t off_start = off;
    while (off + 2 <= len) {
        uint16_t n = (uint16_t)((d[off] << 8) | d[off + 1]);
        off += 2;
        if (off + n + 1 > len) break;
        off += n + 1;
        count++;
    }
    uint8_t* return_codes = count ? (uint8_t*)vox_mpool_alloc(p->mpool, count) : NULL;
    if (count && !return_codes) return -1;
    off = off_start;
    size_t i = 0;
    while (off + 2 <= len && i < count) {
        uint16_t n = (uint16_t)((d[off] << 8) | d[off + 1]);
        off += 2;
        if (off + n + 1 > len) break;
        const char* topic = (const char*)(d + off);
        off += n;
        uint8_t qos_or_options = d[off];
        off += 1;
        uint8_t qos = (p->protocol_version == 5) ? (qos_or_options & VOX_MQTT_PUBLISH_MASK_QOS) : qos_or_options;
        int granted = VOX_MQTT_SUBACK_FAILURE;
        if (p->cb.on_subscribe) {
            int r = p->cb.on_subscribe(p->cb.user_data, packet_id, topic, n, qos);
            granted = (r >= 0 && r <= 2) ? (uint8_t)r : VOX_MQTT_SUBACK_FAILURE;
        }
        return_codes[i++] = (uint8_t)granted;
    }
    if (p->cb.on_subscribe_done) {
        if (p->cb.on_subscribe_done(p->cb.user_data, packet_id, return_codes, i) != 0) {
            if (return_codes) vox_mpool_free(p->mpool, return_codes);
            return -1;
        }
    }
    if (return_codes) vox_mpool_free(p->mpool, return_codes);
    return 0;
}

/* 解析 SUBACK：packet_id(2) + [v5: 属性长度 varint + 属性块] + return_codes[]/reason_codes[] */
static int parse_suback_payload(vox_mqtt_parser_t* p) {
    if (p->buf_len < 2) return -1;
    uint16_t packet_id = (uint16_t)((p->buf[0] << 8) | p->buf[1]);
    size_t off = 2;
    if (p->protocol_version == 5) {
        uint32_t prop_len;
        size_t varint_len;
        if (off >= p->buf_len) return -1;
        if (decode_varint(p->buf + off, p->buf_len - off, &prop_len, &varint_len) <= 0) return -1;
        off += varint_len + prop_len;
        if (off > p->buf_len) return -1;
    }
    if (p->buf_len > off && p->cb.on_suback) {
        if (p->cb.on_suback(p->cb.user_data, packet_id, p->buf + off, p->buf_len - off) != 0) return -1;
    } else if (p->buf_len == off && p->cb.on_suback) {
        if (p->cb.on_suback(p->cb.user_data, packet_id, NULL, 0) != 0) return -1;
    }
    return 0;
}

/* 解析 UNSUBSCRIBE：packet_id(2) + [v5: 属性块] + topic_filter(2+utf8)*n */
static int parse_unsubscribe_payload(vox_mqtt_parser_t* p) {
    const uint8_t* d = p->buf;
    size_t len = p->buf_len;
    if (len < 2) return -1;
    uint16_t packet_id = (uint16_t)((d[0] << 8) | d[1]);
    size_t off = 2;
    if (p->protocol_version == 5) {
        uint32_t prop_len;
        size_t varint_len;
        if (off >= len) return -1;
        if (decode_varint(d + off, len - off, &prop_len, &varint_len) <= 0) return -1;
        off += varint_len + prop_len;
        if (off > len) return -1;
    }
    while (off + 2 <= len) {
        uint16_t n = (uint16_t)((d[off] << 8) | d[off + 1]);
        off += 2;
        if (off + n > len) break;
        const char* topic = (const char*)(d + off);
        off += n;
        if (p->cb.on_unsubscribe) {
            if (p->cb.on_unsubscribe(p->cb.user_data, packet_id, topic, n) != 0) return -1;
        }
    }
    return 0;
}

/* 解析 PUBACK/PUBREC/PUBREL/PUBCOMP：packet_id(2)；v5 时其后为属性长度 varint + 属性块，需校验并跳过 */
static int parse_packet_id_with_v5_props(vox_mqtt_parser_t* p, uint16_t* packet_id) {
    if (p->buf_len < 2) return -1;
    *packet_id = (uint16_t)((p->buf[0] << 8) | p->buf[1]);
    if (p->protocol_version == 5 && p->buf_len > 2) {
        uint32_t prop_len;
        size_t varint_len;
        if (decode_varint(p->buf + 2, p->buf_len - 2, &prop_len, &varint_len) <= 0) return -1;
        if (2 + varint_len + prop_len > p->buf_len) return -1;
    }
    return 0;
}

/* 解析 UNSUBACK：packet_id(2)；v5 还有属性 varint + 属性块 + reason_codes[]，校验并跳过属性 */
static int parse_unsuback_payload(vox_mqtt_parser_t* p, uint16_t* packet_id) {
    if (p->buf_len < 2) return -1;
    *packet_id = (uint16_t)((p->buf[0] << 8) | p->buf[1]);
    if (p->protocol_version == 5 && p->buf_len > 2) {
        uint32_t prop_len;
        size_t varint_len;
        if (decode_varint(p->buf + 2, p->buf_len - 2, &prop_len, &varint_len) <= 0) return -1;
        if (2 + varint_len + prop_len > p->buf_len) return -1;
    }
    return 0;
}

ssize_t vox_mqtt_parser_execute(vox_mqtt_parser_t* p, const char* data, size_t len) {
    if (!p || p->state == VOX_MQTT_STATE_ERROR) return -1;
    const uint8_t* cur = (const uint8_t*)data;
    size_t cur_len = len;
    size_t consumed = 0;

    for (;;) {
        if (p->state == VOX_MQTT_STATE_FIXED_HEADER) {
            if (cur_len < 1) return (ssize_t)consumed;
            p->type = (cur[0] >> 4) & 15;
            p->flags = cur[0] & 15;
            if (p->type < 1 || p->type > 15) {
                set_error(p, "invalid packet type");
                return -1;
            }
            if (p->type == VOX_MQTT_PKT_AUTH) {
                set_error(p, "AUTH not supported");
                return -1;
            }
            cur++; cur_len--; consumed++;
            p->state = VOX_MQTT_STATE_REMAINING_LEN;
            p->remaining_mult = 0;
        }

        if (p->state == VOX_MQTT_STATE_REMAINING_LEN) {
            uint32_t val;
            size_t num_bytes;
            int r = decode_remaining_length(cur, cur_len, &val, &num_bytes);
            if (r < 0) {
                set_error(p, "invalid remaining length");
                return -1;
            }
            if (r == 0) return (ssize_t)consumed;
            p->remaining_len = val;
            cur += num_bytes;
            cur_len -= num_bytes;
            consumed += num_bytes;

            if (p->remaining_len > p->max_payload) {
                set_error(p, "payload exceeds max");
                return -1;
            }

            p->need = p->remaining_len;
            p->buf_len = 0;
            if (p->need == 0) {
                /* 无可变头/负载，直接处理 */
                switch (p->type) {
                    case VOX_MQTT_PKT_PINGREQ:
                        if (p->cb.on_pingreq) p->cb.on_pingreq(p->cb.user_data);
                        break;
                    case VOX_MQTT_PKT_PINGRESP:
                        if (p->cb.on_pingresp) p->cb.on_pingresp(p->cb.user_data);
                        break;
                    case VOX_MQTT_PKT_DISCONNECT:
                        if (p->cb.on_disconnect) p->cb.on_disconnect(p->cb.user_data);
                        break;
                    default:
                        set_error(p, "unexpected empty packet");
                        return -1;
                }
                p->state = VOX_MQTT_STATE_FIXED_HEADER;
                continue;
            }
            if (ensure_buf(p, p->need) != 0) {
                set_error(p, "alloc failed");
                return -1;
            }
            p->state = VOX_MQTT_STATE_VARHEADER_PAYLOAD;
        }

        if (p->state == VOX_MQTT_STATE_VARHEADER_PAYLOAD) {
            size_t take = cur_len;
            if (take > p->need) take = p->need;
            if (take > 0) {
                memcpy(p->buf + p->buf_len, cur, take);
                p->buf_len += take;
                cur += take;
                cur_len -= take;
                consumed += take;
                p->need -= (uint32_t)take;
            }
            if (p->need != 0) return (ssize_t)consumed;

            /* 完整报文已读入 p->buf */
            int err = 0;
            switch (p->type) {
                case VOX_MQTT_PKT_CONNECT:
                    err = parse_connect_payload(p);
                    break;
                case VOX_MQTT_PKT_CONNACK: {
                    if (p->buf_len >= 2) {
                        if (p->protocol_version == 5 && p->buf_len > 2) {
                            p->connack_session_expiry_interval = 0;
                            p->connack_receive_maximum = 0;
                            size_t off = 2;
                            uint32_t prop_len;
                            size_t varint_len;
                            if (decode_varint(p->buf + off, p->buf_len - off, &prop_len, &varint_len) > 0) {
                                off += varint_len;
                                size_t prop_end = off + prop_len;
                                if (prop_end <= p->buf_len) {
                                    for (; off < prop_end; ) {
                                        uint8_t id = p->buf[off++];
                                        int skip = property_value_skip(id, p->buf + off, prop_end - off);
                                        if (skip <= 0) break;
                                        if (id == VOX_MQTT5_PROP_SESSION_EXPIRY_INTERVAL && skip >= 4)
                                            p->connack_session_expiry_interval = (uint32_t)((p->buf[off]<<24)|(p->buf[off+1]<<16)|(p->buf[off+2]<<8)|p->buf[off+3]);
                                        if (id == VOX_MQTT5_PROP_RECEIVE_MAXIMUM && skip >= 2)
                                            p->connack_receive_maximum = (uint16_t)((p->buf[off]<<8)|p->buf[off+1]);
                                        off += (size_t)skip;
                                    }
                                }
                            }
                        }
                        if (p->cb.on_connack)
                            err = p->cb.on_connack(p->cb.user_data, p->buf[0] & VOX_MQTT_CONNACK_FLAG_SESSION_PRESENT, p->buf[1]);
                    }
                    break;
                }
                case VOX_MQTT_PKT_PUBLISH:
                    err = parse_publish_payload(p);
                    break;
                case VOX_MQTT_PKT_PUBACK: {
                    uint16_t id;
                    if (parse_packet_id_with_v5_props(p, &id) == 0) {
                        if (p->cb.on_puback) err = p->cb.on_puback(p->cb.user_data, id);
                    } else err = -1;
                    break;
                }
                case VOX_MQTT_PKT_PUBREC: {
                    uint16_t id;
                    if (parse_packet_id_with_v5_props(p, &id) == 0) {
                        if (p->cb.on_pubrec) err = p->cb.on_pubrec(p->cb.user_data, id);
                    } else err = -1;
                    break;
                }
                case VOX_MQTT_PKT_PUBREL: {
                    uint16_t id;
                    if (parse_packet_id_with_v5_props(p, &id) == 0) {
                        if (p->cb.on_pubrel) err = p->cb.on_pubrel(p->cb.user_data, id);
                    } else err = -1;
                    break;
                }
                case VOX_MQTT_PKT_PUBCOMP: {
                    uint16_t id;
                    if (parse_packet_id_with_v5_props(p, &id) == 0) {
                        if (p->cb.on_pubcomp) err = p->cb.on_pubcomp(p->cb.user_data, id);
                    } else err = -1;
                    break;
                }
                case VOX_MQTT_PKT_SUBSCRIBE:
                    err = parse_subscribe_payload(p);
                    break;
                case VOX_MQTT_PKT_SUBACK:
                    err = parse_suback_payload(p);
                    break;
                case VOX_MQTT_PKT_UNSUBSCRIBE:
                    err = parse_unsubscribe_payload(p);
                    break;
                case VOX_MQTT_PKT_UNSUBACK: {
                    uint16_t id;
                    if (parse_unsuback_payload(p, &id) == 0 && p->cb.on_unsuback)
                        err = p->cb.on_unsuback(p->cb.user_data, id);
                    else err = -1;
                    break;
                }
                default:
                    set_error(p, "unsupported packet type");
                    err = -1;
                    break;
            }
            if (err != 0) return -1;
            p->state = VOX_MQTT_STATE_FIXED_HEADER;
        }
    }
}

/* ===== 编码实现 ===== */

int vox_mqtt_encode_remaining_length(uint8_t* buf, size_t value) {
    size_t n = 0;
    for (;;) {
        uint8_t b = (uint8_t)(value % 128);
        value /= 128;
        if (value) b |= 128;
        if (buf) buf[n] = b;
        n++;
        if (value == 0) break;
        if (n >= 4) return -1;
    }
    return (int)n;
}

/* MQTT 5 可变字节整数编码，返回写入字节数 */
static int encode_varint(uint8_t* buf, uint32_t value) {
    int n = 0;
    do {
        uint8_t b = (uint8_t)(value % 128);
        value /= 128;
        if (value) b |= 128;
        if (buf) buf[n] = b;
        n++;
    } while (value && n < 4);
    return n;
}

static size_t write_utf8(uint8_t* buf, size_t buf_size, const char* s, size_t len) {
    if (len > 65535) return 0;
    if (buf_size < 2 + len) return 0;
    if (buf) {
        buf[0] = (uint8_t)(len >> 8);
        buf[1] = (uint8_t)(len & 0xff);
        if (len) memcpy(buf + 2, s, len);
    }
    return 2 + len;
}

size_t vox_mqtt_encode_connect(uint8_t* buf, size_t buf_size,
    const char* client_id, size_t client_id_len,
    uint16_t keepalive, bool clean_session,
    const char* will_topic, size_t will_topic_len,
    const void* will_msg, size_t will_msg_len, uint8_t will_qos, bool will_retain,
    const char* username, size_t username_len,
    const char* password, size_t password_len) {
    size_t need = 2 + 4 + 2; /* "MQTT" len + "MQTT" + version + flags */
    need += 2; /* keepalive */
    need += 2 + client_id_len;
    uint8_t flags = clean_session ? VOX_MQTT_CONNECT_FLAG_CLEAN_SESSION : 0;
    if (will_topic && will_topic_len) {
        flags |= VOX_MQTT_CONNECT_FLAG_WILL | (will_qos << VOX_MQTT_CONNECT_FLAG_WILL_QOS_SHIFT) | (will_retain ? VOX_MQTT_CONNECT_FLAG_WILL_RETAIN : 0);
        need += 2 + will_topic_len + 2 + will_msg_len;
    }
    if (username && username_len) { flags |= VOX_MQTT_CONNECT_FLAG_USERNAME; need += 2 + username_len; }
    if (password && password_len) { flags |= VOX_MQTT_CONNECT_FLAG_PASSWORD; need += 2 + password_len; }

    int rl_len = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl_len < 0) return 0;
    size_t total = 1 + (size_t)rl_len + need;
    if (buf_size < total) return total;

    buf[0] = (VOX_MQTT_PKT_CONNECT << 4);
    vox_mqtt_encode_remaining_length(buf + 1, need);
    size_t off = 1 + (size_t)rl_len;
    if (buf) {
        buf[off++] = 0;
        buf[off++] = 4;
        memcpy(buf + off, "MQTT", 4);
        off += 4;
        buf[off++] = 4; /* version 3.1.1 */
        buf[off++] = flags;
        buf[off++] = (uint8_t)(keepalive >> 8);
        buf[off++] = (uint8_t)(keepalive & 0xff);
        off += write_utf8(buf + off, buf_size - off, client_id, client_id_len);
        if (will_topic && will_topic_len) {
            off += write_utf8(buf + off, buf_size - off, will_topic, will_topic_len);
            off += write_utf8(buf + off, buf_size - off, (const char*)will_msg, will_msg_len);
        }
        if (username && username_len) off += write_utf8(buf + off, buf_size - off, username, username_len);
        if (password && password_len) off += write_utf8(buf + off, buf_size - off, password, password_len);
    }
    return total;
}

size_t vox_mqtt_encode_connect_v5(uint8_t* buf, size_t buf_size,
    const char* client_id, size_t client_id_len,
    uint16_t keepalive, bool clean_session,
    const char* will_topic, size_t will_topic_len,
    const void* will_msg, size_t will_msg_len, uint8_t will_qos, bool will_retain,
    const char* username, size_t username_len,
    const char* password, size_t password_len,
    uint32_t session_expiry_interval, uint16_t receive_maximum) {
    size_t need = 2 + 4 + 2; /* "MQTT" len + "MQTT" + version 5 + flags */
    need += 2; /* keepalive */
    size_t prop_len = 0;
    if (session_expiry_interval != 0) prop_len += 1 + 4;
    if (receive_maximum != 0) prop_len += 1 + 2;
    need += encode_varint(NULL, (uint32_t)prop_len) + prop_len;
    need += 2 + client_id_len;
    uint8_t flags = clean_session ? VOX_MQTT_CONNECT_FLAG_CLEAN_SESSION : 0;
    if (will_topic && will_topic_len) {
        flags |= VOX_MQTT_CONNECT_FLAG_WILL | (will_qos << VOX_MQTT_CONNECT_FLAG_WILL_QOS_SHIFT) | (will_retain ? VOX_MQTT_CONNECT_FLAG_WILL_RETAIN : 0);
        need += 2 + will_topic_len + 2 + will_msg_len;
    }
    if (username && username_len) { flags |= VOX_MQTT_CONNECT_FLAG_USERNAME; need += 2 + username_len; }
    if (password && password_len) { flags |= VOX_MQTT_CONNECT_FLAG_PASSWORD; need += 2 + password_len; }

    int rl_len = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl_len < 0) return 0;
    size_t total = 1 + (size_t)rl_len + need;
    if (buf_size < total) return total;

    if (buf) {
        buf[0] = (VOX_MQTT_PKT_CONNECT << 4);
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl_len;
        buf[off++] = 0;
        buf[off++] = 4;
        memcpy(buf + off, "MQTT", 4);
        off += 4;
        buf[off++] = 5; /* MQTT 5 */
        buf[off++] = flags;
        buf[off++] = (uint8_t)(keepalive >> 8);
        buf[off++] = (uint8_t)(keepalive & 0xff);
        off += encode_varint(buf + off, (uint32_t)prop_len);
        if (session_expiry_interval != 0) {
            buf[off++] = VOX_MQTT5_PROP_SESSION_EXPIRY_INTERVAL;
            buf[off++] = (uint8_t)(session_expiry_interval >> 24);
            buf[off++] = (uint8_t)(session_expiry_interval >> 16);
            buf[off++] = (uint8_t)(session_expiry_interval >> 8);
            buf[off++] = (uint8_t)(session_expiry_interval & 0xff);
        }
        if (receive_maximum != 0) {
            buf[off++] = VOX_MQTT5_PROP_RECEIVE_MAXIMUM;
            buf[off++] = (uint8_t)(receive_maximum >> 8);
            buf[off++] = (uint8_t)(receive_maximum & 0xff);
        }
        off += write_utf8(buf + off, buf_size - off, client_id, client_id_len);
        if (will_topic && will_topic_len) {
            off += write_utf8(buf + off, buf_size - off, will_topic, will_topic_len);
            off += write_utf8(buf + off, buf_size - off, (const char*)will_msg, will_msg_len);
        }
        if (username && username_len) off += write_utf8(buf + off, buf_size - off, username, username_len);
        if (password && password_len) off += write_utf8(buf + off, buf_size - off, password, password_len);
    }
    return total;
}

size_t vox_mqtt_encode_connack(uint8_t* buf, size_t buf_size, uint8_t session_present, uint8_t return_code) {
    size_t need = 2;
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_CONNACK << 4);
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = session_present & VOX_MQTT_CONNACK_FLAG_SESSION_PRESENT;
        buf[off] = return_code;
    }
    return total;
}

size_t vox_mqtt_encode_publish(uint8_t* buf, size_t buf_size,
    uint8_t qos, bool retain, uint16_t packet_id,
    const char* topic, size_t topic_len,
    const void* payload, size_t payload_len) {
    if (topic_len > 65535) return 0;
    size_t need = 2 + topic_len + (qos ? 2 : 0) + payload_len;
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_PUBLISH << 4) | (retain ? VOX_MQTT_PUBLISH_MASK_RETAIN : 0) | (qos << VOX_MQTT_PUBLISH_QOS_SHIFT);
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = (uint8_t)(topic_len >> 8);
        buf[off++] = (uint8_t)(topic_len & 0xff);
        memcpy(buf + off, topic, topic_len);
        off += topic_len;
        if (qos) {
            buf[off++] = (uint8_t)(packet_id >> 8);
            buf[off++] = (uint8_t)(packet_id & 0xff);
        }
        if (payload_len) memcpy(buf + off, payload, payload_len);
    }
    return total;
}

static size_t encode_pub_ack(uint8_t* buf, size_t buf_size, uint8_t type, uint16_t packet_id) {
    size_t need = 2;
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        uint8_t flags = (type == VOX_MQTT_PKT_PUBREL) ? VOX_MQTT_PUBREL_RESERVED : 0;
        buf[0] = (type << 4) | flags;
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = (uint8_t)(packet_id >> 8);
        buf[off] = (uint8_t)(packet_id & 0xff);
    }
    return total;
}

size_t vox_mqtt_encode_puback(uint8_t* buf, size_t buf_size, uint16_t packet_id) {
    return encode_pub_ack(buf, buf_size, VOX_MQTT_PKT_PUBACK, packet_id);
}
size_t vox_mqtt_encode_pubrec(uint8_t* buf, size_t buf_size, uint16_t packet_id) {
    return encode_pub_ack(buf, buf_size, VOX_MQTT_PKT_PUBREC, packet_id);
}
size_t vox_mqtt_encode_pubrel(uint8_t* buf, size_t buf_size, uint16_t packet_id) {
    return encode_pub_ack(buf, buf_size, VOX_MQTT_PKT_PUBREL, packet_id);
}
size_t vox_mqtt_encode_pubcomp(uint8_t* buf, size_t buf_size, uint16_t packet_id) {
    return encode_pub_ack(buf, buf_size, VOX_MQTT_PKT_PUBCOMP, packet_id);
}

size_t vox_mqtt_encode_subscribe(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const char* const* topic_filters, const size_t* topic_lens, const uint8_t* qos_list, size_t count) {
    size_t need = 2;
    for (size_t i = 0; i < count; i++) {
        size_t L = topic_lens[i];
        if (L > 65535) return 0;
        need += 2 + L + 1;
    }
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_SUBSCRIBE << 4) | VOX_MQTT_SUBSCRIBE_RESERVED;
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = (uint8_t)(packet_id >> 8);
        buf[off++] = (uint8_t)(packet_id & 0xff);
        for (size_t i = 0; i < count; i++) {
            size_t L = topic_lens[i];
            buf[off++] = (uint8_t)(L >> 8);
            buf[off++] = (uint8_t)(L & 0xff);
            memcpy(buf + off, topic_filters[i], L);
            off += L;
            buf[off++] = qos_list ? qos_list[i] : 0;
        }
    }
    return total;
}

size_t vox_mqtt_encode_suback(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const uint8_t* return_codes, size_t count) {
    size_t need = 2 + count;
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_SUBACK << 4);
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = (uint8_t)(packet_id >> 8);
        buf[off++] = (uint8_t)(packet_id & 0xff);
        if (count && return_codes) memcpy(buf + off, return_codes, count);
    }
    return total;
}

size_t vox_mqtt_encode_unsubscribe(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const char* const* topic_filters, const size_t* topic_lens, size_t count) {
    size_t need = 2;
    for (size_t i = 0; i < count; i++) {
        size_t L = topic_lens[i];
        if (L > 65535) return 0;
        need += 2 + L;
    }
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_UNSUBSCRIBE << 4) | VOX_MQTT_UNSUBSCRIBE_RESERVED;
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = (uint8_t)(packet_id >> 8);
        buf[off++] = (uint8_t)(packet_id & 0xff);
        for (size_t i = 0; i < count; i++) {
            size_t L = topic_lens[i];
            buf[off++] = (uint8_t)(L >> 8);
            buf[off++] = (uint8_t)(L & 0xff);
            memcpy(buf + off, topic_filters[i], L);
            off += L;
        }
    }
    return total;
}

size_t vox_mqtt_encode_unsuback(uint8_t* buf, size_t buf_size, uint16_t packet_id) {
    return encode_pub_ack(buf, buf_size, VOX_MQTT_PKT_UNSUBACK, packet_id);
}

/* ===== MQTT 5 编码：CONNECT v5 已在上方，以下为 SUBSCRIBE/UNSUBSCRIBE v5 ===== */
size_t vox_mqtt_encode_subscribe_v5(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const char* const* topic_filters, const size_t* topic_lens, const uint8_t* qos_list, size_t count) {
    size_t need = 2 + 1; /* packet_id + varint(0) */
    for (size_t i = 0; i < count; i++) {
        size_t L = topic_lens[i];
        if (L > 65535) return 0;
        need += 2 + L + 1;
    }
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_SUBSCRIBE << 4) | VOX_MQTT_SUBSCRIBE_RESERVED;
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = (uint8_t)(packet_id >> 8);
        buf[off++] = (uint8_t)(packet_id & 0xff);
        off += encode_varint(buf + off, 0);
        for (size_t i = 0; i < count; i++) {
            size_t L = topic_lens[i];
            buf[off++] = (uint8_t)(L >> 8);
            buf[off++] = (uint8_t)(L & 0xff);
            memcpy(buf + off, topic_filters[i], L);
            off += L;
            buf[off++] = qos_list ? qos_list[i] : 0;
        }
    }
    return total;
}

size_t vox_mqtt_encode_unsubscribe_v5(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const char* const* topic_filters, const size_t* topic_lens, size_t count) {
    size_t need = 2 + 1; /* packet_id + varint(0) */
    for (size_t i = 0; i < count; i++) {
        size_t L = topic_lens[i];
        if (L > 65535) return 0;
        need += 2 + L;
    }
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_UNSUBSCRIBE << 4) | VOX_MQTT_UNSUBSCRIBE_RESERVED;
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = (uint8_t)(packet_id >> 8);
        buf[off++] = (uint8_t)(packet_id & 0xff);
        off += encode_varint(buf + off, 0);
        for (size_t i = 0; i < count; i++) {
            size_t L = topic_lens[i];
            buf[off++] = (uint8_t)(L >> 8);
            buf[off++] = (uint8_t)(L & 0xff);
            memcpy(buf + off, topic_filters[i], L);
            off += L;
        }
    }
    return total;
}

/* ===== MQTT 5 编码实现（CONNACK/PUBLISH/SUBACK/UNSUBACK/DISCONNECT v5） ===== */
size_t vox_mqtt_encode_connack_v5(uint8_t* buf, size_t buf_size, uint8_t session_present, uint8_t reason_code,
    uint32_t session_expiry_interval, uint16_t receive_maximum) {
    size_t prop_len = 0;
    if (session_expiry_interval != 0) prop_len += 1 + 4;
    if (receive_maximum != 0) prop_len += 1 + 2;
    size_t varh = 1 + 1 + encode_varint(NULL, (uint32_t)prop_len) + prop_len;
    int rl = vox_mqtt_encode_remaining_length(NULL, varh);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + varh;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_CONNACK << 4);
        vox_mqtt_encode_remaining_length(buf + 1, varh);
        size_t off = 1 + (size_t)rl;
        buf[off++] = session_present & VOX_MQTT_CONNACK_FLAG_SESSION_PRESENT;
        buf[off++] = reason_code;
        off += encode_varint(buf + off, (uint32_t)prop_len);
        if (session_expiry_interval != 0) {
            buf[off++] = VOX_MQTT5_PROP_SESSION_EXPIRY_INTERVAL;
            buf[off++] = (uint8_t)(session_expiry_interval >> 24);
            buf[off++] = (uint8_t)(session_expiry_interval >> 16);
            buf[off++] = (uint8_t)(session_expiry_interval >> 8);
            buf[off++] = (uint8_t)(session_expiry_interval & 0xff);
        }
        if (receive_maximum != 0) {
            buf[off++] = VOX_MQTT5_PROP_RECEIVE_MAXIMUM;
            buf[off++] = (uint8_t)(receive_maximum >> 8);
            buf[off++] = (uint8_t)(receive_maximum & 0xff);
        }
    }
    return total;
}

size_t vox_mqtt_encode_publish_v5(uint8_t* buf, size_t buf_size,
    uint8_t qos, bool retain, uint16_t packet_id,
    const char* topic, size_t topic_len,
    const void* payload, size_t payload_len) {
    if (topic_len > 65535) return 0;
    size_t need = 2 + topic_len + (qos ? 2 : 0) + 1 + payload_len; /* +1 for varint(0) property length */
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_PUBLISH << 4) | (retain ? VOX_MQTT_PUBLISH_MASK_RETAIN : 0) | (qos << VOX_MQTT_PUBLISH_QOS_SHIFT);
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = (uint8_t)(topic_len >> 8);
        buf[off++] = (uint8_t)(topic_len & 0xff);
        memcpy(buf + off, topic, topic_len);
        off += topic_len;
        if (qos) {
            buf[off++] = (uint8_t)(packet_id >> 8);
            buf[off++] = (uint8_t)(packet_id & 0xff);
        }
        off += encode_varint(buf + off, 0);
        if (payload_len) memcpy(buf + off, payload, payload_len);
    }
    return total;
}

size_t vox_mqtt_encode_suback_v5(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const uint8_t* reason_codes, size_t count) {
    size_t need = 2 + 1 + count; /* packet_id + varint(0) + reason_codes */
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_SUBACK << 4);
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = (uint8_t)(packet_id >> 8);
        buf[off++] = (uint8_t)(packet_id & 0xff);
        off += encode_varint(buf + off, 0);
        if (count && reason_codes) memcpy(buf + off, reason_codes, count);
    }
    return total;
}

size_t vox_mqtt_encode_unsuback_v5(uint8_t* buf, size_t buf_size, uint16_t packet_id,
    const uint8_t* reason_codes, size_t count) {
    size_t need = 2 + 1 + count;
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_UNSUBACK << 4);
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = (uint8_t)(packet_id >> 8);
        buf[off++] = (uint8_t)(packet_id & 0xff);
        off += encode_varint(buf + off, 0);
        if (count && reason_codes) memcpy(buf + off, reason_codes, count);
    }
    return total;
}

size_t vox_mqtt_encode_disconnect_v5(uint8_t* buf, size_t buf_size, uint8_t reason_code) {
    size_t need = 1 + 1; /* reason_code + varint(0) */
    int rl = vox_mqtt_encode_remaining_length(NULL, need);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl + need;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (VOX_MQTT_PKT_DISCONNECT << 4);
        vox_mqtt_encode_remaining_length(buf + 1, need);
        size_t off = 1 + (size_t)rl;
        buf[off++] = reason_code;
        encode_varint(buf + off, 0);
    }
    return total;
}

static size_t encode_simple(uint8_t* buf, size_t buf_size, uint8_t type) {
    int rl = vox_mqtt_encode_remaining_length(NULL, 0);
    if (rl < 0) return 0;
    size_t total = 1 + (size_t)rl;
    if (buf_size < total) return total;
    if (buf) {
        buf[0] = (type << 4);
        vox_mqtt_encode_remaining_length(buf + 1, 0);
    }
    return total;
}

size_t vox_mqtt_encode_pingreq(uint8_t* buf, size_t buf_size) {
    return encode_simple(buf, buf_size, VOX_MQTT_PKT_PINGREQ);
}
size_t vox_mqtt_encode_pingresp(uint8_t* buf, size_t buf_size) {
    return encode_simple(buf, buf_size, VOX_MQTT_PKT_PINGRESP);
}
size_t vox_mqtt_encode_disconnect(uint8_t* buf, size_t buf_size) {
    return encode_simple(buf, buf_size, VOX_MQTT_PKT_DISCONNECT);
}
