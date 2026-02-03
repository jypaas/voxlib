/*
 * vox_websocket_client.c - WebSocket 客户端实现
 */

#include "vox_websocket_client.h"
#include "../vox_crypto.h"
#include "../vox_log.h"
#include "../vox_dns.h"
#include <string.h>
#include <stdio.h>

/* WebSocket 客户端结构 */
struct vox_ws_client {
    vox_loop_t* loop;                    /* 事件循环 */
    vox_mpool_t* mpool;                  /* 内存池（独立创建） */
    vox_tcp_t* tcp;                      /* TCP 连接（WS） */
    vox_tls_t* tls;                      /* TLS 连接（WSS） */
    vox_ws_parser_t* parser;             /* 帧解析器 */
    vox_ws_client_state_t state;         /* 客户端状态 */
    vox_ws_client_config_t config;       /* 配置 */
    vox_string_t* handshake_buffer;      /* 握手缓冲区 */
    char* ws_key;                        /* WebSocket Key */
    char* expected_accept;               /* 期望的 Accept */
    bool close_sent;                     /* 是否已发送关闭帧 */
    void* user_data;                     /* 用户数据 */
    bool owns_mpool;                     /* 是否拥有内存池 */
};

/* WebSocket GUID */
static const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* 前向声明 */
static void ws_client_on_dns_resolved(int status, const vox_dns_addrinfo_t* addrinfo, void* user_data);
static void ws_client_on_tcp_connect(vox_tcp_t* tcp, int status, void* user_data);
static void ws_client_on_tls_connect(vox_tls_t* tls, int status, void* user_data);
static void ws_client_on_tcp_read(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data);
static void ws_client_on_tls_read(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data);
static int ws_client_send_handshake(vox_ws_client_t* client);
static int ws_client_handle_handshake_response(vox_ws_client_t* client, const char* data, size_t len);
static int ws_client_handle_frame(vox_ws_client_t* client, const vox_ws_frame_t* frame, int frame_len);

/* 解析 URL */
static int ws_parse_url(vox_mpool_t* mpool, const char* url, char** host, char** path, uint16_t* port, bool* use_ssl) {
    if (!mpool || !url || !host || !path || !port || !use_ssl) return -1;
    
    *host = NULL;
    *path = NULL;
    *port = 0;
    *use_ssl = false;
    
    /* 检查协议 */
    const char* p = url;
    if (strncmp(p, "ws://", 5) == 0) {
        *use_ssl = false;
        *port = 80;
        p += 5;
    } else if (strncmp(p, "wss://", 6) == 0) {
        *use_ssl = true;
        *port = 443;
        p += 6;
    } else {
        return -1;
    }
    
    /* 解析主机名和端口 */
    const char* path_start = strchr(p, '/');
    const char* port_start = strchr(p, ':');
    
    size_t host_len;
    if (port_start && (!path_start || port_start < path_start)) {
        host_len = port_start - p;
        *port = (uint16_t)atoi(port_start + 1);
    } else if (path_start) {
        host_len = path_start - p;
    } else {
        host_len = strlen(p);
    }
    
    *host = (char*)vox_mpool_alloc(mpool, host_len + 1);
    if (!*host) return -1;
    memcpy(*host, p, host_len);
    (*host)[host_len] = '\0';
    
    /* 解析路径 */
    size_t path_len;
    if (path_start) {
        path_len = strlen(path_start);
        *path = (char*)vox_mpool_alloc(mpool, path_len + 1);
        if (!*path) {
            vox_mpool_free(mpool, *host);
            *host = NULL;
            return -1;
        }
        memcpy(*path, path_start, path_len + 1);
    } else {
        *path = (char*)vox_mpool_alloc(mpool, 2);
        if (!*path) {
            vox_mpool_free(mpool, *host);
            *host = NULL;
            return -1;
        }
        strcpy(*path, "/");
    }
    
    return 0;
}

