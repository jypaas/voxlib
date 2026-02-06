/*
 * vox_mqtt_server.c - MQTT 服务端实现（TCP / TLS / WS / WSS）
 */

#include "vox_mqtt_server.h"
#include "../vox_handle.h"
#include "../vox_list.h"
#include "../vox_log.h"
#include <string.h>
#if defined(VOX_USE_SSL) && VOX_USE_SSL
#include "../vox_tls.h"
#endif
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
#include "../websocket/vox_websocket_server.h"
#endif

#ifndef VOX_CONTAINING_RECORD
#define VOX_CONTAINING_RECORD(ptr, type, member) vox_container_of(ptr, type, member)
#endif

/* 单条订阅 */
typedef struct vox_mqtt_sub {
    vox_list_node_t node;
    char* filter;
    size_t len;
    uint8_t qos;
} vox_mqtt_sub_t;

/* 待释放的写缓冲 */
typedef struct vox_mqtt_pending_write {
    vox_list_node_t node;
    uint8_t* buf;
} vox_mqtt_pending_write_t;

/* QoS 2 入站：客户端发来 PUBLISH qos2，已回 PUBREC，等 PUBREL 后投递 */
typedef struct vox_mqtt_srv_pending_qos2_in {
    vox_list_node_t node;
    uint16_t packet_id;
    char* topic;
    size_t topic_len;
    void* payload;
    size_t payload_len;
    uint8_t qos;
    bool retain;
} vox_mqtt_srv_pending_qos2_in_t;

/* QoS 2 出站：向该连接发了 PUBLISH qos2，等 PUBREC/PUBCOMP */
typedef struct vox_mqtt_srv_pending_qos2_out {
    vox_list_node_t node;
    uint16_t packet_id;
    uint8_t state; /* 0=等 PUBREC，1=等 PUBCOMP */
} vox_mqtt_srv_pending_qos2_out_t;

struct vox_mqtt_connection {
    vox_mqtt_server_t* server;
    vox_tcp_t* tcp;
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    vox_tls_t* tls;
#endif
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    vox_ws_connection_t* ws_conn;
#endif
    vox_mqtt_parser_t* parser;
    vox_list_node_t node;
    vox_list_t subscriptions;
    vox_list_t pending_writes;
    char* client_id;
    size_t client_id_len;
    uint8_t protocol_version;    /* 协商的协议版本 3/4/5 */
    uint32_t session_expiry_interval; /* MQTT 5 CONNECT 属性，用于 CONNACK 回显或默认 */
    uint16_t receive_maximum;    /* MQTT 5 CONNECT 属性，用于 CONNACK 回显或默认 */
    uint16_t next_packet_id;
    vox_list_t pending_qos2_in_list;   /* 来自客户端的 QoS 2，已回 PUBREC，待 PUBREL */
    vox_list_t pending_qos2_out_list;  /* 发往该客户端的 QoS 2，待 PUBREC/PUBCOMP */
    void* user_data;
};

struct vox_mqtt_server {
    vox_loop_t* loop;
    vox_mpool_t* mpool;
    bool owns_mpool;
    vox_tcp_t* tcp_listener;
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    vox_tls_t* tls_listener;
#endif
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    vox_ws_server_t* ws_server;
    char* ws_path;
#endif
    vox_mqtt_server_config_t config;
    vox_list_t connections;
};

/* MQTT topic 匹配：filter 可含 +（单层）和 #（多层级）；快速路径减少高并发下开销 */
static bool topic_match(const char* filter, size_t filter_len, const char* topic, size_t topic_len) {
    /* 快速路径：仅 "#" 匹配任意 topic */
    if (filter_len == 1 && filter[0] == '#') return true;
    /* 快速路径：无通配符时精确匹配 */
    const char* f = filter;
    const char* fend = filter + filter_len;
    const char* t = topic;
    const char* tend = topic + topic_len;
    for (; f < fend && t < tend; f++, t++) {
        if (*f == '#' || *f == '+') goto wildcard_path;
        if (*f != *t) return false;
    }
    if (f == fend && t == tend) return true;
    if (f < fend && *f == '#') return true;
    return false;
wildcard_path:
    while (f < fend && t <= tend) {
        if (f < fend && *f == '#') return true;
        if (f < fend && *f == '+') {
            while (t < tend && *t != '/') t++;
            if (t < tend) t++;
            f++;
            while (f < fend && *f != '/') f++;
            if (f < fend) f++;
            continue;
        }
        if (t >= tend) return (f == fend);
        if (*f != *t) return false;
        f++;
        t++;
    }
    return (f == fend && t == tend);
}

static void tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data);

