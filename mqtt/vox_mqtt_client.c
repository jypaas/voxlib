/*
 * vox_mqtt_client.c - MQTT 客户端实现（TCP / TLS / WS / WSS）
 */

#include "vox_mqtt_client.h"
#include "../vox_handle.h"
#include "../vox_list.h"
#include "../vox_log.h"
#include "../vox_timer.h"
#include <string.h>
#include <stdio.h>
#if defined(VOX_USE_SSL) && VOX_USE_SSL
#include "../vox_tls.h"
#endif
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
#include "../websocket/vox_websocket_client.h"
#endif

#ifndef VOX_CONTAINING_RECORD
#define VOX_CONTAINING_RECORD(ptr, type, member) vox_container_of(ptr, type, member)
#endif

/* QoS 1 出站：等待 PUBACK 确认的消息 */
typedef struct vox_mqtt_pending_qos1 {
    vox_list_node_t node;
    uint16_t packet_id;
    uint8_t* packet_buf;      /* 完整的 PUBLISH 报文（用于重传） */
    size_t packet_len;
    uint64_t send_time;       /* 发送时间戳（毫秒） */
    uint8_t retry_count;      /* 重试次数 */
} vox_mqtt_pending_qos1_t;

/* QoS 2 出站：等待 PUBREC/PUBCOMP 的消息 */
typedef struct vox_mqtt_pending_qos2_out {
    vox_list_node_t node;
    uint16_t packet_id;
    uint8_t* packet_buf;      /* 完整的 PUBLISH 报文（用于重传） */
    size_t packet_len;
    uint8_t state;            /* 0=等 PUBREC，1=等 PUBCOMP */
    uint64_t send_time;       /* 发送时间戳（毫秒） */
    uint8_t retry_count;      /* 重试次数 */
} vox_mqtt_pending_qos2_out_t;

/* 订阅状态：记录已订阅的 topic */
typedef struct vox_mqtt_subscription {
    vox_list_node_t node;
    char* topic_filter;
    size_t topic_filter_len;
    uint8_t qos;
} vox_mqtt_subscription_t;

/* QoS 2 入站：收到 PUBLISH qos2 后暂存，等 PUBREL 时投递 */
typedef struct vox_mqtt_pending_qos2_in {
    vox_list_node_t node;
    uint16_t packet_id;
    char* topic;
    size_t topic_len;
    void* payload;
    size_t payload_len;
    bool retain;
} vox_mqtt_pending_qos2_in_t;

/* 写请求：tcp/tls_write 完成后释放 */
typedef struct vox_mqtt_write_req {
    vox_list_node_t node;
    uint8_t* buf;
    size_t len;
} vox_mqtt_write_req_t;

struct vox_mqtt_client {
    vox_mpool_t* mpool;
    vox_loop_t* loop;
    vox_tcp_t* tcp;
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    vox_tls_t* tls;
#endif
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    vox_ws_client_t* ws_client;
#endif
    vox_dns_getaddrinfo_t* dns_req;
    vox_mqtt_parser_t* parser;

    bool connecting;
    bool connected;
    char* host;
    uint16_t port;
    uint16_t keepalive_sec;
    vox_timer_t ping_timer;

    vox_mqtt_connect_cb connect_cb;
    void* connect_user_data;

    vox_mqtt_message_cb message_cb;
    void* message_user_data;
    vox_mqtt_disconnect_cb disconnect_cb;
    void* disconnect_user_data;
    vox_mqtt_error_cb error_cb;
    void* error_user_data;

    uint16_t next_packet_id;
    vox_mqtt_suback_cb pending_suback_cb;
    void* pending_suback_user_data;
    uint16_t pending_suback_packet_id;

    /* 订阅状态管理 */
    vox_list_t subscriptions;  /* 已订阅的 topic 列表 */

    uint8_t* pending_connect_buf;
    size_t pending_connect_len;

    uint8_t protocol_version; /* 4=3.1.1, 5=MQTT 5，连接成功后用于选择编码 */

    /* QoS 1 出站：等待 PUBACK 的消息队列 */
    vox_list_t pending_qos1_list;
    vox_timer_t qos_retry_timer;      /* QoS 重传定时器 */
    uint32_t qos_retry_interval_ms;   /* 重传间隔（默认 5000ms） */
    uint8_t qos_max_retry;            /* 最大重试次数（默认 3） */

    /* QoS 2 出站：等待 PUBREC/PUBCOMP 的消息队列（支持多路并发） */
    vox_list_t pending_qos2_out_list;
    /* QoS 2 入站：等 PUBREL 后投递 */
    vox_list_t pending_qos2_in_list;

    vox_list_t write_queue;

    bool transport_close_pending;  /* true 表示已排队延迟关闭，disconnect() 不再 destroy tls/tcp */

    /* 自动重连 */
    bool auto_reconnect_enabled;        /* 是否启用自动重连 */
    uint32_t max_reconnect_attempts;    /* 最大重连次数，0 表示无限 */
    uint32_t initial_reconnect_delay_ms; /* 初始重连延迟 */
    uint32_t max_reconnect_delay_ms;    /* 最大重连延迟 */
    uint32_t reconnect_attempts;        /* 当前重连次数 */
    uint32_t current_reconnect_delay_ms; /* 当前重连延迟 */
    vox_timer_t reconnect_timer;        /* 重连定时器 */
    vox_mqtt_connect_options_t* saved_options; /* 保存的连接选项（用于重连） */
};

static void connect_cleanup_on_fail(vox_mqtt_client_t* c);

/* 重连定时器回调 */
static void reconnect_timer_cb(vox_timer_t* timer, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)timer;
    if (!c || !c->auto_reconnect_enabled || !c->saved_options) return;

    VOX_LOG_DEBUG("MQTT client: attempting reconnect (attempt %u)", c->reconnect_attempts + 1);

    /* 停止重连定时器 */
    vox_timer_stop(&c->reconnect_timer);

    /* 尝试重新连接 */
    int ret = vox_mqtt_client_connect(c, c->host, c->port, c->saved_options,
        c->connect_cb, c->connect_user_data);
    if (ret != 0) {
        VOX_LOG_ERROR("MQTT client: reconnect failed");
        /* 连接失败会触发 client_fail，进而再次调度重连 */
    }
}

static void client_fail(vox_mqtt_client_t* c, const char* msg) {
    if (!c) return;
    if (c->connecting) connect_cleanup_on_fail(c);
    c->connected = false;
    c->connecting = false;

    /* 保存回调和用户数据，避免回调中销毁 client 后继续访问 */
    vox_mqtt_connect_cb connect_cb = c->connect_cb;
    void* connect_ud = c->connect_user_data;
    vox_mqtt_error_cb error_cb = c->error_cb;
    void* error_ud = c->error_user_data;
    vox_mqtt_disconnect_cb disconnect_cb = c->disconnect_cb;
    void* disconnect_ud = c->disconnect_user_data;

    /* 清除回调，防止重入 */
    c->connect_cb = NULL;
    c->connect_user_data = NULL;

    /* 检查是否需要自动重连（在调用回调之前） */
    //bool should_reconnect = false;
    if (c->auto_reconnect_enabled && c->saved_options && c->host) {
        /* 检查是否达到最大重连次数 */
        if (c->max_reconnect_attempts == 0 || c->reconnect_attempts < c->max_reconnect_attempts) {
            //should_reconnect = true;
            c->reconnect_attempts++;

            /* 指数退避：每次重连延迟翻倍，但不超过最大延迟 */
            if (c->reconnect_attempts > 1) {
                c->current_reconnect_delay_ms *= 2;
                if (c->current_reconnect_delay_ms > c->max_reconnect_delay_ms) {
                    c->current_reconnect_delay_ms = c->max_reconnect_delay_ms;
                }
            }

            VOX_LOG_DEBUG("MQTT client: scheduling reconnect in %u ms (attempt %u)",
                c->current_reconnect_delay_ms, c->reconnect_attempts);

            /* 启动重连定时器（单次触发） */
            if (vox_timer_start(&c->reconnect_timer, c->current_reconnect_delay_ms, 0,
                reconnect_timer_cb, c) != 0) {
                VOX_LOG_ERROR("MQTT client: failed to start reconnect timer");
                //should_reconnect = false;
            }
        } else {
            VOX_LOG_ERROR("MQTT client: max reconnect attempts (%u) reached",
                c->max_reconnect_attempts);
        }
    }

    /* 调用回调（可能销毁 client，之后不再访问 c） */
    if (connect_cb) connect_cb(c, -1, connect_ud);
    if (error_cb) error_cb(c, msg ? msg : "error", error_ud);
    if (disconnect_cb) disconnect_cb(c, disconnect_ud);
}