/* 创建客户端 */
vox_ws_client_t* vox_ws_client_create(const vox_ws_client_config_t* config) {
    if (!config || !config->loop) return NULL;
    
    /* 创建独立的内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) return NULL;
    
    vox_ws_client_t* client = (vox_ws_client_t*)vox_mpool_alloc(mpool, sizeof(vox_ws_client_t));
    if (!client) {
        vox_mpool_destroy(mpool);
        return NULL;
    }
    
    memset(client, 0, sizeof(vox_ws_client_t));
    client->loop = config->loop;
    client->mpool = mpool;
    client->config = *config;
    client->state = VOX_WS_CLIENT_CLOSED;
    client->owns_mpool = true;
    
    /* 解析 URL */
    if (config->url) {
        char* host = NULL;
        char* path = NULL;
        uint16_t port = 0;
        bool use_ssl = false;
        
        if (ws_parse_url(mpool, config->url, &host, &path, &port, &use_ssl) == 0) {
            client->config.host = host;
            client->config.path = path;
            client->config.port = port;
            client->config.use_ssl = use_ssl;
        } else {
            vox_mpool_destroy(mpool);
            return NULL;
        }
    }
    
    /* 创建解析器 */
    client->parser = vox_ws_parser_create(client->mpool);
    if (!client->parser) {
        vox_mpool_destroy(mpool);
        return NULL;
    }
    
    /* 创建握手缓冲区 */
    client->handshake_buffer = vox_string_create(client->mpool);
    if (!client->handshake_buffer) {
        vox_ws_parser_destroy(client->parser);
        vox_mpool_destroy(mpool);
        return NULL;
    }
    
    return client;
}

/* 销毁客户端 */
void vox_ws_client_destroy(vox_ws_client_t* client) {
    if (!client) return;
    
    if (client->tcp) {
        vox_tcp_destroy(client->tcp);
        client->tcp = NULL;
    }
    
    if (client->tls) {
        vox_tls_destroy(client->tls);
        client->tls = NULL;
    }
    
    if (client->parser) {
        vox_ws_parser_destroy(client->parser);
    }
    
    if (client->handshake_buffer) {
        vox_string_destroy(client->handshake_buffer);
    }
    
    /* 销毁内存池 */
    if (client->owns_mpool && client->mpool) {
        vox_mpool_destroy(client->mpool);
    }
}

/* 连接到服务器 */
int vox_ws_client_connect(vox_ws_client_t* client) {
    if (!client || !client->config.host) return -1;
    if (client->state != VOX_WS_CLIENT_CLOSED) return -1;
    
    client->state = VOX_WS_CLIENT_CONNECTING;
    
    /* 尝试直接解析 IP 地址 */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(client->config.host, client->config.port, &addr) == 0) {
        /* 是有效的 IP 地址，直接连接 */
        ws_client_on_dns_resolved(0, NULL, client);
        return 0;
    }
    
    /* 需要 DNS 解析 */
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", client->config.port);
    
    if (vox_dns_getaddrinfo_simple(client->loop, client->config.host, port_str, 
                                    0, ws_client_on_dns_resolved, client, 5000) != 0) {
        if (client->config.on_error) {
            client->config.on_error(client, "Failed to start DNS resolution", client->config.user_data);
        }
        client->state = VOX_WS_CLIENT_CLOSED;
        return -1;
    }
    
    return 0;
}

/* 生成 WebSocket Key */
static char* ws_generate_key(vox_mpool_t* mpool) {
    if (!mpool) return NULL;
    
    uint8_t random_bytes[16];
    if (vox_crypto_random_bytes(random_bytes, sizeof(random_bytes)) != 0) {
        return NULL;
    }
    
    char* key = (char*)vox_mpool_alloc(mpool, 32);
    if (!key) return NULL;
    
    int len = vox_base64_encode(random_bytes, sizeof(random_bytes), key, 32);
    if (len <= 0) {
        vox_mpool_free(mpool, key);
        return NULL;
    }
    key[len] = '\0';
    
    return key;
}