static void conn_close(vox_mqtt_connection_t* conn) {
    if (!conn) return;
    vox_mqtt_server_t* s = conn->server;
    vox_list_remove(&s->connections, &conn->node);
    if (s->config.on_disconnect) s->config.on_disconnect(conn, s->config.user_data);
    vox_list_node_t* pos, * n;
    vox_list_for_each_safe(pos, n, &conn->pending_writes) {
        vox_mqtt_pending_write_t* pw = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_write_t, node);
        vox_list_remove(&conn->pending_writes, pos);
        if (pw->buf) vox_mpool_free(s->mpool, pw->buf);
        vox_mpool_free(s->mpool, pw);
    }
    vox_list_for_each_safe(pos, n, &conn->subscriptions) {
        vox_mqtt_sub_t* sub = VOX_CONTAINING_RECORD(pos, vox_mqtt_sub_t, node);
        vox_list_remove(&conn->subscriptions, pos);
        if (sub->filter) vox_mpool_free(s->mpool, sub->filter);
        vox_mpool_free(s->mpool, sub);
    }
    if (conn->client_id) vox_mpool_free(s->mpool, conn->client_id);
    if (conn->parser) vox_mqtt_parser_destroy(conn->parser);
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    if (conn->ws_conn) {
        vox_ws_connection_close(conn->ws_conn, 1000, NULL);
        conn->ws_conn = NULL;
    }
#endif
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    if (conn->tls) {
        vox_tls_destroy(conn->tls);
        conn->tls = NULL;
    }
#endif
    if (conn->tcp) {
        vox_tcp_destroy(conn->tcp);
        conn->tcp = NULL;
    }
    /* 释放 QoS 2 挂起项 */
    {
        vox_list_node_t* pos2, * n2;
        vox_list_for_each_safe(pos2, n2, &conn->pending_qos2_in_list) {
            vox_mqtt_srv_pending_qos2_in_t* p = VOX_CONTAINING_RECORD(pos2, vox_mqtt_srv_pending_qos2_in_t, node);
            vox_list_remove(&conn->pending_qos2_in_list, pos2);
            if (p->topic) vox_mpool_free(s->mpool, p->topic);
            if (p->payload) vox_mpool_free(s->mpool, p->payload);
            vox_mpool_free(s->mpool, p);
        }
        vox_list_for_each_safe(pos2, n2, &conn->pending_qos2_out_list) {
            vox_mqtt_srv_pending_qos2_out_t* p = VOX_CONTAINING_RECORD(pos2, vox_mqtt_srv_pending_qos2_out_t, node);
            vox_list_remove(&conn->pending_qos2_out_list, pos2);
            vox_mpool_free(s->mpool, p);
        }
    }
    vox_mpool_free(s->mpool, conn);
}

/** 从连接上弹出并释放首条 pending write（TCP/TLS write 完成回调共用） */
static void conn_pending_write_pop(vox_mqtt_connection_t* conn) {
    if (!conn || vox_list_empty(&conn->pending_writes)) return;
    vox_mqtt_pending_write_t* pw = VOX_CONTAINING_RECORD(vox_list_first(&conn->pending_writes), vox_mqtt_pending_write_t, node);
    vox_list_remove(&conn->pending_writes, &pw->node);
    if (pw->buf) vox_mpool_free(conn->server->mpool, pw->buf);
    vox_mpool_free(conn->server->mpool, pw);
}

static void conn_write_done(vox_tcp_t* tcp, int status, void* user_data) {
    (void)status;
    (void)user_data;
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)tcp->handle.data;
    conn_pending_write_pop(conn);
}

#if defined(VOX_USE_SSL) && VOX_USE_SSL
static void conn_tls_write_done(vox_tls_t* tls, int status, void* user_data) {
    (void)status;
    (void)user_data;
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)tls->handle.data;
    conn_pending_write_pop(conn);
}
#endif

/** 将数据送入 parser，解析失败返回 true（调用方应 conn_close） */
static bool conn_feed_parser(vox_mqtt_connection_t* conn, const void* buf, size_t len) {
    if (!conn || !conn->parser) return true;
    ssize_t used = vox_mqtt_parser_execute(conn->parser, (const char*)buf, len);
    return (used < 0);
}

static void conn_send(vox_mqtt_connection_t* conn, uint8_t* buf, size_t len) {
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    if (conn->ws_conn) {
        if (vox_ws_connection_send_binary(conn->ws_conn, buf, len) != 0) { }
        vox_mpool_free(conn->server->mpool, buf);
        return;
    }
#endif
    vox_mqtt_pending_write_t* pw = (vox_mqtt_pending_write_t*)vox_mpool_alloc(conn->server->mpool, sizeof(vox_mqtt_pending_write_t));
    if (!pw) {
        if (buf) vox_mpool_free(conn->server->mpool, buf);
        return;
    }
    pw->buf = buf;
    vox_list_push_back(&conn->pending_writes, &pw->node);
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    if (conn->tls) {
        if (vox_tls_write(conn->tls, buf, len, conn_tls_write_done) != 0) {
            vox_list_remove(&conn->pending_writes, &pw->node);
            if (buf) vox_mpool_free(conn->server->mpool, buf);
            vox_mpool_free(conn->server->mpool, pw);
        }
        return;
    }
#endif
    if (vox_tcp_write(conn->tcp, buf, len, conn_write_done) != 0) {
        vox_list_remove(&conn->pending_writes, &pw->node);
        if (buf) vox_mpool_free(conn->server->mpool, buf);
        vox_mpool_free(conn->server->mpool, pw);
    }
}