/** 连接建立失败时释放 pending_connect_buf 与 host，并清除 connecting */
static void connect_cleanup_on_fail(vox_mqtt_client_t* c) {
    if (!c) return;
    if (c->pending_connect_buf) {
        vox_mpool_free(c->mpool, c->pending_connect_buf);
        c->pending_connect_buf = NULL;
    }
    c->pending_connect_len = 0;
    if (c->host) {
        vox_mpool_free(c->mpool, c->host);
        c->host = NULL;
    }
    c->connecting = false;
}

/** 将数据送入 parser，解析失败则 client_fail；用于 TCP/TLS/WS 读回调 */
static void feed_parser_or_fail(vox_mqtt_client_t* c, const void* buf, size_t len) {
    if (!c || !c->parser) return;
    ssize_t used = vox_mqtt_parser_execute(c->parser, (const char*)buf, len);
    if (used < 0) {
        const char* err = vox_mqtt_parser_get_error(c->parser);
        client_fail(c, err ? err : "parse error");
    }
}

/* 返回 0 表示 buf 已入队/已发送（由 send_buf 或 write_done 负责释放）；-1 表示失败，调用方需自行释放 buf */
static int send_buf(vox_mqtt_client_t* c, uint8_t* buf, size_t len);
static void tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data);

static void tcp_connect_cb(vox_tcp_t* tcp, int status, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)tcp;
    if (!c || !c->connecting) return;
    if (status != 0) {
        connect_cleanup_on_fail(c);
        client_fail(c, "tcp connect failed");
        return;
    }
    if (c->pending_connect_buf) {
        if (send_buf(c, c->pending_connect_buf, c->pending_connect_len) != 0)
            vox_mpool_free(c->mpool, c->pending_connect_buf);
        c->pending_connect_buf = NULL;
        c->pending_connect_len = 0;
    }
    if (vox_tcp_read_start(c->tcp, NULL, tcp_read_cb) != 0) {
        connect_cleanup_on_fail(c);
        client_fail(c, "tcp read_start failed");
        return;
    }
}

static void tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)tcp;
    if (!c) return;
    if (nread <= 0) {
        client_fail(c, nread == 0 ? "connection closed" : "read error");
        return;
    }
    feed_parser_or_fail(c, buf, (size_t)nread);
}

#if defined(VOX_USE_SSL) && VOX_USE_SSL
static void tls_read_cb(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)tls;
    if (!c) return;
    if (nread <= 0) {
        client_fail(c, nread == 0 ? "connection closed" : "read error");
        return;
    }
    feed_parser_or_fail(c, buf, (size_t)nread);
}

static void tls_handshake_cb(vox_tls_t* tls, int status, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)tls;
    if (!c || !c->connecting) return;
    if (status != 0) {
        connect_cleanup_on_fail(c);
        client_fail(c, "tls handshake failed");
        return;
    }
    if (c->pending_connect_buf) {
        if (send_buf(c, c->pending_connect_buf, c->pending_connect_len) != 0)
            vox_mpool_free(c->mpool, c->pending_connect_buf);
        c->pending_connect_buf = NULL;
        c->pending_connect_len = 0;
    }
    if (vox_tls_read_start(c->tls, NULL, tls_read_cb) != 0) {
        connect_cleanup_on_fail(c);
        client_fail(c, "tls read_start failed");
        return;
    }
}

static void tls_connect_cb(vox_tls_t* tls, int status, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)tls;
    if (!c || !c->connecting) return;
    if (status != 0) {
        connect_cleanup_on_fail(c);
        client_fail(c, "tls connect failed");
        return;
    }
    if (vox_tls_handshake(c->tls, tls_handshake_cb) != 0) {
        connect_cleanup_on_fail(c);
        client_fail(c, "tls handshake start failed");
    }
}
#endif

#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
static void ws_on_connect(vox_ws_client_t* ws, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)ws;
    if (!c || !c->connecting || !c->pending_connect_buf) return;
    if (send_buf(c, c->pending_connect_buf, c->pending_connect_len) != 0)
        vox_mpool_free(c->mpool, c->pending_connect_buf);
    c->pending_connect_buf = NULL;
    c->pending_connect_len = 0;
}
static void ws_on_message(vox_ws_client_t* ws, const void* data, size_t len, vox_ws_message_type_t type, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)ws;
    if (!c || type != VOX_WS_MSG_BINARY || len == 0) return;
    feed_parser_or_fail(c, data, len);
}
static void ws_on_close(vox_ws_client_t* ws, uint16_t code, const char* reason, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)ws;
    (void)code;
    (void)reason;
    if (c) client_fail(c, "ws closed");
}
static void ws_on_error(vox_ws_client_t* ws, const char* error, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)ws;
    if (c) client_fail(c, error ? error : "ws error");
}
#endif

/** 写完成时释放单条 write_req（TCP/TLS 共用） */
static void write_req_done(vox_mqtt_client_t* c, vox_mqtt_write_req_t* req) {
    if (!c || !req) return;
    if (req->buf) vox_mpool_free(c->mpool, req->buf);
    vox_list_remove(&c->write_queue, &req->node);
    vox_mpool_free(c->mpool, req);
}

/* TCP/TLS 层写完成时传入的是 handle.data（即 client），不是 req；完成顺序与 write_queue 一致，取队首即本次完成的 req */
static void write_done_cb(vox_tcp_t* tcp, int status, void* user_data) {
    (void)status;
    (void)user_data;
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)tcp->handle.data;
    vox_list_node_t* front = vox_list_first(&c->write_queue);
    if (!front) return;
    vox_mqtt_write_req_t* req = VOX_CONTAINING_RECORD(front, vox_mqtt_write_req_t, node);
    write_req_done(c, req);
}

#if defined(VOX_USE_SSL) && VOX_USE_SSL
static void tls_write_done_cb(vox_tls_t* tls, int status, void* user_data) {
    (void)status;
    (void)user_data;
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)tls->handle.data;
    vox_list_node_t* front = vox_list_first(&c->write_queue);
    if (!front) return;
    vox_mqtt_write_req_t* req = VOX_CONTAINING_RECORD(front, vox_mqtt_write_req_t, node);
    write_req_done(c, req);
}
#endif

static int send_buf(vox_mqtt_client_t* c, uint8_t* buf, size_t len) {
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    if (c->ws_client) {
        if (vox_ws_client_send_binary(c->ws_client, buf, len) != 0)
            return -1;
        /* WS 内部会拷贝数据再发，调用方不再持有 buf，此处统一释放 */
        if (buf) vox_mpool_free(c->mpool, buf);
        return 0;
    }
#endif
    vox_mqtt_write_req_t* req = (vox_mqtt_write_req_t*)vox_mpool_alloc(c->mpool, sizeof(vox_mqtt_write_req_t));
    if (!req)
        return -1;
    memset(req, 0, sizeof(*req));
    req->buf = buf;
    req->len = len;
    vox_list_push_back(&c->write_queue, &req->node);
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    if (c->tls) {
        if (vox_tls_write(c->tls, buf, len, tls_write_done_cb) != 0) {
            vox_list_remove(&c->write_queue, &req->node);
            vox_mpool_free(c->mpool, req);
            return -1;
        }
        return 0;
    }
#endif
    if (vox_tcp_write(c->tcp, buf, len, write_done_cb) != 0) {
        vox_list_remove(&c->write_queue, &req->node);
        vox_mpool_free(c->mpool, req);
        return -1;
    }
    return 0;
}

