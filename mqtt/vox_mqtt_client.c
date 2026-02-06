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

    uint8_t* pending_connect_buf;
    size_t pending_connect_len;

    uint8_t protocol_version; /* 4=3.1.1, 5=MQTT 5，连接成功后用于选择编码 */

    /* QoS 2 出站：仅支持单路 in-flight，0 表示无 */
    uint16_t pending_qos2_out_packet_id;
    uint8_t pending_qos2_out_state; /* 0=等 PUBREC，1=等 PUBCOMP */
    /* QoS 2 入站：等 PUBREL 后投递 */
    vox_list_t pending_qos2_in_list;

    vox_list_t write_queue;

    bool transport_close_pending;  /* true 表示已排队延迟关闭，disconnect() 不再 destroy tls/tcp */
};

static void connect_cleanup_on_fail(vox_mqtt_client_t* c);

static void client_fail(vox_mqtt_client_t* c, const char* msg) {
    if (!c) return;
    if (c->connecting) connect_cleanup_on_fail(c);
    c->connected = false;
    c->connecting = false;
    if (c->connect_cb) {
        vox_mqtt_connect_cb cb = c->connect_cb;
        void* ud = c->connect_user_data;
        c->connect_cb = NULL;
        c->connect_user_data = NULL;
        cb(c, -1, ud);
    }
    if (c->error_cb) c->error_cb(c, msg ? msg : "error", c->error_user_data);
    if (c->disconnect_cb) c->disconnect_cb(c, c->disconnect_user_data);
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

static int on_connack(void* user_data, uint8_t session_present, uint8_t return_code) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    (void)session_present;
    if (!c || !c->connecting) return 0;
    c->connecting = false;
    c->connected = (return_code == VOX_MQTT_CONNACK_ACCEPTED);
    if (c->connect_cb) {
        vox_mqtt_connect_cb cb = c->connect_cb;
        void* ud = c->connect_user_data;
        c->connect_cb = NULL;
        c->connect_user_data = NULL;
        cb(c, c->connected ? 0 : (int)return_code, ud);
    }
    if (!c->connected) {
        client_fail(c, "connack refused");
        return 0;
    }
    if (c->keepalive_sec > 0) {
        uint64_t interval_ms = (uint64_t)c->keepalive_sec * 500; /* ping at half keepalive */
        if (interval_ms < 1000) interval_ms = 1000;
        vox_timer_start(&c->ping_timer, interval_ms, interval_ms, ping_timer_cb, c);
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

static int on_pubrec(void* user_data, uint16_t packet_id) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    if (c->pending_qos2_out_packet_id != packet_id || c->pending_qos2_out_state != 0) return 0;
    c->pending_qos2_out_state = 1;
    size_t need = vox_mqtt_encode_pubrel(NULL, 0, packet_id);
    if (need > 0) {
        uint8_t* buf = (uint8_t*)vox_mpool_alloc(c->mpool, need);
        if (buf) {
            vox_mqtt_encode_pubrel(buf, need, packet_id);
            if (send_buf(c, buf, need) != 0)
                vox_mpool_free(c->mpool, buf);
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
    if (c->pending_qos2_out_packet_id == packet_id) {
        c->pending_qos2_out_packet_id = 0;
        c->pending_qos2_out_state = 0;
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
        cb(c, packet_id, return_codes, count, ud);
    }
    return 0;
}

static int on_error(void* user_data, const char* message) {
    vox_mqtt_client_t* c = (vox_mqtt_client_t*)user_data;
    client_fail(c, message ? message : "parser error");
    return 0;
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
    vox_list_init(&c->pending_qos2_in_list);
    if (vox_timer_init(&c->ping_timer, loop) != 0) {
        vox_mpool_free(loop_mpool, c);
        return NULL;
    }

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
    if (c->parser) vox_mqtt_parser_destroy(c->parser);
    c->parser = NULL;
    vox_list_node_t* pos, * n;
    vox_list_for_each_safe(pos, n, &c->pending_qos2_in_list) {
        vox_mqtt_pending_qos2_in_t* p = VOX_CONTAINING_RECORD(pos, vox_mqtt_pending_qos2_in_t, node);
        vox_list_remove(&c->pending_qos2_in_list, pos);
        if (p->topic) vox_mpool_free(pool, p->topic);
        if (p->payload) vox_mpool_free(pool, p->payload);
        vox_mpool_free(pool, p);
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
    if (qos == 2 && c->pending_qos2_out_packet_id != 0) return -1; /* 仅支持单路 QoS 2 in-flight */

    uint16_t packet_id = 0;
    if (qos > 0) {
        if (++c->next_packet_id == 0) c->next_packet_id = 1;  /* MQTT packet_id 不能为 0 */
        packet_id = c->next_packet_id;
    }
    if (qos == 2) {
        c->pending_qos2_out_packet_id = packet_id;
        c->pending_qos2_out_state = 0;
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
    if (send_buf(c, buf, need) != 0) {
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
    return 0;
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