static int on_connect(void* user_data, const char* client_id, size_t client_id_len,
    uint8_t protocol_version,
    uint16_t keepalive, uint8_t flags,
    const char* will_topic, size_t will_topic_len,
    const void* will_msg, size_t will_msg_len,
    const char* username, size_t username_len,
    const char* password, size_t password_len,
    uint32_t session_expiry_interval, uint16_t receive_maximum) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)user_data;
    vox_mqtt_server_t* s = conn->server;
    (void)keepalive;
    (void)flags;
    (void)will_topic;
    (void)will_topic_len;
    (void)will_msg;
    (void)will_msg_len;
    (void)username;
    (void)username_len;
    (void)password;
    (void)password_len;

    conn->session_expiry_interval = session_expiry_interval;
    conn->receive_maximum = receive_maximum;

    /* Broker 版本层：仅接受配置中的协议版本，屏蔽不兼容版本 */
    if (s->config.accepted_versions != 0) {
        unsigned int mask = 1u << (protocol_version & VOX_MQTT_VERSION_NIBBLE_MASK);
        if ((s->config.accepted_versions & mask) == 0) {
            size_t need;
            if (protocol_version == VOX_MQTT_VERSION_5) {
                need = vox_mqtt_encode_connack_v5(NULL, 0, 0, VOX_MQTT5_REASON_REFUSED_PROTOCOL, 0, 0);
            } else {
                need = vox_mqtt_encode_connack(NULL, 0, 0, VOX_MQTT_CONNACK_REFUSED_PROTOCOL);
            }
            uint8_t* buf = need > 0 ? (uint8_t*)vox_mpool_alloc(s->mpool, need) : NULL;
            if (buf) {
                if (protocol_version == VOX_MQTT_VERSION_5) {
                    vox_mqtt_encode_connack_v5(buf, need, 0, VOX_MQTT5_REASON_REFUSED_PROTOCOL, 0, 0);
                } else {
                    vox_mqtt_encode_connack(buf, need, 0, VOX_MQTT_CONNACK_REFUSED_PROTOCOL);
                }
                conn_send(conn, buf, need);
            }
            return -1;  /* 触发 on_error/conn_close */
        }
    }

    conn->protocol_version = protocol_version;

    if (client_id_len > 0) {
        conn->client_id = (char*)vox_mpool_alloc(s->mpool, client_id_len + 1);
        if (conn->client_id) {
            memcpy(conn->client_id, client_id, client_id_len);
            conn->client_id[client_id_len] = '\0';
            conn->client_id_len = client_id_len;
        }
    }

    /* 按协商版本回包：v5 用 CONNACK v5（回显 Session Expiry / Receive Maximum 或默认），否则 3.1.1 */
    size_t need;
    if (conn->protocol_version == VOX_MQTT_VERSION_5) {
        uint16_t rm = (conn->receive_maximum != 0) ? conn->receive_maximum : 65535;
        need = vox_mqtt_encode_connack_v5(NULL, 0, 0, VOX_MQTT5_REASON_SUCCESS, conn->session_expiry_interval, rm);
    } else {
        need = vox_mqtt_encode_connack(NULL, 0, 0, VOX_MQTT_CONNACK_ACCEPTED);
    }
    uint8_t* buf = (uint8_t*)vox_mpool_alloc(s->mpool, need);
    if (!buf) return -1;
    if (conn->protocol_version == VOX_MQTT_VERSION_5) {
        uint16_t rm = (conn->receive_maximum != 0) ? conn->receive_maximum : 65535;
        vox_mqtt_encode_connack_v5(buf, need, 0, VOX_MQTT5_REASON_SUCCESS, conn->session_expiry_interval, rm);
    } else {
        vox_mqtt_encode_connack(buf, need, 0, VOX_MQTT_CONNACK_ACCEPTED);
    }
    conn_send(conn, buf, need);

    if (s->config.on_connect) s->config.on_connect(conn, conn->client_id ? conn->client_id : "", conn->client_id_len, s->config.user_data);
    return 0;
}

static int on_subscribe(void* user_data, uint16_t packet_id, const char* topic_filter, size_t topic_len, uint8_t qos) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)user_data;
    vox_mqtt_server_t* s = conn->server;
    (void)packet_id;
    vox_mqtt_sub_t* sub = (vox_mqtt_sub_t*)vox_mpool_alloc(s->mpool, sizeof(vox_mqtt_sub_t));
    if (!sub) return -1;
    memset(sub, 0, sizeof(*sub));
    sub->filter = (char*)vox_mpool_alloc(s->mpool, topic_len + 1);
    if (!sub->filter) {
        vox_mpool_free(s->mpool, sub);
        return -1;
    }
    memcpy(sub->filter, topic_filter, topic_len);
    sub->filter[topic_len] = '\0';
    sub->len = topic_len;
    sub->qos = qos > 1 ? 1 : qos;
    vox_list_push_back(&conn->subscriptions, &sub->node);
    return (int)sub->qos;  /* 授予的 qos */
}

static int on_subscribe_done(void* user_data, uint16_t packet_id, const uint8_t* return_codes, size_t count) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)user_data;
    vox_mqtt_server_t* s = conn->server;
    size_t need = (conn->protocol_version == VOX_MQTT_VERSION_5)
        ? vox_mqtt_encode_suback_v5(NULL, 0, packet_id, return_codes, count)
        : vox_mqtt_encode_suback(NULL, 0, packet_id, return_codes, count);
    if (need == 0) return 0;
    uint8_t* buf = (uint8_t*)vox_mpool_alloc(s->mpool, need);
    if (!buf) return -1;
    if (conn->protocol_version == VOX_MQTT_VERSION_5) {
        vox_mqtt_encode_suback_v5(buf, need, packet_id, return_codes, count);
    } else {
        vox_mqtt_encode_suback(buf, need, packet_id, return_codes, count);
    }
    conn_send(conn, buf, need);
    return 0;
}