static void ping_timer_cb(vox_timer_t* timer, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)timer;
    if (!c || !c->connected) return;
    size_t need = vox_mqtt_encode_pingreq(NULL, 0);
    uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
    if (!buf) return;
    vox_mqtt_encode_pingreq(buf, need);
    if (send_buf(c, buf, need) != 0)
        vox_mpool_free(c->mpool, buf);
}

/* QoS 重传定时器回调：检查超时的 QoS 1/2 消息并重传 */
static void qos_retry_timer_cb(vox_timer_t* timer, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)timer;
    if (!c || !c->connected) return;

    uint64_t now = vox_loop_now(c->loop);
    vox_list_node_t* pos, *n;

    /* 处理 QoS 1 pending 消息 */
    vox_list_for_each_safe(pos, n, &c->pending_qos1_list) {
        vox_mqtt_pending_qos1_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos1_t, node);

        /* 检查是否超时 */
        if (now - p->send_time >= c->qos_retry_interval_ms) {
            if (p->retry_count >= c->qos_max_retry) {
                /* 超过最大重试次数，放弃并通知用户 */
                VOX_LOG_ERROR("QoS 1 publish timeout, packet_id=%u", p->packet_id);
                if (c->error_cb) {
                    c->error_cb(c, "QoS 1 publish timeout", c->error_user_data);
                }
                /* 移除并释放 */
                vox_list_remove(&c->pending_qos1_list, pos);
                if (p->packet_buf) vox_mpool_free(c->mpool, p->packet_buf);
                vox_mpool_free(c->mpool, p);
            } else {
                /* 重传：设置 DUP 标志（MQTT 规范要求） */
                if (p->packet_buf && p->packet_len > 0) {
                    p->packet_buf[0] |= 0x08;  /* DUP flag */
                    VOX_LOG_DEBUG("Retrying QoS 1 publish, packet_id=%u, retry=%u",
                        p->packet_id, p->retry_count + 1);
                    /* 重新发送（不释放 buf，由 PUBACK 或下次超时释放） */
                    if (send_buf(c, p->packet_buf, p->packet_len) == 0) {
                        p->send_time = now;
                        p->retry_count++;
                    }
                }
            }
        }
    }

    /* 处理 QoS 2 pending 消息 */
    vox_list_for_each_safe(pos, n, &c->pending_qos2_out_list) {
        vox_mqtt_pending_qos2_out_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos2_out_t, node);

        /* 检查是否超时 */
        if (now - p->send_time >= c->qos_retry_interval_ms) {
            if (p->retry_count >= c->qos_max_retry) {
                /* 超过最大重试次数，放弃并通知用户 */
                VOX_LOG_ERROR("QoS 2 publish timeout, packet_id=%u, state=%u", p->packet_id, p->state);
                if (c->error_cb) {
                    c->error_cb(c, "QoS 2 publish timeout", c->error_user_data);
                }
                /* 移除并释放 */
                vox_list_remove(&c->pending_qos2_out_list, pos);
                if (p->packet_buf) vox_mpool_free(c->mpool, p->packet_buf);
                vox_mpool_free(c->mpool, p);
            } else {
                /* 重传 */
                if (p->state == 0) {
                    /* 等待 PUBREC：重传 PUBLISH */
                    if (p->packet_buf && p->packet_len > 0) {
                        p->packet_buf[0] |= 0x08;  /* DUP flag */
                        VOX_LOG_DEBUG("Retrying QoS 2 PUBLISH, packet_id=%u, retry=%u",
                            p->packet_id, p->retry_count + 1);
                        if (send_buf(c, p->packet_buf, p->packet_len) == 0) {
                            p->send_time = now;
                            p->retry_count++;
                        }
                    }
                } else {
                    /* 等待 PUBCOMP：重传 PUBREL */
                    VOX_LOG_DEBUG("Retrying QoS 2 PUBREL, packet_id=%u, retry=%u",
                        p->packet_id, p->retry_count + 1);
                    size_t need = vox_mqtt_encode_pubrel(NULL, 0, p->packet_id);
                    if (need > 0) {
                        uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
                        if (buf) {
                            vox_mqtt_encode_pubrel(buf, need, p->packet_id);
                            if (send_buf(c, buf, need) == 0) {
                                p->send_time = now;
                                p->retry_count++;
                            } else {
                                vox_mpool_free(c->mpool, buf);
                            }
                        }
                    }
                }
            }
        }
    }

    /* 如果两个队列都为空，停止定时器 */
    if (vox_list_empty(&c->pending_qos1_list) && vox_list_empty(&c->pending_qos2_out_list)) {
        vox_timer_stop(&c->qos_retry_timer);
    }
}

static int on_connack(void* user_data, uint8_t session_present, uint8_t return_code) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)session_present;
    if (!c || !c->connecting) return 0;
    c->connecting = false;
    c->connected = (return_code == VOX_MQTT_CONNACK_ACCEPTED);

    /* 保存回调和状态，避免回调中销毁 client 后继续访问 */
    vox_mqtt_connect_cb cb = c->connect_cb;
    void* ud = c->connect_user_data;
    bool connected = c->connected;
    uint16_t keepalive_sec = c->keepalive_sec;

    c->connect_cb = NULL;
    c->connect_user_data = NULL;

    /* 调用连接回调（可能销毁 client） */
    if (cb) {
        cb(c, connected ? 0 : (int)return_code, ud);
    }

    /* 如果连接失败，调用 client_fail（已经处理了 use-after-free） */
    if (!connected) {
        client_fail(c, "connack refused");
        return 0;
    }

    /* 连接成功，启动 ping timer（检查返回值） */
    if (keepalive_sec > 0) {
        uint64_t interval_ms = (uint64_t)keepalive_sec * 500; /* ping at half keepalive */
        if (interval_ms < 1000) interval_ms = 1000;
        if (vox_timer_start(&c->ping_timer, interval_ms, interval_ms, ping_timer_cb, c) != 0) {
            /* Timer 启动失败，记录错误但不断开连接 */
            VOX_LOG_ERROR("MQTT client: failed to start ping timer");
        }
    }

    /* 连接成功，重置重连计数器 */
    c->reconnect_attempts = 0;
    c->current_reconnect_delay_ms = c->initial_reconnect_delay_ms;

    /* 如果会话不存在，自动重新订阅之前的订阅 */
    if (!session_present && !vox_list_empty(&c->subscriptions)) {
        VOX_LOG_DEBUG("MQTT client: auto-resubscribing %zu topics",
            vox_list_size(&c->subscriptions));
        vox_list_node_t* pos;
        vox_list_for_each(pos, &c->subscriptions) {
            vox_mqtt_subscription_t* sub = VOX_CONTAINING_RECORD(pos, vox_mqtt_subscription_t, node);
            /* 重新发送 SUBSCRIBE（不使用回调，静默重新订阅） */
            if (++c->next_packet_id == 0) c->next_packet_id = 1;
            uint16_t packet_id = c->next_packet_id;
            const char* topics[] = { sub->topic_filter };
            size_t lens[] = { sub->topic_filter_len };
            uint8_t qoss[] = { sub->qos };
            size_t need;
            if (c->protocol_version == VOX_MQTT_VERSION_5)
                need = vox_mqtt_encode_subscribe_v5(NULL, 0, packet_id, topics, lens, qoss, 1);
            else
                need = vox_mqtt_encode_subscribe(NULL, 0, packet_id, topics, lens, qoss, 1);
            if (need > 0) {
                uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
                if (buf) {
                    if (c->protocol_version == VOX_MQTT_VERSION_5)
                        vox_mqtt_encode_subscribe_v5(buf, need, packet_id, topics, lens, qoss, 1);
                    else
                        vox_mqtt_encode_subscribe(buf, need, packet_id, topics, lens, qoss, 1);
                    if (send_buf(c, buf, need) != 0) {
                        vox_mpool_free(c->mpool, buf);
                        VOX_LOG_ERROR("MQTT client: failed to resubscribe topic: %.*s",
                            (int)sub->topic_filter_len, sub->topic_filter);
                    }
                }
            }
        }
    }

    return 0;
}