/* 计算期望的 Accept */
static char* ws_calculate_accept(vox_mpool_t* mpool, const char* key) {
    if (!mpool || !key) return NULL;
    
    size_t key_len = strlen(key);
    size_t guid_len = strlen(WS_GUID);
    char* concat = (char*)vox_mpool_alloc(mpool, key_len + guid_len);
    if (!concat) return NULL;
    
    memcpy(concat, key, key_len);
    memcpy(concat + key_len, WS_GUID, guid_len);
    
    uint8_t digest[VOX_SHA1_DIGEST_SIZE];
    vox_sha1(concat, key_len + guid_len, digest);
    vox_mpool_free(mpool, concat);
    
    char* accept = (char*)vox_mpool_alloc(mpool, 32);
    if (!accept) return NULL;
    
    int len = vox_base64_encode(digest, sizeof(digest), accept, 32);
    if (len <= 0) {
        vox_mpool_free(mpool, accept);
        return NULL;
    }
    accept[len] = '\0';
    
    return accept;
}

/* 发送握手 */
static int ws_client_send_handshake(vox_ws_client_t* client) {
    if (!client) return -1;
    
    /* 生成 Key */
    client->ws_key = ws_generate_key(client->mpool);
    if (!client->ws_key) return -1;
    
    client->expected_accept = ws_calculate_accept(client->mpool, client->ws_key);
    if (!client->expected_accept) return -1;
    
    /* 构建握手请求 */
    vox_string_t* request = vox_string_create(client->mpool);
    if (!request) return -1;
    
    vox_string_append_format(request, "GET %s HTTP/1.1\r\n", 
                            client->config.path ? client->config.path : "/");
    vox_string_append_format(request, "Host: %s\r\n", client->config.host);
    vox_string_append(request, "Upgrade: websocket\r\n");
    vox_string_append(request, "Connection: Upgrade\r\n");
    vox_string_append_format(request, "Sec-WebSocket-Key: %s\r\n", client->ws_key);
    vox_string_append(request, "Sec-WebSocket-Version: 13\r\n");
    vox_string_append(request, "\r\n");
    
    const char* req_data = vox_string_cstr(request);
    size_t req_len = vox_string_length(request);
    
    int ret;
    if (client->tcp) {
        ret = vox_tcp_write(client->tcp, req_data, req_len, NULL);
    } else {
        ret = vox_tls_write(client->tls, req_data, req_len, NULL);
    }
    
    vox_string_destroy(request);
    
    if (ret == 0) {
        client->state = VOX_WS_CLIENT_HANDSHAKING;
    }
    
    return ret;
}

/* 处理握手响应 */
static int ws_client_handle_handshake_response(vox_ws_client_t* client, const char* data, size_t len) {
    if (!client || !data || len == 0) return -1;
    
    /* 追加到握手缓冲区 */
    vox_string_append_data(client->handshake_buffer, data, len);
    
    /* 查找握手结束标记 */
    const char* buf = vox_string_cstr(client->handshake_buffer);
    size_t buf_len = vox_string_length(client->handshake_buffer);
    const char* end_marker = strstr(buf, "\r\n\r\n");
    
    if (!end_marker) {
        /* 握手未完成 */
        if (buf_len > 8192) {
            /* 握手数据过大 */
            return -1;
        }
        return 0;
    }
    
    /* 检查状态行 */
    if (strncmp(buf, "HTTP/1.1 101", 12) != 0) {
        return -1;
    }
    
    /* 查找 Sec-WebSocket-Accept */
    const char* accept_header = strstr(buf, "Sec-WebSocket-Accept:");
    if (!accept_header) return -1;
    
    accept_header += 21; /* strlen("Sec-WebSocket-Accept:") */
    while (*accept_header == ' ' || *accept_header == '\t') {
        accept_header++;
    }
    
    const char* accept_end = accept_header;
    while (*accept_end != '\r' && *accept_end != '\n' && *accept_end != '\0') {
        accept_end++;
    }
    
    size_t accept_len = accept_end - accept_header;
    char* received_accept = (char*)vox_mpool_alloc(client->mpool, accept_len + 1);
    if (!received_accept) return -1;
    memcpy(received_accept, accept_header, accept_len);
    received_accept[accept_len] = '\0';
    
    /* 验证 Accept */
    int valid = (strcmp(received_accept, client->expected_accept) == 0);
    /* 注意：received_accept 来自内存池，不需要 free */
    
    if (!valid) return -1;
    
    /* 握手成功 */
    client->state = VOX_WS_CLIENT_OPEN;
    vox_string_destroy(client->handshake_buffer);
    client->handshake_buffer = NULL;
    
    /* 调用连接回调 */
    if (client->config.on_connect) {
        client->config.on_connect(client, client->config.user_data);
    }
    
    return 1;
}