static int on_unsubscribe(void* user_data, uint16_t packet_id, const char* topic_filter, size_t topic_len) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)user_data;
    vox_mqtt_server_t* s = conn->server;
    vox_list_node_t* pos, * n;
    vox_list_for_each_safe(pos, n, &conn->subscriptions) {
        vox_mqtt_sub_t* sub = VOX_CONTAINING_RECORD(pos, vox_mqtt_sub_t, node);
        if (sub->len == topic_len && memcmp(sub->filter, topic_filter, topic_len) == 0) {
            vox_list_remove(&conn->subscriptions, pos);
            vox_mpool_free(s->mpool, sub->filter);
            vox_mpool_free(s->mpool, sub);
            break;
        }
    }
    size_t need;
    if (conn->protocol_version == VOX_MQTT_VERSION_5) {
        uint8_t reason0 = VOX_MQTT5_REASON_SUCCESS;
        need = vox_mqtt_encode_unsuback_v5(NULL, 0, packet_id, &reason0, 1);
    } else {
        need = vox_mqtt_encode_unsuback(NULL, 0, packet_id);
    }
    uint8_t* buf = (uint8_t*)vox_mpool_alloc(s->mpool, need);
    if (!buf) return -1;
    if (conn->protocol_version == VOX_MQTT_VERSION_5) {
        uint8_t reason0 = VOX_MQTT5_REASON_SUCCESS;
        vox_mqtt_encode_unsuback_v5(buf, need, packet_id, &reason0, 1);
    } else {
        vox_mqtt_encode_unsuback(buf, need, packet_id);
    }
    conn_send(conn, buf, need);
    return 0;
}

/** 将消息投递到 on_publish 并转发给匹配的订阅者（含 QoS 2 出站跟踪） */
static void forward_message_to_subscribers(vox_mqtt_server_t* s, vox_mqtt_connection_t* from_conn,
    const char* topic, size_t topic_len, const void* payload, size_t payload_len, uint8_t qos, bool retain) {
    if (s->config.on_publish) s->config.on_publish(from_conn, topic, topic_len, payload, payload_len, qos, s->config.user_data);

    size_t need_qos0 = vox_mqtt_encode_publish(NULL, 0, 0, retain, 0, topic, topic_len, payload, payload_len);
    size_t need_qos1 = vox_mqtt_encode_publish(NULL, 0, 1, retain, 0, topic, topic_len, payload, payload_len);
    size_t need_qos2 = vox_mqtt_encode_publish(NULL, 0, 2, retain, 0, topic, topic_len, payload, payload_len);
    size_t need_qos0_v5 = vox_mqtt_encode_publish_v5(NULL, 0, 0, retain, 0, topic, topic_len, payload, payload_len);
    size_t need_qos1_v5 = vox_mqtt_encode_publish_v5(NULL, 0, 1, retain, 0, topic, topic_len, payload, payload_len);
    size_t need_qos2_v5 = vox_mqtt_encode_publish_v5(NULL, 0, 2, retain, 0, topic, topic_len, payload, payload_len);

    vox_list_node_t* pos;
    vox_list_for_each(pos, &s->connections) {
        vox_mqtt_connection_t* c = VOX_CONTAINING_RECORD(pos, vox_mqtt_connection_t, node);
        vox_list_node_t* sp;
        vox_list_for_each(sp, &c->subscriptions) {
            vox_mqtt_sub_t* sub = VOX_CONTAINING_RECORD(sp, vox_mqtt_sub_t, node);
            if (topic_match(sub->filter, sub->len, topic, topic_len)) {
                uint8_t grant_qos = sub->qos < qos ? sub->qos : qos;
                int use_v5 = (c->protocol_version == VOX_MQTT_VERSION_5);
                size_t need;
                if (grant_qos == 0) need = use_v5 ? need_qos0_v5 : need_qos0;
                else if (grant_qos == 1) need = use_v5 ? need_qos1_v5 : need_qos1;
                else need = use_v5 ? need_qos2_v5 : need_qos2;
                if (need == 0) break;
                uint8_t* buf = (uint8_t*)vox_mpool_alloc(s->mpool, need);
                if (!buf) break;
                uint16_t pid = 0;
                if (grant_qos > 0) {
                    if (++c->next_packet_id == 0) c->next_packet_id = 1;
                    pid = c->next_packet_id;
                }
                if (use_v5) {
                    vox_mqtt_encode_publish_v5(buf, need, grant_qos, retain, pid, topic, topic_len, payload, payload_len);
                } else {
                    vox_mqtt_encode_publish(buf, need, grant_qos, retain, pid, topic, topic_len, payload, payload_len);
                }
                conn_send(c, buf, need);
                if (grant_qos == 2) {
                    vox_mqtt_srv_pending_qos2_out_t* out = (vox_mqtt_srv_pending_qos2_out_t*)vox_mpool_alloc(s->mpool, sizeof(vox_mqtt_srv_pending_qos2_out_t));
                    if (out) {
                        memset(out, 0, sizeof(*out));
                        out->packet_id = pid;
                        out->state = 0;
                        vox_list_push_back(&c->pending_qos2_out_list, &out->node);
                    }
                }
                break;
            }
        }
    }
}