static int on_publish(void* user_data, uint8_t qos, bool retain, uint16_t packet_id,
    const char* topic, size_t topic_len, const void* payload, size_t payload_len) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    if (qos != 2) {
        if (c->message_cb) c->message_cb(c, topic, topic_len, payload, payload_len, qos, retain, c->message_user_data);
        return 0;
    }
    /* QoS 2：先回 PUBREC，等 PUBREL 再投递 */
    vox_mqtt_pending_qos2_in_t* pending = (vox_mqtt_pending_qos2_in_t*)vox_mpool_alloc(c->mpool, sizeof(vox_mqtt_pending_qos2_in_t));
    if (!pending) return -1;
    memset(pending, 0, sizeof(*pending));
    pending->packet_id = packet_id;
    pending->topic_len = topic_len;
    pending->payload_len = payload_len;
    pending->retain = retain;
    pending->topic = (char*)vox_mpool_alloc(c->mpool, topic_len + 1);
    if (!pending->topic) {
        vox_mpool_free(c->mpool, pending);
        return -1;
    }
    memcpy(pending->topic, topic, topic_len);
    pending->topic[topic_len] = '\0';
    if (payload_len > 0) {
        pending->payload = vox_mpool_alloc(c->mpool, payload_len);
        if (!pending->payload) {
            vox_mpool_free(c->mpool, pending->topic);
            vox_mpool_free(c->mpool, pending);
            return -1;
        }
        memcpy(pending->payload, payload, payload_len);
    }
    vox_list_push_back(&c->pending_qos2_in_list, &pending->node);

    size_t need = vox_mqtt_encode_pubrec(NULL, 0, packet_id);
    if (need > 0) {
        uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
        if (!buf) {
            vox_list_remove(&c->pending_qos2_in_list, &pending->node);
            if (pending->topic) vox_mpool_free(c->mpool, pending->topic);
            if (pending->payload) vox_mpool_free(c->mpool, pending->payload);
            vox_mpool_free(c->mpool, pending);
            return -1;
        }
        vox_mqtt_encode_pubrec(buf, need, packet_id);
        if (send_buf(c, buf, need) != 0)
            vox_mpool_free(c->mpool, buf);
    }
    return 0;
}

static int on_puback(void* user_data, uint16_t packet_id) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;

    /* 从 pending QoS 1 队列中移除已确认的消息 */
    vox_list_node_t* pos, *n;
    vox_list_for_each_safe(pos, n, &c->pending_qos1_list) {
        vox_mqtt_pending_qos1_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos1_t, node);
        if (p->packet_id == packet_id) {
            vox_list_remove(&c->pending_qos1_list, pos);
            if (p->packet_buf) vox_mpool_free(c->mpool, p->packet_buf);
            vox_mpool_free(c->mpool, p);
            break;
        }
    }

    /* 如果队列为空，停止重传定时器 */
    if (vox_list_empty(&c->pending_qos1_list)) {
        vox_timer_stop(&c->qos_retry_timer);
    }

    return 0;
}

static int on_pubrec(void* user_data, uint16_t packet_id) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;

    /* 从 pending QoS 2 出站队列中查找并更新状态 */
    vox_list_node_t* pos, *n;
    vox_list_for_each_safe(pos, n, &c->pending_qos2_out_list) {
        vox_mqtt_pending_qos2_out_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos2_out_t, node);
        if (p->packet_id == packet_id && p->state == 0) {
            /* 收到 PUBREC，更新状态为等待 PUBCOMP，发送 PUBREL */
            p->state = 1;
            p->send_time = vox_loop_now(c->loop);
            p->retry_count = 0;  /* 重置重试计数 */

            size_t need = vox_mqtt_encode_pubrel(NULL, 0, packet_id);
            if (need > 0) {
                uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
                if (buf) {
                    vox_mqtt_encode_pubrel(buf, need, packet_id);
                    if (send_buf(c, buf, need) != 0)
                        vox_mpool_free(c->mpool, buf);
                }
            }
            break;
        }
    }
    return 0;
}

static int on_pubrel(void* user_data, uint16_t packet_id) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    vox_list_node_t* pos, * n;
    vox_list_for_each_safe(pos, n, &c->pending_qos2_in_list) {
        vox_mqtt_pending_qos2_in_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos2_in_t, node);
        if (p->packet_id != packet_id) continue;
        vox_list_remove(&c->pending_qos2_in_list, pos);
        size_t need = vox_mqtt_encode_pubcomp(NULL, 0, packet_id);
        if (need > 0) {
            uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
            if (buf) {
                vox_mqtt_encode_pubcomp(buf, need, packet_id);
                if (send_buf(c, buf, need) != 0)
                    vox_mpool_free(c->mpool, buf);
            }
        }
        if (c->message_cb)
            c->message_cb(c, p->topic, p->topic_len, p->payload ? p->payload : "", p->payload_len, 2, p->retain, c->message_user_data);
        if (p->topic) vox_mpool_free(c->mpool, p->topic);
        if (p->payload) vox_mpool_free(c->mpool, p->payload);
        vox_mpool_free(c->mpool, p);
        break;
    }
    return 0;
}

static int on_pubcomp(void* user_data, uint16_t packet_id) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;

    /* 从 pending QoS 2 出站队列中移除已完成的消息 */
    vox_list_node_t* pos, *n;
    vox_list_for_each_safe(pos, n, &c->pending_qos2_out_list) {
        vox_mqtt_pending_qos2_out_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos2_out_t, node);
        if (p->packet_id == packet_id) {
            vox_list_remove(&c->pending_qos2_out_list, pos);
            if (p->packet_buf) vox_mpool_free(c->mpool, p->packet_buf);
            vox_mpool_free(c->mpool, p);
            break;
        }
    }

    /* 如果 QoS 1 和 QoS 2 队列都为空，停止重传定时器 */
    if (vox_list_empty(&c->pending_qos1_list) && vox_list_empty(&c->pending_qos2_out_list)) {
        vox_timer_stop(&c->qos_retry_timer);
    }

    return 0;
}

static int on_suback(void* user_data, uint16_t packet_id, const uint8_t* return_codes, size_t count) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    if (c->pending_suback_cb && c->pending_suback_packet_id == packet_id) {
        vox_mqtt_suback_cb cb = c->pending_suback_cb;
        void* ud = c->pending_suback_user_data;
        c->pending_suback_cb = NULL;
        c->pending_suback_user_data = NULL;
        /* 调用回调（可能销毁 client，之后不再访问 c） */
        cb(c, packet_id, return_codes, count, ud);
    }
    return 0;
}

static int on_error(void* user_data, const char* message) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    client_fail(c, message ? message : "parser error");
    return 0;
}

/* 释放克隆的连接选项 */
static void free_cloned_options(vox_mpool_t* pool, vox_mqtt_connect_options_t* opts) {
    if (!pool || !opts) return;
    if (opts->client_id) vox_mpool_free(pool, (void*)opts->client_id);
    if (opts->username) vox_mpool_free(pool, (void*)opts->username);
    if (opts->password) vox_mpool_free(pool, (void*)opts->password);
    if (opts->will_topic) vox_mpool_free(pool, (void*)opts->will_topic);
    if (opts->will_msg) vox_mpool_free(pool, (void*)opts->will_msg);
    if (opts->ws_path) vox_mpool_free(pool, (void*)opts->ws_path);
    vox_mpool_free(pool, opts);
}