/* 处理帧 */
static int ws_client_handle_frame(vox_ws_client_t* client, const vox_ws_frame_t* frame, int frame_len) {
    if (!client || !frame) return -1;
    
    /* 客户端接收的帧不应该有掩码 */
    if (frame->masked) {
        if (client->config.on_error) {
            client->config.on_error(client, "Received masked frame from server", client->config.user_data);
        }
        return -1;
    }
    
    /* 复制负载 */
    uint8_t* payload = NULL;
    if (frame->payload_len > 0) {
        payload = (uint8_t*)vox_mpool_alloc(client->mpool, frame->payload_len);
        if (!payload) return -1;
        memcpy(payload, frame->payload, frame->payload_len);
    }
    
    /* 处理不同类型的帧 */
    if (frame->opcode == VOX_WS_OP_TEXT) {
        /* 文本消息 */
        if (!vox_ws_validate_utf8(payload, frame->payload_len)) {
            vox_ws_client_close(client, VOX_WS_CLOSE_INVALID_DATA, "Invalid UTF-8");
            return -1;
        }
        
        if (client->config.on_message) {
            client->config.on_message(client, payload, frame->payload_len,
                                     VOX_WS_MSG_TEXT, client->config.user_data);
        }
    } else if (frame->opcode == VOX_WS_OP_BINARY) {
        /* 二进制消息 */
        if (client->config.on_message) {
            client->config.on_message(client, payload, frame->payload_len,
                                     VOX_WS_MSG_BINARY, client->config.user_data);
        }
    } else if (frame->opcode == VOX_WS_OP_CLOSE) {
        /* 关闭帧 */
        uint16_t code = VOX_WS_CLOSE_NORMAL;
        const char* reason = "";
        char* reason_buf = NULL;
        
        /* RFC 6455: Close payload 必须是 0 字节或至少 2 字节（状态码） */
        if (frame->payload_len == 1) {
            vox_ws_client_close(client, VOX_WS_CLOSE_PROTOCOL_ERROR, "Invalid close frame");
            return -1;
        }
        
        if (frame->payload_len >= 2) {
            code = ((uint16_t)payload[0] << 8) | (uint16_t)payload[1];
            
            /* RFC 6455: 验证状态码范围和保留码 */
            if (code < 1000 || code > 4999 || 
                code == 1004 || code == 1005 || code == 1006 || code == 1015) {
                vox_ws_client_close(client, VOX_WS_CLOSE_PROTOCOL_ERROR, "Invalid close code");
                return -1;
            }
            
            if (frame->payload_len > 2) {
                size_t reason_len = frame->payload_len - 2;
                
                /* RFC 6455: 验证 reason 的 UTF-8 编码 */
                if (!vox_ws_validate_utf8(payload + 2, reason_len)) {
                    vox_ws_client_close(client, VOX_WS_CLOSE_INVALID_DATA, "Invalid UTF-8 in close reason");
                    return -1;
                }
                
                reason_buf = (char*)vox_mpool_alloc(client->mpool, reason_len + 1);
                if (reason_buf) {
                    memcpy(reason_buf, payload + 2, reason_len);
                    reason_buf[reason_len] = '\0';
                    reason = reason_buf;
                }
            }
        }
        
        if (client->config.on_close) {
            client->config.on_close(client, code, reason, client->config.user_data);
        }
        
        /* 回复关闭帧 */
        if (!client->close_sent) {
            vox_ws_client_close(client, code, reason);
        }
        
        client->state = VOX_WS_CLIENT_CLOSED;
        return -1;
    } else if (frame->opcode == VOX_WS_OP_PING) {
        /* Ping -> 回复 Pong */
        void* pong_frame;
        size_t pong_len;
        if (vox_ws_build_frame(client->mpool, VOX_WS_OP_PONG, payload,
                              frame->payload_len, true, &pong_frame, &pong_len) == 0) {
            if (client->tcp) {
                vox_tcp_write(client->tcp, pong_frame, pong_len, NULL);
            } else {
                vox_tls_write(client->tls, pong_frame, pong_len, NULL);
            }
        }
    }
    /* PONG 帧忽略 */
    
    return frame_len;
}