static int on_publish_from_client(void* user_data, uint8_t qos, bool retain, uint16_t packet_id,
    const char* topic, size_t topic_len, const void* payload, size_t payload_len) {
    vox_mqtt_connection_t* from = (vox_mqtt_connection_t*)user_data;
    vox_mqtt_server_t* s = from->server;

    if (qos == 2) {
        /* QoS 2：先回 PUBREC，等 PUBREL 后再投递 */
        vox_mqtt_srv_pending_qos2_in_t* pending = (vox_mqtt_srv_pending_qos2_in_t*)vox_mpool_alloc(s->mpool, sizeof(vox_mqtt_srv_pending_qos2_in_t));
        if (!pending) return -1;
        memset(pending, 0, sizeof(*pending));
        pending->packet_id = packet_id;
        pending->topic_len = topic_len;
        pending->payload_len = payload_len;
        pending->qos = 2;
        pending->retain = retain;
        pending->topic = (char*)vox_mpool_alloc(s->mpool, topic_len + 1);
        if (!pending->topic) {
            vox_mpool_free(s->mpool, pending);
            return -1;
        }
        memcpy(pending->topic, topic, topic_len);
        pending->topic[topic_len] = '\0';
        if (payload_len > 0) {
            pending->payload = vox_mpool_alloc(s->mpool, payload_len);
            if (!pending->payload) {
                vox_mpool_free(s->mpool, pending->topic);
                vox_mpool_free(s->mpool, pending);
                return -1;
            }
            memcpy(pending->payload, payload, payload_len);
        }
        vox_list_push_back(&from->pending_qos2_in_list, &pending->node);

        size_t need = vox_mqtt_encode_pubrec(NULL, 0, packet_id);
        if (need > 0) {
            uint8_t* buf = (uint8_t*)vox_mpool_alloc(s->mpool, need);
            if (!buf) {
                vox_list_remove(&from->pending_qos2_in_list, &pending->node);
                if (pending->topic) vox_mpool_free(s->mpool, pending->topic);
                if (pending->payload) vox_mpool_free(s->mpool, pending->payload);
                vox_mpool_free(s->mpool, pending);
                return -1;
            }
            vox_mqtt_encode_pubrec(buf, need, packet_id);
            conn_send(from, buf, need);
        }
        return 0;
    }

    forward_message_to_subscribers(s, from, topic, topic_len, payload, payload_len, qos, retain);
    return 0;
}

static int on_pubrec_from_client(void* user_data, uint16_t packet_id) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)user_data;
    vox_mqtt_server_t* s = conn->server;
    vox_list_node_t* pos;
    vox_list_for_each(pos, &conn->pending_qos2_out_list) {
        vox_mqtt_srv_pending_qos2_out_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_srv_pending_qos2_out_t, node);
        if (p->packet_id != packet_id) continue;
        p->state = 1;
        size_t need = vox_mqtt_encode_pubrel(NULL, 0, packet_id);
        if (need > 0) {
            uint8_t* buf = (uint8_t*)vox_mpool_alloc(s->mpool, need);
            if (buf) {
                vox_mqtt_encode_pubrel(buf, need, packet_id);
                conn_send(conn, buf, need);
            }
        }
        break;
    }
    return 0;
}

static int on_pubrel_from_client(void* user_data, uint16_t packet_id) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)user_data;
    vox_mqtt_server_t* s = conn->server;
    vox_list_node_t* pos, * n;
    vox_list_for_each_safe(pos, n, &conn->pending_qos2_in_list) {
        vox_mqtt_srv_pending_qos2_in_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_srv_pending_qos2_in_t, node);
        if (p->packet_id != packet_id) continue;
        vox_list_remove(&conn->pending_qos2_in_list, pos);
        size_t need = vox_mqtt_encode_pubcomp(NULL, 0, packet_id);
        if (need > 0) {
            uint8_t* buf = (uint8_t*)vox_mpool_alloc(s->mpool, need);
            if (buf) {
                vox_mqtt_encode_pubcomp(buf, need, packet_id);
                conn_send(conn, buf, need);
            }
        }
        forward_message_to_subscribers(s, conn, p->topic, p->topic_len,
            p->payload ? p->payload : "", p->payload_len, p->qos, p->retain);
        if (p->topic) vox_mpool_free(s->mpool, p->topic);
        if (p->payload) vox_mpool_free(s->mpool, p->payload);
        vox_mpool_free(s->mpool, p);
        break;
    }
    return 0;
}

static int on_pubcomp_from_client(void* user_data, uint16_t packet_id) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)user_data;
    vox_mqtt_server_t* s = conn->server;
    vox_list_node_t* pos, * n;
    vox_list_for_each_safe(pos, n, &conn->pending_qos2_out_list) {
        vox_mqtt_srv_pending_qos2_out_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_srv_pending_qos2_out_t, node);
        if (p->packet_id == packet_id) {
            vox_list_remove(&conn->pending_qos2_out_list, pos);
            vox_mpool_free(s->mpool, p);
            break;
        }
    }
    return 0;
}