vox_mqtt_client_t* vox_mqtt_client_create(vox_loop_t* loop) {
    if (!loop) return NULL;
    vox_mpool_t* loop_mpool = vox_loop_get_mpool(loop);
    if (!loop_mpool) return NULL;
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)vox_mpool_alloc(loop_mpool, sizeof(vox_mqtt_client_t));
    if (!c) return NULL;
    memset(c, 0, sizeof(vox_mqtt_client_t));
    c->loop = loop;
    c->mpool = loop_mpool;
    vox_list_init(&c->write_queue);
    vox_list_init(&c->pending_qos1_list);
    vox_list_init(&c->pending_qos2_out_list);
    vox_list_init(&c->pending_qos2_in_list);
    vox_list_init(&c->subscriptions);

    /* 设置 QoS 重传默认参数 */
    c->qos_retry_interval_ms = 5000;  /* 5秒 */
    c->qos_max_retry = 3;

    if (vox_timer_init(&c->ping_timer, loop) != 0) {
        vox_mpool_free(loop_mpool, c);
        return NULL;
    }

    if (vox_timer_init(&c->qos_retry_timer, loop) != 0) {
        vox_mpool_free(loop_mpool, c);
        return NULL;
    }

    if (vox_timer_init(&c->reconnect_timer, loop) != 0) {
        vox_mpool_free(loop_mpool, c);
        return NULL;
    }

    /* 设置自动重连默认参数（默认禁用） */
    c->auto_reconnect_enabled = false;
    c->max_reconnect_attempts = 0;  /* 0 表示无限重试 */
    c->initial_reconnect_delay_ms = 1000;  /* 1秒 */
    c->max_reconnect_delay_ms = 60000;  /* 60秒 */
    c->reconnect_attempts = 0;
    c->current_reconnect_delay_ms = 0;
    c->saved_options = NULL;

    c->tcp = vox_tcp_create(loop);
    if (!c->tcp) {
        vox_mpool_free(loop_mpool, c);
        return NULL;
    }
    vox_handle_set_data((vox_handle_t*)c->tcp, c);

    vox_mqtt_parser_config_t pcfg = { 0 };
    vox_mqtt_parser_callbacks_t pcb = { 0 };
    pcb.on_connack = on_connack;
    pcb.on_publish = on_publish;
    pcb.on_puback = on_puback;
    pcb.on_pubrec = on_pubrec;
    pcb.on_pubrel = on_pubrel;
    pcb.on_pubcomp = on_pubcomp;
    pcb.on_suback = on_suback;
    pcb.on_error = on_error;
    pcb.user_data = c;

    c->parser = vox_mqtt_parser_create(loop_mpool, &pcfg, &pcb);
    if (!c->parser) {
        vox_tcp_destroy(c->tcp);
        vox_mpool_free(loop_mpool, c);
        return NULL;
    }
    return c;
}

void vox_mqtt_client_destroy(vox_mqtt_client_t* c) {
    if (!c) return;
    vox_mpool_t* pool = c->mpool;

    /* 如果有 pending 的 deferred close，先执行 disconnect 并运行一次 loop 让 callback 完成 */
    if (c->transport_close_pending && c->loop) {
        /* 运行一次 loop 来处理 pending 的 deferred_close_transport_cb */
        vox_loop_run(c->loop, VOX_RUN_NOWAIT);
    }

    vox_mqtt_client_disconnect(c);
    if (c->pending_connect_buf) {
        vox_mpool_free(pool, c->pending_connect_buf);
        c->pending_connect_buf = NULL;
    }
    if (c->dns_req) {
        vox_dns_getaddrinfo_cancel(c->dns_req);
        vox_dns_getaddrinfo_destroy(c->dns_req);
        c->dns_req = NULL;
    }
    vox_timer_stop(&c->ping_timer);
    vox_timer_stop(&c->qos_retry_timer);
    vox_timer_stop(&c->reconnect_timer);
    if (c->parser) vox_mqtt_parser_destroy(c->parser);
    c->parser = NULL;
    vox_list_node_t* pos, * n;

    /* 清理 pending QoS 1 消息 */
    vox_list_for_each_safe(pos, n, &c->pending_qos1_list) {
        vox_mqtt_pending_qos1_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos1_t, node);
        vox_list_remove(&c->pending_qos1_list, pos);
        if (p->packet_buf) vox_mpool_free(pool, p->packet_buf);
        vox_mpool_free(pool, p);
    }

    /* 清理 pending QoS 2 出站消息 */
    vox_list_for_each_safe(pos, n, &c->pending_qos2_out_list) {
        vox_mqtt_pending_qos2_out_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos2_out_t, node);
        vox_list_remove(&c->pending_qos2_out_list, pos);
        if (p->packet_buf) vox_mpool_free(pool, p->packet_buf);
        vox_mpool_free(pool, p);
    }

    /* 清理 pending QoS 2 入站消息 */
    vox_list_for_each_safe(pos, n, &c->pending_qos2_in_list) {
        vox_mqtt_pending_qos2_in_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos2_in_t, node);
        vox_list_remove(&c->pending_qos2_in_list, pos);
        if (p->topic) vox_mpool_free(pool, p->topic);
        if (p->payload) vox_mpool_free(pool, p->payload);
        vox_mpool_free(pool, p);
    }

    /* 清理订阅列表 */
    vox_list_for_each_safe(pos, n, &c->subscriptions) {
        vox_mqtt_subscription_t* sub = VOX_CONTAINING_RECORD(pos, vox_mqtt_subscription_t, node);
        vox_list_remove(&c->subscriptions, pos);
        if (sub->topic_filter) vox_mpool_free(pool, sub->topic_filter);
        vox_mpool_free(pool, sub);
    }

    /* 清理保存的连接选项（用于自动重连） */
    if (c->saved_options) {
        free_cloned_options(pool, c->saved_options);
        c->saved_options = NULL;
    }

    vox_list_for_each_safe(pos, n, &c->write_queue) {
        vox_mqtt_write_req_t* req = VOX_CONTAINING_RECORD(pos, vox_mqtt_write_req_t, node);
        vox_list_remove(&c->write_queue, pos);
        if (req->buf) vox_mpool_free(pool, req->buf);
        vox_mpool_free(pool, req);
    }
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    if (c->ws_client) {
        vox_ws_client_destroy(c->ws_client);
        c->ws_client = NULL;
    }
#endif
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    if (c->tls) {
        vox_tls_destroy(c->tls);
        c->tls = NULL;
    }
#endif
    if (c->tcp) vox_tcp_destroy(c->tcp);
    c->tcp = NULL;
    if (c->host) vox_mpool_free(pool, c->host);
    vox_mpool_free(pool, c);
}

static void dns_cb(vox_dns_getaddrinfo_t* dns, int status, const vox_dns_addrinfo_t* addrinfo, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)dns;
    if (!c || !c->connecting) return;
    c->dns_req = NULL;
    if (status != 0 || !addrinfo || addrinfo->count == 0) {
        client_fail(c, "dns failed");
        return;
    }
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    if (c->tls) {
        if (vox_tls_connect(c->tls, &addrinfo->addrs[0], tls_connect_cb) != 0) {
            client_fail(c, "tls_connect failed");
            return;
        }
        return;
    }
#endif
    if (vox_tcp_connect(c->tcp, &addrinfo->addrs[0], tcp_connect_cb) != 0) {
        client_fail(c, "tcp_connect failed");
        return;
    }
}

