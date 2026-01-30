/*
 * vox_websocket_server.c - WebSocket 服务器实现
 */

#include "vox_websocket_server.h"
#include "../vox_crypto.h"
#include "../vox_log.h"
#include <string.h>

/* WebSocket 连接状态 */
typedef enum {
    VOX_WS_CONN_HANDSHAKING,  /* 正在握手 */
    VOX_WS_CONN_OPEN,         /* 已打开 */
    VOX_WS_CONN_CLOSING,      /* 正在关闭 */
    VOX_WS_CONN_CLOSED        /* 已关闭 */
} vox_ws_conn_state_t;

/* WebSocket 连接结构 */
struct vox_ws_connection {
    vox_ws_server_t* server;        /* 所属服务器 */
    vox_tcp_t* tcp;                 /* TCP 连接（WS） */
    vox_tls_t* tls;                 /* TLS 连接（WSS） */
    vox_mpool_t* mpool;             /* 内存池 */
    vox_ws_parser_t* parser;        /* 帧解析器 */
    vox_ws_conn_state_t state;      /* 连接状态 */
    vox_string_t* handshake_buffer; /* 握手缓冲区 */
    bool handshake_complete;        /* 握手是否完成 */
    bool close_sent;                /* 是否已发送关闭帧 */
    void* user_data;                /* 用户数据 */
};

/* WebSocket 服务器结构 */
struct vox_ws_server {
    vox_loop_t* loop;               /* 事件循环 */
    vox_mpool_t* mpool;             /* 内存池（独立创建） */
    vox_tcp_t* tcp_listener;        /* TCP 监听器（WS） */
    vox_tls_t* tls_listener;        /* TLS 监听器（WSS） */
    vox_ssl_context_t* ssl_ctx;     /* SSL 上下文 */
    vox_ws_server_config_t config;  /* 配置 */
    bool is_ssl;                    /* 是否使用 SSL */
    bool owns_mpool;                /* 是否拥有内存池 */
};

/* WebSocket GUID（RFC 6455） */
static const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* 前向声明 */
static void ws_on_tcp_connection(vox_tcp_t* server, int status, void* user_data);
static void ws_on_tls_connection(vox_tls_t* server, int status, void* user_data);
static void ws_on_tls_handshake(vox_tls_t* tls, int status, void* user_data);
static void ws_on_tcp_read(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data);
static void ws_on_tls_read(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data);
static int ws_handle_handshake(vox_ws_connection_t* conn, const char* data, size_t len);
static int ws_handle_frame(vox_ws_connection_t* conn, const vox_ws_frame_t* frame, int frame_len);