static int on_pingreq(void* user_data) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)user_data;
    /* PINGRESP 格式在 3.1.1 与 5 中相同 */
    size_t need = vox_mqtt_encode_pingresp(NULL, 0);
    uint8_t* buf = (uint8_t*)vox_mpool_alloc(conn->server->mpool, need);
    if (!buf) return -1;
    vox_mqtt_encode_pingresp(buf, need);
    conn_send(conn, buf, need);
    return 0;
}

static int on_error_conn(void* user_data, const char* message) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)user_data;
    (void)message;
    conn_close(conn);
    return 0;
}

/* 创建 MQTT 连接并挂接 parser，返回 conn；失败返回 NULL（调用方负责释放已分配资源） */
static vox_mqtt_connection_t* conn_create_common(vox_mqtt_server_t* s) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)vox_mpool_alloc(s->mpool, sizeof(vox_mqtt_connection_t));
    if (!conn) return NULL;
    memset(conn, 0, sizeof(*conn));
    conn->server = s;
    vox_list_init(&conn->subscriptions);
    vox_list_init(&conn->pending_writes);
    vox_list_init(&conn->pending_qos2_in_list);
    vox_list_init(&conn->pending_qos2_out_list);

    vox_mqtt_parser_config_t pcfg = { 0 };
    vox_mqtt_parser_callbacks_t pcb = { 0 };
    pcb.on_connect = on_connect;
    pcb.on_subscribe = on_subscribe;
    pcb.on_subscribe_done = on_subscribe_done;
    pcb.on_unsubscribe = on_unsubscribe;
    pcb.on_publish = on_publish_from_client;
    pcb.on_pubrec = on_pubrec_from_client;
    pcb.on_pubrel = on_pubrel_from_client;
    pcb.on_pubcomp = on_pubcomp_from_client;
    pcb.on_pingreq = on_pingreq;
    pcb.on_error = on_error_conn;
    pcb.user_data = conn;
    conn->parser = vox_mqtt_parser_create(s->mpool, &pcfg, &pcb);
    if (!conn->parser) {
        vox_mpool_free(s->mpool, conn);
        return NULL;
    }
    vox_list_push_back(&s->connections, &conn->node);
    return conn;
}

static void on_tcp_connection(vox_tcp_t* listener, int status, void* user_data) {
    vox_mqtt_server_t* s = (vox_mqtt_server_t*)user_data;
    if (status != 0 || !s) return;

    vox_tcp_t* client = vox_tcp_create(s->loop);
    if (!client) return;
    if (vox_tcp_accept(listener, client) != 0) {
        vox_tcp_destroy(client);
        return;
    }

    vox_mqtt_connection_t* conn = conn_create_common(s);
    if (!conn) {
        vox_tcp_destroy(client);
        return;
    }
    conn->tcp = client;
    vox_handle_set_data((vox_handle_t*)client, conn);

    if (vox_tcp_read_start(client, NULL, tcp_read_cb) != 0) {
        conn_close(conn);
        return;
    }
}

#if defined(VOX_USE_SSL) && VOX_USE_SSL
static void mqtt_tls_read_cb(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)tls->handle.data;
    (void)user_data;
    if (!conn) return;
    if (nread <= 0) { conn_close(conn); return; }
    if (conn_feed_parser(conn, buf, (size_t)nread)) conn_close(conn);
}

static void mqtt_on_tls_handshake(vox_tls_t* tls, int status, void* user_data) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)user_data;
    (void)tls;
    if (!conn) return;
    if (status != 0) {
        vox_tls_destroy(conn->tls);
        conn->tls = NULL;
        conn_close(conn);
        return;
    }
    if (vox_tls_read_start(conn->tls, NULL, mqtt_tls_read_cb) != 0) {
        conn_close(conn);
        return;
    }
}

static void on_tls_connection(vox_tls_t* listener, int status, void* user_data) {
    vox_mqtt_server_t* s = (vox_mqtt_server_t*)user_data;
    if (status != 0 || !s) return;

    vox_ssl_context_t* ssl_ctx = listener->ssl_ctx;
    vox_tls_t* client = vox_tls_create(s->loop, ssl_ctx);
    if (!client) return;
    if (vox_tls_accept(listener, client) != 0) {
        vox_tls_destroy(client);
        return;
    }

    vox_mqtt_connection_t* conn = conn_create_common(s);
    if (!conn) {
        vox_tls_destroy(client);
        return;
    }
    conn->tls = client;
    vox_handle_set_data((vox_handle_t*)client, conn);

    if (vox_tls_handshake(client, mqtt_on_tls_handshake) != 0) {
        conn_close(conn);
        return;
    }
}
#endif

#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
static void mqtt_ws_on_connection(vox_ws_connection_t* ws_conn, void* user_data) {
    vox_mqtt_server_t* s = (vox_mqtt_server_t*)user_data;
    if (!s) return;

    vox_mqtt_connection_t* conn = conn_create_common(s);
    if (!conn) {
        vox_ws_connection_close(ws_conn, 1011, "internal error");
        return;
    }
    conn->ws_conn = ws_conn;
    vox_ws_connection_set_user_data(ws_conn, conn);
}