/* 深拷贝连接选项（用于自动重连） */
static vox_mqtt_connect_options_t* clone_connect_options(
    vox_mpool_t* pool, const vox_mqtt_connect_options_t* src) {
    if (!pool || !src) return NULL;

    vox_mqtt_connect_options_t* dst = (vox_mqtt_connect_options_t*)
        vox_mpool_alloc(pool, sizeof(vox_mqtt_connect_options_t));
    if (!dst) return NULL;

    /* 拷贝基本字段 */
    memcpy(dst, src, sizeof(vox_mqtt_connect_options_t));

    /* 深拷贝字符串字段 */
    #define CLONE_STR(field, len_field) \
        if (src->field) { \
            size_t len = src->len_field ? src->len_field : strlen(src->field); \
            char* copy = (char*)vox_mpool_alloc(pool, len + 1); \
            if (!copy) goto cleanup_fail; \
            memcpy(copy, src->field, len); \
            copy[len] = '\0'; \
            dst->field = copy; \
            dst->len_field = len; \
        }

    CLONE_STR(client_id, client_id_len);
    CLONE_STR(username, username_len);
    CLONE_STR(password, password_len);
    CLONE_STR(will_topic, will_topic_len);
    CLONE_STR(ws_path, ws_path_len);

    /* will_msg 是二进制数据 */
    if (src->will_msg && src->will_msg_len > 0) {
        void* copy = vox_mpool_alloc(pool, src->will_msg_len);
        if (!copy) goto cleanup_fail;
        memcpy(copy, src->will_msg, src->will_msg_len);
        dst->will_msg = copy;
    }

    #undef CLONE_STR
    return dst;

cleanup_fail:
    /* 清理已分配的字符串 */
    if (dst->client_id) vox_mpool_free(pool, (void*)dst->client_id);
    if (dst->username) vox_mpool_free(pool, (void*)dst->username);
    if (dst->password) vox_mpool_free(pool, (void*)dst->password);
    if (dst->will_topic) vox_mpool_free(pool, (void*)dst->will_topic);
    if (dst->will_msg) vox_mpool_free(pool, (void*)dst->will_msg);
    if (dst->ws_path) vox_mpool_free(pool, (void*)dst->ws_path);
    vox_mpool_free(pool, dst);
    return NULL;
}

int vox_mqtt_client_connect(vox_mqtt_client_t* c,
    const char* host, uint16_t port,
    const vox_mqtt_connect_options_t* options,
    vox_mqtt_connect_cb cb, void* user_data) {
    if (!c || !options || !options->client_id) return -1;
    if (c->connected || c->connecting) return -1;
    if (!host || !*host) return -1;

    size_t client_id_len = options->client_id_len;
    if (client_id_len == 0) client_id_len = strlen(options->client_id);

    c->connecting = true;
    c->port = port;
    c->connect_cb = cb;
    c->connect_user_data = user_data;
    c->keepalive_sec = options->keepalive > 0 ? options->keepalive : 60;

    /* 保存自动重连配置 */
    c->auto_reconnect_enabled = options->enable_auto_reconnect;
    c->max_reconnect_attempts = options->max_reconnect_attempts;
    c->initial_reconnect_delay_ms = options->initial_reconnect_delay_ms > 0 ?
        options->initial_reconnect_delay_ms : 1000;
    c->max_reconnect_delay_ms = options->max_reconnect_delay_ms > 0 ?
        options->max_reconnect_delay_ms : 60000;
    c->reconnect_attempts = 0;  /* 重置重连计数 */
    c->current_reconnect_delay_ms = c->initial_reconnect_delay_ms;

    /* 保存连接选项副本（用于自动重连） */
    if (c->saved_options) {
        free_cloned_options(c->mpool, c->saved_options);
        c->saved_options = NULL;
    }
    if (c->auto_reconnect_enabled) {
        c->saved_options = clone_connect_options(c->mpool, options);
        if (!c->saved_options) {
            c->connecting = false;
            return -1;
        }
    }

    size_t host_len = strlen(host);
    c->host = (char*)vox_mpool_alloc(c->mpool, host_len + 1);
    if (!c->host) {
        c->connecting = false;
        return -1;
    }
    memcpy(c->host, host, host_len + 1);

    c->protocol_version = options->use_mqtt5 ? VOX_MQTT_VERSION_5 : VOX_MQTT_VERSION_3_1_1;

    size_t need;
    if (options->use_mqtt5) {
        need = vox_mqtt_encode_connect_v5(NULL, 0,
            options->client_id, client_id_len,
            c->keepalive_sec, options->clean_session,
            options->will_topic, options->will_topic_len,
            options->will_msg, options->will_msg_len, options->will_qos, options->will_retain,
            options->username, options->username_len,
            options->password, options->password_len,
            0, 0); /* session_expiry_interval, receive_maximum 为 0 则省略 */
    } else {
        need = vox_mqtt_encode_connect(NULL, 0,
            options->client_id, client_id_len,
            c->keepalive_sec, options->clean_session,
            options->will_topic, options->will_topic_len,
            options->will_msg, options->will_msg_len, options->will_qos, options->will_retain,
            options->username, options->username_len,
            options->password, options->password_len);
    }
    if (need == 0) {
        vox_mpool_free(c->mpool, c->host);
        c->host = NULL;
        c->connecting = false;
        return -1;
    }
    uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
    if (!buf) {
        vox_mpool_free(c->mpool, c->host);
        c->host = NULL;
        c->connecting = false;
        return -1;
    }
    if (options->use_mqtt5) {
        vox_mqtt_encode_connect_v5(buf, need,
            options->client_id, client_id_len,
            c->keepalive_sec, options->clean_session,
            options->will_topic, options->will_topic_len,
            options->will_msg, options->will_msg_len, options->will_qos, options->will_retain,
            options->username, options->username_len,
            options->password, options->password_len,
            0, 0);
    } else {
        vox_mqtt_encode_connect(buf, need,
            options->client_id, client_id_len,
            c->keepalive_sec, options->clean_session,
            options->will_topic, options->will_topic_len,
            options->will_msg, options->will_msg_len, options->will_qos, options->will_retain,
            options->username, options->username_len,
            options->password, options->password_len);
    }

    vox_mqtt_parser_reset(c->parser);
    if (options->use_mqtt5)
        vox_mqtt_parser_set_protocol_version(c->parser, VOX_MQTT_VERSION_5);
    c->pending_connect_buf = buf;
    c->pending_connect_len = need;

#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    if (options->ws_path && options->ws_path[0]) {
        size_t path_len = options->ws_path_len ? options->ws_path_len : strlen(options->ws_path);
#if defined(VOX_USE_SSL) && VOX_USE_SSL
        const char* scheme = (options->ssl_ctx) ? "wss" : "ws";
        bool use_wss = (options->ssl_ctx != NULL);
#else
        const char* scheme = "ws";
        bool use_wss = false;
#endif
        size_t url_cap = 8 + host_len + 8 + path_len + 2;
        char* url = (char*)vox_mpool_alloc(c->mpool, url_cap);
        if (!url) {
            connect_cleanup_on_fail(c);
            return -1;
        }
        snprintf(url, url_cap, "%s://%s:%u%.*s", scheme, host, (unsigned)port, (int)path_len, options->ws_path);
        vox_ws_client_config_t ws_cfg = { 0 };
        ws_cfg.loop = c->loop;
        ws_cfg.url = url;
        ws_cfg.use_ssl = use_wss;
#if defined(VOX_USE_SSL) && VOX_USE_SSL
        ws_cfg.ssl_ctx = options->ssl_ctx;
#else
        ws_cfg.ssl_ctx = NULL;
#endif
        ws_cfg.on_connect = ws_on_connect;
        ws_cfg.on_message = ws_on_message;
        ws_cfg.on_close = ws_on_close;
        ws_cfg.on_error = ws_on_error;
        ws_cfg.user_data = c;
        c->ws_client = vox_ws_client_create(&ws_cfg);
        vox_mpool_free(c->mpool, url);
        if (!c->ws_client) {
            connect_cleanup_on_fail(c);
            return -1;
        }
        if (vox_ws_client_connect(c->ws_client) != 0) {
            vox_ws_client_destroy(c->ws_client);
            c->ws_client = NULL;
            connect_cleanup_on_fail(c);
            return -1;
        }
        return 0;
    }
#endif