/* 创建服务器 */
vox_ws_server_t* vox_ws_server_create(const vox_ws_server_config_t* config) {
    if (!config || !config->loop) return NULL;
    
    /* 创建独立的内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) return NULL;
    
    vox_ws_server_t* server = (vox_ws_server_t*)vox_mpool_alloc(mpool, sizeof(vox_ws_server_t));
    if (!server) {
        vox_mpool_destroy(mpool);
        return NULL;
    }
    
    memset(server, 0, sizeof(vox_ws_server_t));
    server->loop = config->loop;
    server->mpool = mpool;
    server->ssl_ctx = config->ssl_ctx;
    server->config = *config;
    server->owns_mpool = true;
    
    return server;
}

/* 销毁服务器 */
void vox_ws_server_destroy(vox_ws_server_t* server) {
    if (!server) return;
    
    vox_ws_server_close(server);
    
    /* 销毁内存池 */
    if (server->owns_mpool && server->mpool) {
        vox_mpool_destroy(server->mpool);
    }
}

/* 监听（WS） */
int vox_ws_server_listen(vox_ws_server_t* server, const vox_socket_addr_t* addr, int backlog) {
    if (!server || !addr) return -1;
    
    server->is_ssl = false;
    server->tcp_listener = vox_tcp_create(server->loop);
    if (!server->tcp_listener) return -1;
    
    if (vox_tcp_bind(server->tcp_listener, addr, 0) != 0) {
        vox_tcp_destroy(server->tcp_listener);
        server->tcp_listener = NULL;
        return -1;
    }
    
    if (vox_tcp_listen(server->tcp_listener, backlog, ws_on_tcp_connection) != 0) {
        vox_tcp_destroy(server->tcp_listener);
        server->tcp_listener = NULL;
        return -1;
    }
    
    server->tcp_listener->handle.data = server;
    return 0;
}

/* 监听（WSS） */
int vox_ws_server_listen_ssl(vox_ws_server_t* server, const vox_socket_addr_t* addr,
                              int backlog, vox_ssl_context_t* ssl_ctx) {
    if (!server || !addr || !ssl_ctx) return -1;
    
    server->is_ssl = true;
    server->ssl_ctx = ssl_ctx;
    server->tls_listener = vox_tls_create(server->loop, ssl_ctx);
    if (!server->tls_listener) return -1;
    
    if (vox_tls_bind(server->tls_listener, addr, 0) != 0) {
        vox_tls_destroy(server->tls_listener);
        server->tls_listener = NULL;
        return -1;
    }
    
    if (vox_tls_listen(server->tls_listener, backlog, ws_on_tls_connection) != 0) {
        vox_tls_destroy(server->tls_listener);
        server->tls_listener = NULL;
        return -1;
    }
    
    server->tls_listener->handle.data = server;
    return 0;
}

/* 关闭服务器 */
void vox_ws_server_close(vox_ws_server_t* server) {
    if (!server) return;
    
    if (server->tcp_listener) {
        vox_tcp_destroy(server->tcp_listener);
        server->tcp_listener = NULL;
    }
    
    if (server->tls_listener) {
        vox_tls_destroy(server->tls_listener);
        server->tls_listener = NULL;
    }
}

/* TCP 连接回调 */
static void ws_on_tcp_connection(vox_tcp_t* server, int status, void* user_data) {
    (void)user_data;
    
    if (status != 0) return;
    
    vox_ws_server_t* ws_server = (vox_ws_server_t*)server->handle.data;
    if (!ws_server) return;
    
    /* 创建连接 */
    vox_ws_connection_t* conn = (vox_ws_connection_t*)vox_mpool_alloc(
        ws_server->mpool, sizeof(vox_ws_connection_t));
    if (!conn) return;
    
    memset(conn, 0, sizeof(vox_ws_connection_t));
    conn->server = ws_server;
    conn->mpool = ws_server->mpool;
    conn->state = VOX_WS_CONN_HANDSHAKING;
    conn->parser = vox_ws_parser_create(conn->mpool);
    conn->handshake_buffer = vox_string_create(conn->mpool);
    
    if (!conn->parser || !conn->handshake_buffer) return;
    
    /* 接受连接 */
    conn->tcp = vox_tcp_create(ws_server->loop);
    if (!conn->tcp) return;
    
    if (vox_tcp_accept(server, conn->tcp) != 0) {
        vox_tcp_destroy(conn->tcp);
        return;
    }
    
    conn->tcp->handle.data = conn;
    vox_tcp_nodelay(conn->tcp, true);
    vox_tcp_read_start(conn->tcp, NULL, ws_on_tcp_read);
}

/* TLS 连接回调 */
static void ws_on_tls_connection(vox_tls_t* server, int status, void* user_data) {
    (void)user_data;
    
    if (status != 0) return;
    
    vox_ws_server_t* ws_server = (vox_ws_server_t*)server->handle.data;
    if (!ws_server) return;
    
    /* 创建连接 */
    vox_ws_connection_t* conn = (vox_ws_connection_t*)vox_mpool_alloc(
        ws_server->mpool, sizeof(vox_ws_connection_t));
    if (!conn) return;
    
    memset(conn, 0, sizeof(vox_ws_connection_t));
    conn->server = ws_server;
    conn->mpool = ws_server->mpool;
    conn->state = VOX_WS_CONN_HANDSHAKING;
    conn->parser = vox_ws_parser_create(conn->mpool);
    conn->handshake_buffer = vox_string_create(conn->mpool);
    
    if (!conn->parser || !conn->handshake_buffer) return;
    
    /* 接受连接 */
    conn->tls = vox_tls_create(ws_server->loop, ws_server->ssl_ctx);
    if (!conn->tls) return;
    
    if (vox_tls_accept(server, conn->tls) != 0) {
        vox_tls_destroy(conn->tls);
        return;
    }
    
    conn->tls->handle.data = conn;
    vox_tls_nodelay(conn->tls, true);
    
    /* 开始 TLS 握手 */
    if (vox_tls_handshake(conn->tls, ws_on_tls_handshake) != 0) {
        vox_tls_destroy(conn->tls);
        return;
    }
}

/* TLS 握手完成回调 */
static void ws_on_tls_handshake(vox_tls_t* tls, int status, void* user_data) {
    (void)user_data;
    
    vox_ws_connection_t* conn = (vox_ws_connection_t*)tls->handle.data;
    if (!conn) return;
    
    if (status != 0) {
        /* 握手失败 */
        vox_tls_destroy(conn->tls);
        return;
    }
    
    /* 握手成功，开始读取 WebSocket 数据 */
    vox_tls_read_start(conn->tls, NULL, ws_on_tls_read);
}

/* 解析 HTTP 头 */
static const char* ws_get_header(vox_mpool_t* mpool, const char* headers, size_t len, const char* name) {
    if (!mpool || !headers || !name) return NULL;
    
    size_t name_len = strlen(name);
    const char* p = headers;
    const char* end = headers + len;
    
    while (p < end) {
        /* 查找行尾 */
        const char* line_end = p;
        while (line_end < end && *line_end != '\r' && *line_end != '\n') {
            line_end++;
        }
        
        /* 检查是否匹配 */
        if ((size_t)(line_end - p) > name_len + 1) {
            if (strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
                /* 跳过冒号和空格 */
                const char* value = p + name_len + 1;
                while (value < line_end && (*value == ' ' || *value == '\t')) {
                    value++;
                }
                
                /* 分配并返回值 */
                size_t value_len = line_end - value;
                char* result = (char*)vox_mpool_alloc(mpool, value_len + 1);
                if (result) {
                    memcpy(result, value, value_len);
                    result[value_len] = '\0';
                    return result;
                }
            }
        }
        
        /* 跳到下一行 */
        p = line_end;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
    
    return NULL;
}

/* 处理握手 */
static int ws_handle_handshake(vox_ws_connection_t* conn, const char* data, size_t len) {
    if (!conn || !data || len == 0) return -1;
    
    /* 追加到握手缓冲区 */
    vox_string_append_data(conn->handshake_buffer, data, len);
    
    /* 查找握手结束标记 */
    const char* buf = vox_string_cstr(conn->handshake_buffer);
    size_t buf_len = vox_string_length(conn->handshake_buffer);
    const char* end_marker = strstr(buf, "\r\n\r\n");
    
    if (!end_marker) {
        /* 握手未完成 */
        if (buf_len > 8192) {
            /* 握手数据过大 */
            return -1;
        }
        return 0;
    }
    
    /* 解析握手 */
    const char* key = ws_get_header(conn->mpool, buf, buf_len, "Sec-WebSocket-Key");
    const char* version = ws_get_header(conn->mpool, buf, buf_len, "Sec-WebSocket-Version");
    
    if (!key || !version || strcmp(version, "13") != 0) {
        /* 注意：key 和 version 来自内存池，不需要 free */
        return -1;
    }
    
    /* 计算 Accept */
    size_t key_len = strlen(key);
    size_t guid_len = strlen(WS_GUID);
    char* concat = (char*)vox_mpool_alloc(conn->mpool, key_len + guid_len);
    if (!concat) {
        return -1;
    }
    
    memcpy(concat, key, key_len);
    memcpy(concat + key_len, WS_GUID, guid_len);
    
    uint8_t digest[VOX_SHA1_DIGEST_SIZE];
    vox_sha1(concat, key_len + guid_len, digest);
    
    char accept[64];
    int accept_len = vox_base64_encode(digest, sizeof(digest), accept, sizeof(accept));
    if (accept_len <= 0) {
        return -1;
    }
    accept[accept_len] = '\0';
    
    /* 注意：key 和 version 来自内存池，不需要 free */
    
    /* 构建响应 */
    vox_string_t* response = vox_string_create(conn->mpool);
    if (!response) return -1;
    
    vox_string_append(response, "HTTP/1.1 101 Switching Protocols\r\n");
    vox_string_append(response, "Upgrade: websocket\r\n");
    vox_string_append(response, "Connection: Upgrade\r\n");
    vox_string_append(response, "Sec-WebSocket-Accept: ");
    vox_string_append(response, accept);
    vox_string_append(response, "\r\n\r\n");
    
    /* 发送响应 */
    const char* resp_data = vox_string_cstr(response);
    size_t resp_len = vox_string_length(response);
    
    int ret;
    if (conn->tcp) {
        ret = vox_tcp_write(conn->tcp, resp_data, resp_len, NULL);
    } else {
        ret = vox_tls_write(conn->tls, resp_data, resp_len, NULL);
    }
    
    if (ret != 0) return -1;
    
    /* 握手完成 */
    conn->handshake_complete = true;
    conn->state = VOX_WS_CONN_OPEN;
    vox_string_destroy(conn->handshake_buffer);
    conn->handshake_buffer = NULL;
    
    /* 调用连接回调 */
    if (conn->server->config.on_connection) {
        conn->server->config.on_connection(conn, conn->server->config.user_data);
    }
    
    return 1;
}

/* 处理帧 */
static int ws_handle_frame(vox_ws_connection_t* conn, const vox_ws_frame_t* frame, int frame_len) {
    if (!conn || !frame) return -1;
    
    /* 复制并解掩码负载 */
    uint8_t* payload = NULL;
    if (frame->payload_len > 0) {
        payload = (uint8_t*)vox_mpool_alloc(conn->mpool, frame->payload_len);
        if (!payload) return -1;
        
        memcpy(payload, frame->payload, frame->payload_len);
        if (frame->masked) {
            vox_ws_mask_payload(payload, frame->payload_len, frame->mask_key);
        }
    }
    
    /* 处理不同类型的帧 */
    if (frame->opcode == VOX_WS_OP_TEXT) {
        /* 文本消息 */
        if (!vox_ws_validate_utf8(payload, frame->payload_len)) {
            vox_ws_connection_close(conn, VOX_WS_CLOSE_INVALID_DATA, "Invalid UTF-8");
            return -1;
        }
        
        if (conn->server->config.on_message) {
            conn->server->config.on_message(conn, payload, frame->payload_len,
                                           VOX_WS_MSG_TEXT, conn->server->config.user_data);
        }
    } else if (frame->opcode == VOX_WS_OP_BINARY) {
        /* 二进制消息 */
        if (conn->server->config.on_message) {
            conn->server->config.on_message(conn, payload, frame->payload_len,
                                           VOX_WS_MSG_BINARY, conn->server->config.user_data);
        }
    } else if (frame->opcode == VOX_WS_OP_CLOSE) {
        /* 关闭帧 */
        uint16_t code = VOX_WS_CLOSE_NORMAL;
        const char* reason = "";
        char* reason_buf = NULL;
        
        /* RFC 6455: Close payload 必须是 0 字节或至少 2 字节（状态码） */
        if (frame->payload_len == 1) {
            vox_ws_connection_close(conn, VOX_WS_CLOSE_PROTOCOL_ERROR, "Invalid close frame");
            return -1;
        }
        
        if (frame->payload_len >= 2) {
            code = ((uint16_t)payload[0] << 8) | (uint16_t)payload[1];
            
            /* RFC 6455: 验证状态码范围和保留码 */
            if (code < 1000 || code > 4999 || 
                code == 1004 || code == 1005 || code == 1006 || code == 1015) {
                vox_ws_connection_close(conn, VOX_WS_CLOSE_PROTOCOL_ERROR, "Invalid close code");
                return -1;
            }
            
            if (frame->payload_len > 2) {
                size_t reason_len = frame->payload_len - 2;
                
                /* RFC 6455: 验证 reason 的 UTF-8 编码 */
                if (!vox_ws_validate_utf8(payload + 2, reason_len)) {
                    vox_ws_connection_close(conn, VOX_WS_CLOSE_INVALID_DATA, "Invalid UTF-8 in close reason");
                    return -1;
                }
                
                reason_buf = (char*)vox_mpool_alloc(conn->mpool, reason_len + 1);
                if (reason_buf) {
                    memcpy(reason_buf, payload + 2, reason_len);
                    reason_buf[reason_len] = '\0';
                    reason = reason_buf;
                }
            }
        }
        
        if (conn->server->config.on_close) {
            conn->server->config.on_close(conn, code, reason, conn->server->config.user_data);
        }
        
        /* 回复关闭帧 */
        if (!conn->close_sent) {
            vox_ws_connection_close(conn, code, reason);
        }
        
        conn->state = VOX_WS_CONN_CLOSED;
        return -1;
    } else if (frame->opcode == VOX_WS_OP_PING) {
        /* Ping -> 回复 Pong */
        void* pong_frame;
        size_t pong_len;
        if (vox_ws_build_frame(conn->mpool, VOX_WS_OP_PONG, payload,
                              frame->payload_len, false, &pong_frame, &pong_len) == 0) {
            if (conn->tcp) {
                vox_tcp_write(conn->tcp, pong_frame, pong_len, NULL);
            } else {
                vox_tls_write(conn->tls, pong_frame, pong_len, NULL);
            }
        }
    }
    /* PONG 和 CONTINUATION 帧忽略或由解析器处理 */
    
    return frame_len;
}

/* TCP 读取回调 */
static void ws_on_tcp_read(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    (void)user_data;
    
    vox_ws_connection_t* conn = (vox_ws_connection_t*)tcp->handle.data;
    if (!conn) return;
    
    if (nread < 0) {
        /* 连接错误或关闭 */
        if (conn->server->config.on_error) {
            conn->server->config.on_error(conn, "Connection closed", conn->server->config.user_data);
        }
        vox_tcp_destroy(conn->tcp);
        return;
    }
    
    if (nread == 0) return;
    
    /* 处理握手 */
    if (!conn->handshake_complete) {
        int ret = ws_handle_handshake(conn, (const char*)buf, (size_t)nread);
        if (ret < 0) {
            vox_tcp_destroy(conn->tcp);
        }
        return;
    }
    
    /* 解析帧 */
    if (vox_ws_parser_feed(conn->parser, buf, (size_t)nread) != 0) {
        vox_tcp_destroy(conn->tcp);
        return;
    }
    
    /* 处理所有完整的帧 */
    vox_ws_frame_t frame;
    int frame_len;
    while ((frame_len = vox_ws_parser_parse_frame(conn->parser, &frame)) > 0) {
        if (ws_handle_frame(conn, &frame, frame_len) < 0) {
            vox_tcp_destroy(conn->tcp);
            return;
        }
        
        /* 从缓冲区移除已处理的帧 */
        vox_string_remove(conn->parser->buffer, 0, frame_len);
    }
    
    if (frame_len < 0) {
        /* 解析错误 */
        vox_tcp_destroy(conn->tcp);
    }
}

/* TLS 读取回调 */
static void ws_on_tls_read(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data) {
    (void)user_data;
    
    vox_ws_connection_t* conn = (vox_ws_connection_t*)tls->handle.data;
    if (!conn) return;
    
    if (nread < 0) {
        /* 连接错误或关闭 */
        if (conn->server->config.on_error) {
            conn->server->config.on_error(conn, "Connection closed", conn->server->config.user_data);
        }
        vox_tls_destroy(conn->tls);
        return;
    }
    
    if (nread == 0) return;
    
    /* 处理握手 */
    if (!conn->handshake_complete) {
        int ret = ws_handle_handshake(conn, (const char*)buf, (size_t)nread);
        if (ret < 0) {
            vox_tls_destroy(conn->tls);
        }
        return;
    }
    
    /* 解析帧 */
    if (vox_ws_parser_feed(conn->parser, buf, (size_t)nread) != 0) {
        vox_tls_destroy(conn->tls);
        return;
    }
    
    /* 处理所有完整的帧 */
    vox_ws_frame_t frame;
    int frame_len;
    while ((frame_len = vox_ws_parser_parse_frame(conn->parser, &frame)) > 0) {
        if (ws_handle_frame(conn, &frame, frame_len) < 0) {
            vox_tls_destroy(conn->tls);
            return;
        }
        
        /* 从缓冲区移除已处理的帧 */
        vox_string_remove(conn->parser->buffer, 0, frame_len);
    }
    
    if (frame_len < 0) {
        /* 解析错误 */
        vox_tls_destroy(conn->tls);
    }
}

/* 发送文本消息 */
int vox_ws_connection_send_text(vox_ws_connection_t* conn, const char* text, size_t len) {
    if (!conn || !text || len == 0) return -1;
    if (conn->state != VOX_WS_CONN_OPEN) return -1;
    
    void* frame;
    size_t frame_len;
    if (vox_ws_build_frame(conn->mpool, VOX_WS_OP_TEXT, text, len, false, &frame, &frame_len) != 0) {
        return -1;
    }
    
    if (conn->tcp) {
        return vox_tcp_write(conn->tcp, frame, frame_len, NULL);
    } else {
        return vox_tls_write(conn->tls, frame, frame_len, NULL);
    }
}

/* 发送二进制消息 */
int vox_ws_connection_send_binary(vox_ws_connection_t* conn, const void* data, size_t len) {
    if (!conn || !data || len == 0) return -1;
    if (conn->state != VOX_WS_CONN_OPEN) return -1;
    
    void* frame;
    size_t frame_len;
    if (vox_ws_build_frame(conn->mpool, VOX_WS_OP_BINARY, data, len, false, &frame, &frame_len) != 0) {
        return -1;
    }
    
    if (conn->tcp) {
        return vox_tcp_write(conn->tcp, frame, frame_len, NULL);
    } else {
        return vox_tls_write(conn->tls, frame, frame_len, NULL);
    }
}

/* 发送 Ping */
int vox_ws_connection_send_ping(vox_ws_connection_t* conn, const void* data, size_t len) {
    if (!conn) return -1;
    if (conn->state != VOX_WS_CONN_OPEN) return -1;
    
    void* frame;
    size_t frame_len;
    if (vox_ws_build_frame(conn->mpool, VOX_WS_OP_PING, data, len, false, &frame, &frame_len) != 0) {
        return -1;
    }
    
    if (conn->tcp) {
        return vox_tcp_write(conn->tcp, frame, frame_len, NULL);
    } else {
        return vox_tls_write(conn->tls, frame, frame_len, NULL);
    }
}

/* 关闭连接 */
int vox_ws_connection_close(vox_ws_connection_t* conn, uint16_t code, const char* reason) {
    if (!conn) return -1;
    if (conn->close_sent) return 0;
    
    conn->close_sent = true;
    conn->state = VOX_WS_CONN_CLOSING;
    
    void* frame;
    size_t frame_len;
    if (vox_ws_build_close_frame(conn->mpool, code, reason, false, &frame, &frame_len) != 0) {
        return -1;
    }
    
    if (conn->tcp) {
        return vox_tcp_write(conn->tcp, frame, frame_len, NULL);
    } else {
        return vox_tls_write(conn->tls, frame, frame_len, NULL);
    }
}

/* 获取用户数据 */
void* vox_ws_connection_get_user_data(vox_ws_connection_t* conn) {
    return conn ? conn->user_data : NULL;
}

/* 设置用户数据 */
void vox_ws_connection_set_user_data(vox_ws_connection_t* conn, void* user_data) {
    if (conn) {
        conn->user_data = user_data;
    }
}

/* 获取对端地址 */
int vox_ws_connection_getpeername(vox_ws_connection_t* conn, vox_socket_addr_t* addr) {
    if (!conn || !addr) return -1;
    
    if (conn->tcp) {
        return vox_tcp_getpeername(conn->tcp, addr);
    } else if (conn->tls) {
        return vox_tls_getpeername(conn->tls, addr);
    }
    
    return -1;
}