static void mqtt_ws_on_message(vox_ws_connection_t* ws_conn, const void* data, size_t len,
    vox_ws_message_type_t type, void* user_data) {
    (void)user_data;
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)vox_ws_connection_get_user_data(ws_conn);
    if (!conn || type != VOX_WS_MSG_BINARY) return;
    if (conn_feed_parser(conn, data, len)) conn_close(conn);
}

static void mqtt_ws_on_close(vox_ws_connection_t* ws_conn, uint16_t code, const char* reason, void* user_data) {
    (void)code;
    (void)reason;
    (void)user_data;
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)vox_ws_connection_get_user_data(ws_conn);
    if (conn) {
        conn->ws_conn = NULL;
        conn_close(conn);
    }
}

static void mqtt_ws_on_error(vox_ws_connection_t* ws_conn, const char* error, void* user_data) {
    (void)error;
    (void)user_data;
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)vox_ws_connection_get_user_data(ws_conn);
    if (conn) {
        conn->ws_conn = NULL;
        conn_close(conn);
    }
}
#endif

static void tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    vox_mqtt_connection_t* conn = (vox_mqtt_connection_t*)tcp->handle.data;
    (void)user_data;
    if (!conn) return;
    if (nread <= 0) { conn_close(conn); return; }
    if (conn_feed_parser(conn, buf, (size_t)nread)) conn_close(conn);
}

vox_mqtt_server_t* vox_mqtt_server_create(const vox_mqtt_server_config_t* config) {
    if (!config || !config->loop) return NULL;
    vox_mpool_t* mpool = config->mpool;
    bool own = false;
    if (!mpool) {
        mpool = vox_mpool_create();
        if (!mpool) return NULL;
        own = true;
    }
    vox_mqtt_server_t* s = (vox_mqtt_server_t*)vox_mpool_alloc(mpool, sizeof(vox_mqtt_server_t));
    if (!s) {
        if (own) vox_mpool_destroy(mpool);
        return NULL;
    }
    memset(s, 0, sizeof(vox_mqtt_server_t));
    s->loop = config->loop;
    s->mpool = mpool;
    s->owns_mpool = own;
    s->config = *config;
    vox_list_init(&s->connections);
    return s;
}

void vox_mqtt_server_destroy(vox_mqtt_server_t* s) {
    if (!s) return;
    vox_mqtt_server_close(s);
    vox_list_node_t* pos, * n;
    vox_list_for_each_safe(pos, n, &s->connections) {
        vox_mqtt_connection_t* c = VOX_CONTAINING_RECORD(pos, vox_mqtt_connection_t, node);
        conn_close(c);
    }
    if (s->owns_mpool && s->mpool) vox_mpool_destroy(s->mpool);
}

int vox_mqtt_server_listen(vox_mqtt_server_t* s, const vox_socket_addr_t* addr, int backlog) {
    if (!s || !addr) return -1;
    s->tcp_listener = vox_tcp_create(s->loop);
    if (!s->tcp_listener) return -1;
    if (vox_tcp_bind(s->tcp_listener, addr, 0) != 0) {
        vox_tcp_destroy(s->tcp_listener);
        s->tcp_listener = NULL;
        return -1;
    }
    if (vox_tcp_listen(s->tcp_listener, backlog, on_tcp_connection) != 0) {
        vox_tcp_destroy(s->tcp_listener);
        s->tcp_listener = NULL;
        return -1;
    }
    s->tcp_listener->handle.data = s;
    return 0;
}

#if defined(VOX_USE_SSL) && VOX_USE_SSL
int vox_mqtt_server_listen_ssl(vox_mqtt_server_t* s, const vox_socket_addr_t* addr,
    int backlog, vox_ssl_context_t* ssl_ctx) {
    if (!s || !addr || !ssl_ctx) return -1;
    s->tls_listener = vox_tls_create(s->loop, ssl_ctx);
    if (!s->tls_listener) return -1;
    if (vox_tls_bind(s->tls_listener, addr, 0) != 0) {
        vox_tls_destroy(s->tls_listener);
        s->tls_listener = NULL;
        return -1;
    }
    if (vox_tls_listen(s->tls_listener, backlog, on_tls_connection) != 0) {
        vox_tls_destroy(s->tls_listener);
        s->tls_listener = NULL;
        return -1;
    }
    s->tls_listener->handle.data = s;
    return 0;
}
#endif

#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
/** WS/WSS 共用：path_copy 由调用方分配并传入，失败时由调用方释放；ssl_ctx 非 NULL 时为 WSS */
static int listen_ws_common(vox_mqtt_server_t* s, const vox_socket_addr_t* addr,
    int backlog, char* path_copy, vox_ssl_context_t* ssl_ctx) {
    vox_ws_server_config_t wscfg = { 0 };
    wscfg.loop = s->loop;
    wscfg.ssl_ctx = ssl_ctx;
    wscfg.on_connection = mqtt_ws_on_connection;
    wscfg.on_message = mqtt_ws_on_message;
    wscfg.on_close = mqtt_ws_on_close;
    wscfg.on_error = mqtt_ws_on_error;
    wscfg.user_data = s;
    wscfg.path = path_copy;
    s->ws_server = vox_ws_server_create(&wscfg);
    if (!s->ws_server) return -1;
    s->ws_path = path_copy;
    int ret = ssl_ctx
        ? vox_ws_server_listen_ssl(s->ws_server, addr, backlog, ssl_ctx)
        : vox_ws_server_listen(s->ws_server, addr, backlog);
    if (ret != 0) {
        vox_ws_server_destroy(s->ws_server);
        s->ws_server = NULL;
        s->ws_path = NULL;
        return -1;
    }
    return 0;
}