#if defined(VOX_USE_SSL) && VOX_USE_SSL
    if (options->ssl_ctx) {
        c->tls = vox_tls_create(c->loop, options->ssl_ctx);
        if (!c->tls) {
            connect_cleanup_on_fail(c);
            return -1;
        }
        vox_handle_set_data((vox_handle_t*)c->tls, c);
        vox_socket_addr_t addr;
        if (vox_socket_parse_address(host, port, &addr) == 0) {
            if (vox_tls_connect(c->tls, &addr, tls_connect_cb) != 0) {
                vox_tls_destroy(c->tls);
                c->tls = NULL;
                connect_cleanup_on_fail(c);
                return -1;
            }
            return 0;
        }
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
        c->dns_req = vox_dns_getaddrinfo_create(c->loop);
        if (!c->dns_req) {
            vox_tls_destroy(c->tls);
            c->tls = NULL;
            connect_cleanup_on_fail(c);
            return -1;
        }
        if (vox_dns_getaddrinfo(c->dns_req, host, port_str, 0, dns_cb, c, 5000) != 0) {
            vox_dns_getaddrinfo_destroy(c->dns_req);
            c->dns_req = NULL;
            vox_tls_destroy(c->tls);
            c->tls = NULL;
            connect_cleanup_on_fail(c);
            return -1;
        }
        return 0;
    }
#endif

    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, port, &addr) == 0) {
        c->tcp->read_cb = tcp_read_cb;
        if (vox_tcp_connect(c->tcp, &addr, tcp_connect_cb) != 0) {
            connect_cleanup_on_fail(c);
            return -1;
        }
        return 0;
    }
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    c->dns_req = vox_dns_getaddrinfo_create(c->loop);
    if (!c->dns_req) {
        connect_cleanup_on_fail(c);
        return -1;
    }
    c->tcp->read_cb = tcp_read_cb;
    if (vox_dns_getaddrinfo(c->dns_req, host, port_str, 0, dns_cb, c, 5000) != 0) {
        vox_dns_getaddrinfo_destroy(c->dns_req);
        c->dns_req = NULL;
        connect_cleanup_on_fail(c);
        return -1;
    }
    return 0;
}

/** 延迟关闭传输：在下次 loop 迭代执行，使 DISCONNECT 的 write_done 有机会先执行。执行后清除 transport_close_pending，tcp 仅 close 不 destroy。 */
static void deferred_close_transport_cb(vox_loop_t* loop, void* user_data) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)loop;
    if (!c) return;
    c->transport_close_pending = false;
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    if (c->tls) {
        vox_handle_close((vox_handle_t*)c->tls, NULL);
        vox_tls_destroy(c->tls);
        c->tls = NULL;
    }
    else
#endif
    if (c->tcp)
        vox_handle_close((vox_handle_t*)c->tcp, NULL);
}

/** 若已连接则编码并发送 DISCONNECT 报文，然后关闭当前传输。WS 路径下 WS 层会复制数据，可立即 free buf；TCP/TLS 经 send_buf 在 write_done 时释放，并延迟关闭传输使 write_done 先执行。 */
static void send_disconnect_then_close_transport(vox_mqtt_client_t* c) {
    if (!c || !c->connected) return;
    size_t need = (c->protocol_version == VOX_MQTT_VERSION_5)
        ? vox_mqtt_encode_disconnect_v5(NULL, 0, VOX_MQTT5_REASON_SUCCESS)
        : vox_mqtt_encode_disconnect(NULL, 0);
    if (need > 0) {
        uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
        if (buf) {
            if (c->protocol_version == VOX_MQTT_VERSION_5)
                vox_mqtt_encode_disconnect_v5(buf, need, VOX_MQTT5_REASON_SUCCESS);
            else
                vox_mqtt_encode_disconnect(buf, need);
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
            if (c->ws_client) {
                /* WS 层 vox_ws_client_send_binary 内部会拷贝数据再写，可立即释放 */
                (void)vox_ws_client_send_binary(c->ws_client, buf, need);
                vox_mpool_free(c->mpool, buf);
            } else
#endif
                if (send_buf(c, buf, need) != 0)
                    vox_mpool_free(c->mpool, buf);
        }
    }
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    if (c->ws_client) {
        vox_ws_client_close(c->ws_client, 1000, NULL);
        vox_ws_client_destroy(c->ws_client);
        c->ws_client = NULL;
    }
    else
#endif
    {
        /* TCP/TLS：延迟关闭，让 DISCONNECT 的 write_done_cb 先执行再关 handle，避免 disconnect 后立即 destroy 时 use-after-free */
        if (c->loop) {
            c->transport_close_pending = true;
            (void)vox_loop_queue_work(c->loop, deferred_close_transport_cb, c);
        } else {
#if defined(VOX_USE_SSL) && VOX_USE_SSL
            if (c->tls) {
                vox_handle_close((vox_handle_t*)c->tls, NULL);
                vox_tls_destroy(c->tls);
                c->tls = NULL;
            }
            else
#endif
            if (c->tcp)
                vox_handle_close((vox_handle_t*)c->tcp, NULL);
        }
    }
}

void vox_mqtt_client_disconnect(vox_mqtt_client_t* c) {
    if (!c) return;
    if (c->dns_req) {
        vox_dns_getaddrinfo_cancel(c->dns_req);
        vox_dns_getaddrinfo_destroy(c->dns_req);
        c->dns_req = NULL;
    }
    vox_timer_stop(&c->ping_timer);
    send_disconnect_then_close_transport(c);
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    if (c->ws_client) {
        vox_ws_client_destroy(c->ws_client);
        c->ws_client = NULL;
    }
#endif
    /* 若已排队延迟关闭，由 deferred_close_transport_cb 负责 destroy tls，此处不重复 destroy */
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    if (!c->transport_close_pending && c->tls) {
        vox_tls_destroy(c->tls);
        c->tls = NULL;
    }
#endif
    c->connected = false;
    c->connecting = false;
}

bool vox_mqtt_client_is_connected(vox_mqtt_client_t* c) {
    return c && c->connected;
}

int vox_mqtt_client_publish(vox_mqtt_client_t* c,
    const char* topic, size_t topic_len,
    const void* payload, size_t payload_len,
    uint8_t qos, bool retain) {
    if (!c || !topic) return -1;
    if (!c->connected) return -1;
    if (topic_len == 0) topic_len = strlen(topic);
    if (qos > 2) return -1;
    /* QoS 2 现在支持多路并发，移除单路限制 */

    uint16_t packet_id = 0;
    if (qos > 0) {
        if (++c->next_packet_id == 0) c->next_packet_id = 1;  /* MQTT packet_id 不能为 0 */
        packet_id = c->next_packet_id;
    }

    size_t need;
    if (c->protocol_version == VOX_MQTT_VERSION_5)
        need = vox_mqtt_encode_publish_v5(NULL, 0, qos, retain, packet_id, topic, topic_len, payload, payload_len);
    else
        need = vox_mqtt_encode_publish(NULL, 0, qos, retain, packet_id, topic, topic_len, payload, payload_len);
    if (need == 0) return -1;
    uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
    if (!buf) return -1;
    if (c->protocol_version == VOX_MQTT_VERSION_5)
        vox_mqtt_encode_publish_v5(buf, need, qos, retain, packet_id, topic, topic_len, payload, payload_len);
    else
        vox_mqtt_encode_publish(buf, need, qos, retain, packet_id, topic, topic_len, payload, payload_len);

    /* QoS 1: 保存到 pending 队列用于重传 */
    if (qos == 1) {
        vox_mqtt_pending_qos1_t* pending = (vox_mqtt_pending_qos1_t*)vox_mpool_alloc(c->mpool, sizeof(vox_mqtt_pending_qos1_t));
        if (!pending) {
            vox_mpool_free(c->mpool, buf);
            return -1;
        }
        memset(pending, 0, sizeof(*pending));
        pending->packet_id = packet_id;
        pending->packet_buf = buf;  /* 保存报文用于重传 */
        pending->packet_len = need;
        pending->send_time = vox_loop_now(c->loop);
        pending->retry_count = 0;
        vox_list_push_back(&c->pending_qos1_list, &pending->node);

        /* 启动重传定时器（如果还没启动） */
        if (!vox_timer_is_active(&c->qos_retry_timer)) {
            vox_timer_start(&c->qos_retry_timer, c->qos_retry_interval_ms,
                c->qos_retry_interval_ms, qos_retry_timer_cb, c);
        }
    }

    /* QoS 2: 保存到 pending 队列用于重传 */
    if (qos == 2) {
        vox_mqtt_pending_qos2_out_t* pending = (vox_mqtt_pending_qos2_out_t*)vox_mpool_alloc(c->mpool, sizeof(vox_mqtt_pending_qos2_out_t));
        if (!pending) {
            vox_mpool_free(c->mpool, buf);
            return -1;
        }
        memset(pending, 0, sizeof(*pending));
        pending->packet_id = packet_id;
        pending->packet_buf = buf;  /* 保存报文用于重传 */
        pending->packet_len = need;
        pending->state = 0;  /* 0=等 PUBREC */
        pending->send_time = vox_loop_now(c->loop);
        pending->retry_count = 0;
        vox_list_push_back(&c->pending_qos2_out_list, &pending->node);

        /* 启动重传定时器（如果还没启动） */
        if (!vox_timer_is_active(&c->qos_retry_timer)) {
            vox_timer_start(&c->qos_retry_timer, c->qos_retry_interval_ms,
                c->qos_retry_interval_ms, qos_retry_timer_cb, c);
        }
    }

    if (send_buf(c, buf, need) != 0) {
        /* 发送失败 */
        if (qos == 1) {
            /* QoS 1: 从 pending 队列移除 */
            vox_list_node_t* pos, *n;
            vox_list_for_each_safe(pos, n, &c->pending_qos1_list) {
                vox_mqtt_pending_qos1_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos1_t, node);
                if (p->packet_id == packet_id) {
                    vox_list_remove(&c->pending_qos1_list, pos);
                    vox_mpool_free(c->mpool, p);
                    break;
                }
            }
        } else if (qos == 2) {
            /* QoS 2: 从 pending 队列移除 */
            vox_list_node_t* pos, *n;
            vox_list_for_each_safe(pos, n, &c->pending_qos2_out_list) {
                vox_mqtt_pending_qos2_out_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos2_out_t, node);
                if (p->packet_id == packet_id) {
                    vox_list_remove(&c->pending_qos2_out_list, pos);
                    vox_mpool_free(c->mpool, p);
                    break;
                }
            }
        }
        vox_mpool_free(c->mpool, buf);
        return -1;
    }
    return 0;
}