/* DNS 解析完成回调 */
static void ws_client_on_dns_resolved(int status, const vox_dns_addrinfo_t* addrinfo, void* user_data) {
    vox_ws_client_t* client = (vox_ws_client_t*)user_data;
    if (!client) return;
    
    vox_socket_addr_t addr;
    
    /* 检查解析状态 */
    if (status != 0 || !addrinfo || addrinfo->count == 0) {
        /* DNS 解析失败，尝试直接解析 */
        if (vox_socket_parse_address(client->config.host, client->config.port, &addr) != 0) {
            if (client->config.on_error) {
                client->config.on_error(client, "Failed to resolve host", client->config.user_data);
            }
            client->state = VOX_WS_CLIENT_CLOSED;
            return;
        }
    } else {
        /* 使用第一个解析的地址 */
        addr = addrinfo->addrs[0];
    }
    
    /* 创建连接 */
    if (client->config.use_ssl) {
        /* WSS 连接 */
        vox_ssl_context_t* ssl_ctx = client->config.ssl_ctx;
        if (!ssl_ctx) {
            ssl_ctx = vox_ssl_context_create(client->mpool, VOX_SSL_MODE_CLIENT);
            if (!ssl_ctx) {
                if (client->config.on_error) {
                    client->config.on_error(client, "Failed to create SSL context", client->config.user_data);
                }
                client->state = VOX_WS_CLIENT_CLOSED;
                return;
            }
        }
        
        client->tls = vox_tls_create(client->loop, ssl_ctx);
        if (!client->tls) {
            if (client->config.on_error) {
                client->config.on_error(client, "Failed to create TLS connection", client->config.user_data);
            }
            client->state = VOX_WS_CLIENT_CLOSED;
            return;
        }
        
        client->tls->handle.data = client;
        vox_tls_nodelay(client->tls, true);
        
        if (vox_tls_connect(client->tls, &addr, ws_client_on_tls_connect) != 0) {
            if (client->config.on_error) {
                client->config.on_error(client, "Failed to start TLS connection", client->config.user_data);
            }
            vox_tls_destroy(client->tls);
            client->tls = NULL;
            client->state = VOX_WS_CLIENT_CLOSED;
        }
    } else {
        /* WS 连接 */
        client->tcp = vox_tcp_create(client->loop);
        if (!client->tcp) {
            if (client->config.on_error) {
                client->config.on_error(client, "Failed to create TCP connection", client->config.user_data);
            }
            client->state = VOX_WS_CLIENT_CLOSED;
            return;
        }
        
        client->tcp->handle.data = client;
        vox_tcp_nodelay(client->tcp, true);
        
        if (vox_tcp_connect(client->tcp, &addr, ws_client_on_tcp_connect) != 0) {
            if (client->config.on_error) {
                client->config.on_error(client, "Failed to start TCP connection", client->config.user_data);
            }
            vox_tcp_destroy(client->tcp);
            client->tcp = NULL;
            client->state = VOX_WS_CLIENT_CLOSED;
        }
    }
}

/* TCP 连接回调 */
static void ws_client_on_tcp_connect(vox_tcp_t* tcp, int status, void* user_data) {
    (void)user_data;
    
    vox_ws_client_t* client = (vox_ws_client_t*)tcp->handle.data;
    if (!client) return;
    
    if (status != 0) {
        if (client->config.on_error) {
            client->config.on_error(client, "Connection failed", client->config.user_data);
        }
        client->state = VOX_WS_CLIENT_CLOSED;
        return;
    }
    
    /* 发送握手 */
    if (ws_client_send_handshake(client) != 0) {
        if (client->config.on_error) {
            client->config.on_error(client, "Failed to send handshake", client->config.user_data);
        }
        vox_tcp_destroy(client->tcp);
        client->tcp = NULL;
        client->state = VOX_WS_CLIENT_CLOSED;
        return;
    }
    
    /* 开始读取 */
    vox_tcp_read_start(client->tcp, NULL, ws_client_on_tcp_read);
}