int vox_mqtt_server_listen_ws(vox_mqtt_server_t* s, const vox_socket_addr_t* addr,
    int backlog, const char* path) {
    if (!s || !addr || !path) return -1;
    size_t plen = strlen(path) + 1;
    char* path_copy = (char*)vox_mpool_alloc(s->mpool, plen);
    if (!path_copy) return -1;
    memcpy(path_copy, path, plen);
    if (listen_ws_common(s, addr, backlog, path_copy, NULL) != 0) {
        vox_mpool_free(s->mpool, path_copy);
        return -1;
    }
    return 0;
}

int vox_mqtt_server_listen_wss(vox_mqtt_server_t* s, const vox_socket_addr_t* addr,
    int backlog, const char* path, vox_ssl_context_t* ssl_ctx) {
    if (!s || !addr || !path || !ssl_ctx) return -1;
    size_t plen = strlen(path) + 1;
    char* path_copy = (char*)vox_mpool_alloc(s->mpool, plen);
    if (!path_copy) return -1;
    memcpy(path_copy, path, plen);
    if (listen_ws_common(s, addr, backlog, path_copy, ssl_ctx) != 0) {
        vox_mpool_free(s->mpool, path_copy);
        return -1;
    }
    return 0;
}
#endif

void vox_mqtt_server_close(vox_mqtt_server_t* s) {
    if (!s) return;
    if (s->tcp_listener) {
        vox_tcp_destroy(s->tcp_listener);
        s->tcp_listener = NULL;
    }
#if defined(VOX_USE_SSL) && VOX_USE_SSL
    if (s->tls_listener) {
        vox_tls_destroy(s->tls_listener);
        s->tls_listener = NULL;
    }
#endif
#if defined(VOX_USE_WEBSOCKET) && VOX_USE_WEBSOCKET
    if (s->ws_server) {
        vox_ws_server_close(s->ws_server);
        vox_ws_server_destroy(s->ws_server);
        s->ws_server = NULL;
        if (s->ws_path) {
            vox_mpool_free(s->mpool, (void*)s->ws_path);
            s->ws_path = NULL;
        }
    }
#endif
}

int vox_mqtt_connection_publish(vox_mqtt_connection_t* conn,
    const char* topic, size_t topic_len,
    const void* payload, size_t payload_len,
    uint8_t qos, bool retain) {
    if (!conn || !topic) return -1;
    if (topic_len == 0) topic_len = strlen(topic);
    if (qos > 2) return -1;
    uint16_t pid = 0;
    if (qos > 0) {
        if (++conn->next_packet_id == 0) conn->next_packet_id = 1;
        pid = conn->next_packet_id;
    }
    size_t need = (conn->protocol_version == VOX_MQTT_VERSION_5)
        ? vox_mqtt_encode_publish_v5(NULL, 0, qos, retain, pid, topic, topic_len, payload, payload_len)
        : vox_mqtt_encode_publish(NULL, 0, qos, retain, pid, topic, topic_len, payload, payload_len);
    if (need == 0) return -1;
    uint8_t* buf = (uint8_t*)vox_mpool_alloc(conn->server->mpool, need);
    if (!buf) return -1;
    if (conn->protocol_version == VOX_MQTT_VERSION_5) {
        vox_mqtt_encode_publish_v5(buf, need, qos, retain, pid, topic, topic_len, payload, payload_len);
    } else {
        vox_mqtt_encode_publish(buf, need, qos, retain, pid, topic, topic_len, payload, payload_len);
    }
    conn_send(conn, buf, need);
    if (qos == 2) {
        vox_mqtt_srv_pending_qos2_out_t* out = (vox_mqtt_srv_pending_qos2_out_t*)vox_mpool_alloc(conn->server->mpool, sizeof(vox_mqtt_srv_pending_qos2_out_t));
        if (out) {
            memset(out, 0, sizeof(*out));
            out->packet_id = pid;
            out->state = 0;
            vox_list_push_back(&conn->pending_qos2_out_list, &out->node);
        }
    }
    return 0;
}

void* vox_mqtt_connection_get_user_data(vox_mqtt_connection_t* conn) {
    return conn ? conn->user_data : NULL;
}
void vox_mqtt_connection_set_user_data(vox_mqtt_connection_t* conn, void* user_data) {
    if (conn) conn->user_data = user_data;
}

uint8_t vox_mqtt_connection_get_protocol_version(vox_mqtt_connection_t* conn) {
    return conn ? conn->protocol_version : 0;
}

uint32_t vox_mqtt_connection_get_session_expiry_interval(vox_mqtt_connection_t* conn) {
    return conn ? conn->session_expiry_interval : 0;
}

uint16_t vox_mqtt_connection_get_receive_maximum(vox_mqtt_connection_t* conn) {
    return conn ? conn->receive_maximum : 0;
}