int vox_mqtt_client_subscribe(vox_mqtt_client_t* c,
    const char* topic_filter, size_t topic_filter_len,
    uint8_t qos,
    vox_mqtt_suback_cb on_suback, void* user_data) {
    if (!c || !topic_filter) return -1;
    if (!c->connected) return -1;
    if (topic_filter_len == 0) topic_filter_len = strlen(topic_filter);
    if (c->pending_suback_cb) return -1;  /* 仅支持一个 in-flight subscribe */

    if (++c->next_packet_id == 0) c->next_packet_id = 1;
    uint16_t packet_id = c->next_packet_id;
    const char* topics[] = { topic_filter };
    size_t lens[] = { topic_filter_len };
    uint8_t qos_list[] = { qos > 2 ? 2 : qos };
    size_t need;
    if (c->protocol_version == VOX_MQTT_VERSION_5)
        need = vox_mqtt_encode_subscribe_v5(NULL, 0, packet_id, topics, lens, qos_list, 1);
    else
        need = vox_mqtt_encode_subscribe(NULL, 0, packet_id, topics, lens, qos_list, 1);
    if (need == 0) return -1;
    uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
    if (!buf) return -1;
    if (c->protocol_version == VOX_MQTT_VERSION_5)
        vox_mqtt_encode_subscribe_v5(buf, need, packet_id, topics, lens, qos_list, 1);
    else
        vox_mqtt_encode_subscribe(buf, need, packet_id, topics, lens, qos_list, 1);
    c->pending_suback_cb = on_suback;
    c->pending_suback_user_data = user_data;
    c->pending_suback_packet_id = packet_id;
    if (send_buf(c, buf, need) != 0) {
        c->pending_suback_cb = NULL;
        c->pending_suback_user_data = NULL;
        vox_mpool_free(c->mpool, buf);
        return -1;
    }

    /* 添加到订阅列表（乐观添加，假设会成功） */
    vox_mqtt_subscription_t* sub = (vox_mqtt_subscription_t*)vox_mpool_alloc(c->mpool, sizeof(vox_mqtt_subscription_t));
    if (sub) {
        memset(sub, 0, sizeof(*sub));
        sub->topic_filter = (char*)vox_mpool_alloc(c->mpool, topic_filter_len + 1);
        if (sub->topic_filter) {
            memcpy(sub->topic_filter, topic_filter, topic_filter_len);
            sub->topic_filter[topic_filter_len] = '\0';
            sub->topic_filter_len = topic_filter_len;
            sub->qos = qos > 2 ? 2 : qos;
            vox_list_push_back(&c->subscriptions, &sub->node);
        } else {
            vox_mpool_free(c->mpool, sub);
        }
    }

    return 0;
}

int vox_mqtt_client_unsubscribe(vox_mqtt_client_t* c,
    const char* topic_filter, size_t topic_filter_len) {
    if (!c || !topic_filter) return -1;
    if (!c->connected) return -1;
    if (topic_filter_len == 0) topic_filter_len = strlen(topic_filter);

    if (++c->next_packet_id == 0) c->next_packet_id = 1;
    uint16_t packet_id = c->next_packet_id;
    const char* topics[] = { topic_filter };
    size_t lens[] = { topic_filter_len };
    size_t need;
    if (c->protocol_version == VOX_MQTT_VERSION_5)
        need = vox_mqtt_encode_unsubscribe_v5(NULL, 0, packet_id, topics, lens, 1);
    else
        need = vox_mqtt_encode_unsubscribe(NULL, 0, packet_id, topics, lens, 1);
    if (need == 0) return -1;
    uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
    if (!buf) return -1;
    if (c->protocol_version == VOX_MQTT_VERSION_5)
        vox_mqtt_encode_unsubscribe_v5(buf, need, packet_id, topics, lens, 1);
    else
        vox_mqtt_encode_unsubscribe(buf, need, packet_id, topics, lens, 1);
    if (send_buf(c, buf, need) != 0) {
        vox_mpool_free(c->mpool, buf);
        return -1;
    }

    /* 从订阅列表中移除 */
    vox_list_node_t* pos, *n;
    vox_list_for_each_safe(pos, n, &c->subscriptions) {
        vox_mqtt_subscription_t* sub = VOX_CONTAINING_RECORD(pos, vox_mqtt_subscription_t, node);
        if (sub->topic_filter_len == topic_filter_len &&
            memcmp(sub->topic_filter, topic_filter, topic_filter_len) == 0) {
            vox_list_remove(&c->subscriptions, pos);
            if (sub->topic_filter) vox_mpool_free(c->mpool, sub->topic_filter);
            vox_mpool_free(c->mpool, sub);
            break;
        }
    }

    return 0;
}

void vox_mqtt_client_foreach_subscription(vox_mqtt_client_t* c,
    vox_mqtt_subscription_cb cb, void* user_data) {
    if (!c || !cb) return;

    vox_list_node_t* pos;
    vox_list_for_each(pos, &c->subscriptions) {
        vox_mqtt_subscription_t* sub = VOX_CONTAINING_RECORD(pos, vox_mqtt_subscription_t, node);
        cb(sub->topic_filter, sub->topic_filter_len, sub->qos, user_data);
    }
}

void vox_mqtt_client_set_message_cb(vox_mqtt_client_t* c, vox_mqtt_message_cb cb, void* user_data) {
    if (c) { c->message_cb = cb; c->message_user_data = user_data; }
}
void vox_mqtt_client_set_disconnect_cb(vox_mqtt_client_t* c, vox_mqtt_disconnect_cb cb, void* user_data) {
    if (c) { c->disconnect_cb = cb; c->disconnect_user_data = user_data; }
}
void vox_mqtt_client_set_error_cb(vox_mqtt_client_t* c, vox_mqtt_error_cb cb, void* user_data) {
    if (c) { c->error_cb = cb; c->error_user_data = user_data; }
}