/* TLS 连接回调 */
static void ws_client_on_tls_connect(vox_tls_t* tls, int status, void* user_data) {
    (void)user_data;
    
    vox_ws_client_t* client = (vox_ws_client_t*)tls->handle.data;
    if (!client) return;
    
    if (status != 0) {
        if (client->config.on_error) {
            client->config.on_error(client, "Connection failed", client->config.user_data);
        }
        client->state = VOX_WS_CLIENT_CLOSED;
        return;
    }
    
    /* 发送握手 */
    if (ws_client_send_handshake(client) != 0) {
        if (client->config.on_error) {
            client->config.on_error(client, "Failed to send handshake", client->config.user_data);
        }
        vox_tls_destroy(client->tls);
        client->tls = NULL;
        client->state = VOX_WS_CLIENT_CLOSED;
        return;
    }
    
    /* 开始读取 */
    vox_tls_read_start(client->tls, NULL, ws_client_on_tls_read);
}

/* TCP 读取回调 */
static void ws_client_on_tcp_read(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    (void)user_data;
    
    vox_ws_client_t* client = (vox_ws_client_t*)tcp->handle.data;
    if (!client) return;
    
    if (nread < 0) {
        if (client->config.on_error) {
            client->config.on_error(client, "Connection closed", client->config.user_data);
        }
        vox_tcp_destroy(client->tcp);
        client->tcp = NULL;
        client->state = VOX_WS_CLIENT_CLOSED;
        return;
    }
    
    if (nread == 0) return;
    
    /* 处理握手响应 */
    if (client->state == VOX_WS_CLIENT_HANDSHAKING) {
        int ret = ws_client_handle_handshake_response(client, (const char*)buf, (size_t)nread);
        if (ret < 0) {
            if (client->config.on_error) {
                client->config.on_error(client, "Handshake failed", client->config.user_data);
            }
            vox_tcp_destroy(client->tcp);
            client->tcp = NULL;
            client->state = VOX_WS_CLIENT_CLOSED;
        }
        return;
    }
    
    /* 解析帧 */
    if (vox_ws_parser_feed(client->parser, buf, (size_t)nread) != 0) {
        vox_tcp_destroy(client->tcp);
        client->tcp = NULL;
        client->state = VOX_WS_CLIENT_CLOSED;
        return;
    }
    
    /* 处理所有完整的帧 */
    vox_ws_frame_t frame;
    int frame_len;
    while ((frame_len = vox_ws_parser_parse_frame(client->parser, &frame)) > 0) {
        if (ws_client_handle_frame(client, &frame, frame_len) < 0) {
            vox_tcp_destroy(client->tcp);
            client->tcp = NULL;
            client->state = VOX_WS_CLIENT_CLOSED;
            return;
        }
        
        /* 从缓冲区移除已处理的帧 */
        vox_string_remove(client->parser->buffer, 0, frame_len);
    }
    
    if (frame_len < 0) {
        vox_tcp_destroy(client->tcp);
        client->tcp = NULL;
        client->state = VOX_WS_CLIENT_CLOSED;
    }
}

/* TLS 读取回调 */
static void ws_client_on_tls_read(vox_tls_t* tls, ssize_t nread, const void* buf, void* user_data) {
    (void)user_data;
    
    vox_ws_client_t* client = (vox_ws_client_t*)tls->handle.data;
    if (!client) return;
    
    if (nread < 0) {
        if (client->config.on_error) {
            client->config.on_error(client, "Connection closed", client->config.user_data);
        }
        vox_tls_destroy(client->tls);
        client->tls = NULL;
        client->state = VOX_WS_CLIENT_CLOSED;
        return;
    }
    
    if (nread == 0) return;
    
    /* 处理握手响应 */
    if (client->state == VOX_WS_CLIENT_HANDSHAKING) {
        int ret = ws_client_handle_handshake_response(client, (const char*)buf, (size_t)nread);
        if (ret < 0) {
            if (client->config.on_error) {
                client->config.on_error(client, "Handshake failed", client->config.user_data);
            }
            vox_tls_destroy(client->tls);
            client->tls = NULL;
            client->state = VOX_WS_CLIENT_CLOSED;
        }
        return;
    }
    
    /* 解析帧 */
    if (vox_ws_parser_feed(client->parser, buf, (size_t)nread) != 0) {
        vox_tls_destroy(client->tls);
        client->tls = NULL;
        client->state = VOX_WS_CLIENT_CLOSED;
        return;
    }
    
    /* 处理所有完整的帧 */
    vox_ws_frame_t frame;
    int frame_len;
    while ((frame_len = vox_ws_parser_parse_frame(client->parser, &frame)) > 0) {
        if (ws_client_handle_frame(client, &frame, frame_len) < 0) {
            vox_tls_destroy(client->tls);
            client->tls = NULL;
            client->state = VOX_WS_CLIENT_CLOSED;
            return;
        }
        
        /* 从缓冲区移除已处理的帧 */
        vox_string_remove(client->parser->buffer, 0, frame_len);
    }
    
    if (frame_len < 0) {
        vox_tls_destroy(client->tls);
        client->tls = NULL;
        client->state = VOX_WS_CLIENT_CLOSED;
    }
}

/* 发送文本消息 */
int vox_ws_client_send_text(vox_ws_client_t* client, const char* text, size_t len) {
    if (!client || !text || len == 0) return -1;
    if (client->state != VOX_WS_CLIENT_OPEN) return -1;
    
    void* frame;
    size_t frame_len;
    if (vox_ws_build_frame(client->mpool, VOX_WS_OP_TEXT, text, len, true, &frame, &frame_len) != 0) {
        return -1;
    }
    
    if (client->tcp) {
        return vox_tcp_write(client->tcp, frame, frame_len, NULL);
    } else {
        return vox_tls_write(client->tls, frame, frame_len, NULL);
    }
}

/* 发送二进制消息 */
int vox_ws_client_send_binary(vox_ws_client_t* client, const void* data, size_t len) {
    if (!client || !data || len == 0) return -1;
    if (client->state != VOX_WS_CLIENT_OPEN) return -1;
    
    void* frame;
    size_t frame_len;
    if (vox_ws_build_frame(client->mpool, VOX_WS_OP_BINARY, data, len, true, &frame, &frame_len) != 0) {
        return -1;
    }
    
    if (client->tcp) {
        return vox_tcp_write(client->tcp, frame, frame_len, NULL);
    } else {
        return vox_tls_write(client->tls, frame, frame_len, NULL);
    }
}

/* 发送 Ping */
int vox_ws_client_send_ping(vox_ws_client_t* client, const void* data, size_t len) {
    if (!client) return -1;
    if (client->state != VOX_WS_CLIENT_OPEN) return -1;
    
    void* frame;
    size_t frame_len;
    if (vox_ws_build_frame(client->mpool, VOX_WS_OP_PING, data, len, true, &frame, &frame_len) != 0) {
        return -1;
    }
    
    if (client->tcp) {
        return vox_tcp_write(client->tcp, frame, frame_len, NULL);
    } else {
        return vox_tls_write(client->tls, frame, frame_len, NULL);
    }
}

/* 关闭连接 */
int vox_ws_client_close(vox_ws_client_t* client, uint16_t code, const char* reason) {
    if (!client) return -1;
    if (client->close_sent) return 0;
    
    client->close_sent = true;
    client->state = VOX_WS_CLIENT_CLOSING;
    
    void* frame;
    size_t frame_len;
    if (vox_ws_build_close_frame(client->mpool, code, reason, true, &frame, &frame_len) != 0) {
        return -1;
    }
    
    if (client->tcp) {
        return vox_tcp_write(client->tcp, frame, frame_len, NULL);
    } else {
        return vox_tls_write(client->tls, frame, frame_len, NULL);
    }
}

/* 获取状态 */
vox_ws_client_state_t vox_ws_client_get_state(const vox_ws_client_t* client) {
    return client ? client->state : VOX_WS_CLIENT_CLOSED;
}

/* 获取用户数据 */
void* vox_ws_client_get_user_data(vox_ws_client_t* client) {
    return client ? client->user_data : NULL;
}

/* 设置用户数据 */
void vox_ws_client_set_user_data(vox_ws_client_t* client, void* user_data) {
    if (client) {
        client->user_data = user_data;
    }
}
