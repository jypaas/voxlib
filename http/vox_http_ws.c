/*
 * vox_http_ws.c - WebSocket 实现（复用 websocket 模块核心代码）
 */

#include "vox_http_ws.h"
#include "vox_http_internal.h"
#include "../vox_crypto.h"
#include "../vox_log.h"
#include "../websocket/vox_websocket.h"
#include <string.h>
#include <stdint.h>

struct vox_http_ws_conn {
    vox_mpool_t* mpool;
    void* conn; /* vox_http_conn_t* */

    vox_http_ws_callbacks_t cbs;

    vox_ws_parser_t* parser;  /* 复用 WebSocket 解析器 */
    vox_string_t* frag;       /* 分片重组缓存 */
    bool frag_active;
    bool frag_is_text;

    bool close_sent;
};

vox_http_ws_conn_t* vox_http_ws_internal_create(vox_mpool_t* mpool, void* conn, const vox_http_ws_callbacks_t* cbs) {
    if (!mpool) return NULL;
    vox_http_ws_conn_t* ws = (vox_http_ws_conn_t*)vox_mpool_alloc(mpool, sizeof(vox_http_ws_conn_t));
    if (!ws) return NULL;
    memset(ws, 0, sizeof(*ws));
    ws->mpool = mpool;
    ws->conn = conn;
    if (cbs) ws->cbs = *cbs;
    
    /* 使用 WebSocket 模块的解析器 */
    ws->parser = vox_ws_parser_create(mpool);
    ws->frag = vox_string_create(mpool);
    if (!ws->parser || !ws->frag) return NULL;
    
    return ws;
}

static vox_strview_t vox_http_trim_ows(vox_strview_t v) {
    /* OWS: optional whitespace */
    while (v.len > 0 && (v.ptr[0] == ' ' || v.ptr[0] == '\t')) {
        v.ptr++;
        v.len--;
    }
    while (v.len > 0 && (v.ptr[v.len - 1] == ' ' || v.ptr[v.len - 1] == '\t')) {
        v.len--;
    }
    return v;
}

static vox_strview_t vox_http_req_get_header_ci(const vox_http_request_t* req, const char* name) {
    if (!req || !name) return (vox_strview_t)VOX_STRVIEW_NULL;
    vox_vector_t* headers = (vox_vector_t*)req->headers;
    if (!headers) return (vox_strview_t)VOX_STRVIEW_NULL;
    size_t nlen = strlen(name);
    size_t cnt = vox_vector_size(headers);
    for (size_t i = 0; i < cnt; i++) {
        const vox_http_header_t* kv = (const vox_http_header_t*)vox_vector_get(headers, i);
        if (!kv || !kv->name.ptr) continue;
        if (vox_http_strieq(kv->name.ptr, kv->name.len, name, nlen)) return kv->value;
    }
    return (vox_strview_t)VOX_STRVIEW_NULL;
}

static int vox_http_sv_contains_token_ci(vox_strview_t v, const char* tok) {
    if (!v.ptr || v.len == 0 || !tok || !*tok) return 0;
    size_t tlen = strlen(tok);
    if (tlen == 0 || tlen > v.len) return 0;
    for (size_t i = 0; i + tlen <= v.len; i++) {
        if (strncasecmp(v.ptr + i, tok, tlen) == 0) return 1;
    }
    return 0;
}

static void vox_http_ws_report_error(vox_http_ws_conn_t* ws, const char* msg) {
    if (ws && ws->cbs.on_error) {
        ws->cbs.on_error(ws, msg ? msg : "ws error", ws->cbs.user_data);
    }
}

static bool vox_http_ws_is_valid_close_code(int code) {
    /* RFC 6455 Section 7.4.1: 验证关闭状态码 */
    if (code < 1000 || code > 4999) return false;
    
    /* 保留的状态码（不能在 Close 帧中发送） */
    if (code == 1004 || code == 1005 || code == 1006 || code == 1015) {
        return false;
    }
    
    /* 1000-2999: 协议定义的 */
    /* 3000-3999: 库/框架保留的 */
    /* 4000-4999: 应用保留的 */
    return true;
}

static int vox_http_ws_send_frame(vox_http_ws_conn_t* ws, uint8_t opcode, const void* data, size_t len) {
    if (!ws) return -1;
    if (len > 0 && !data) return -1;

    /* 复用 WebSocket 模块的帧构建函数，服务器侧不使用掩码 */
    void* frame = NULL;
    size_t frame_len = 0;
    
    if (vox_ws_build_frame(ws->mpool, opcode, data, len, false, &frame, &frame_len) != 0) {
        return -1;
    }
    
    return vox_http_conn_ws_write(ws->conn, frame, frame_len);
}

static int vox_http_ws_send_close_frame(vox_http_ws_conn_t* ws, int code, const char* reason) {
    if (!ws) return -1;
    if (ws->close_sent) return 0;
    ws->close_sent = true;

    /* 复用 WebSocket 模块的关闭帧构建函数 */
    void* frame = NULL;
    size_t frame_len = 0;
    
    if (vox_ws_build_close_frame(ws->mpool, (uint16_t)code, reason, false, &frame, &frame_len) != 0) {
        return -1;
    }
    
    return vox_http_conn_ws_write(ws->conn, frame, frame_len);
}

int vox_http_ws_upgrade(vox_http_context_t* ctx, const vox_http_ws_callbacks_t* cbs) {
    if (!ctx) return -1;
    const vox_http_request_t* req = vox_http_context_request(ctx);
    if (!req) return -1;

    /* 检查 WebSocket 升级所需的头 */
    vox_strview_t conn = vox_http_trim_ows(vox_http_req_get_header_ci(req, "Connection"));
    vox_strview_t upg = vox_http_trim_ows(vox_http_req_get_header_ci(req, "Upgrade"));
    if (!conn.ptr || !upg.ptr) return -1;
    if (!vox_http_sv_contains_token_ci(conn, "upgrade")) return -1;
    if (!vox_http_sv_contains_token_ci(upg, "websocket")) return -1;

    vox_strview_t key = vox_http_req_get_header_ci(req, "Sec-WebSocket-Key");
    vox_strview_t ver = vox_http_req_get_header_ci(req, "Sec-WebSocket-Version");
    key = vox_http_trim_ows(key);
    ver = vox_http_trim_ows(ver);
    if (!key.ptr || key.len == 0) return -1;
    if (!ver.ptr || ver.len == 0) return -1;
    if (!(ver.len == 2 && ver.ptr[0] == '1' && ver.ptr[1] == '3')) return -1;

    /* 如果 parser 没有设置 is_upgrade 标志，但所有必要的头都存在，仍然允许升级 */
    /* 这修复了某些情况下 parser 未能正确识别 upgrade 请求的问题 */

    /* 创建 ws conn（生命周期跟随连接 mpool） */
    vox_http_ws_conn_t* ws = vox_http_ws_internal_create(ctx->mpool, ctx->conn, cbs);
    if (!ws) return -1;

    /* Sec-WebSocket-Accept = base64(sha1(key + GUID)) */
    static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    size_t guid_len = strlen(kGuid);
    size_t concat_len = key.len + guid_len;
    char* concat = (char*)vox_mpool_alloc(ctx->mpool, concat_len);
    if (!concat) return -1;
    memcpy(concat, key.ptr, key.len);
    memcpy(concat + key.len, kGuid, guid_len);

    uint8_t digest[VOX_SHA1_DIGEST_SIZE];
    vox_sha1(concat, concat_len, digest);

    char accept[64];
    int alen = vox_base64_encode(digest, sizeof(digest), accept, sizeof(accept));
    if (alen <= 0) {
        vox_mpool_free(ctx->mpool, concat);
        return -1;
    }
    accept[alen] = '\0';
    char* accept_copy = (char*)vox_mpool_alloc(ctx->mpool, (size_t)alen + 1);
    if (!accept_copy) {
        vox_mpool_free(ctx->mpool, concat);
        return -1;
    }
    memcpy(accept_copy, accept, (size_t)alen + 1);
    vox_mpool_free(ctx->mpool, concat);

    /* 写 101 响应头（server 负责发送） */
    vox_http_context_status(ctx, 101);
    vox_http_context_header(ctx, "Upgrade", "websocket");
    vox_http_context_header(ctx, "Connection", "Upgrade");
    vox_http_context_header(ctx, "Sec-WebSocket-Accept", accept_copy);

    /* 标记 upgrade：write_done 后切换到 WS 模式 */
    if (ctx->conn) {
        if (vox_http_conn_mark_ws_upgrade(ctx->conn, ws) != 0) return -1;
    }

    /* 终止后续 handlers，避免再写普通响应 */
    vox_http_context_abort(ctx);
    return 0;
}

int vox_http_ws_send_text(vox_http_ws_conn_t* ws, const char* text, size_t len) {
    if (!ws) return -1;
    if (!text && len > 0) return -1;
    return vox_http_ws_send_frame(ws, VOX_WS_OP_TEXT, text, len);
}

int vox_http_ws_send_binary(vox_http_ws_conn_t* ws, const void* data, size_t len) {
    if (!ws) return -1;
    if (!data && len > 0) return -1;
    return vox_http_ws_send_frame(ws, VOX_WS_OP_BINARY, data, len);
}

int vox_http_ws_close(vox_http_ws_conn_t* ws, int code, const char* reason) {
    if (!ws) return -1;
    (void)vox_http_ws_send_close_frame(ws, code ? code : 1000, reason);
    vox_http_conn_ws_close(ws->conn);
    return 0;
}

void vox_http_ws_internal_on_open(vox_http_ws_conn_t* ws) {
    if (!ws) return;
    if (ws->cbs.on_connect) ws->cbs.on_connect(ws, ws->cbs.user_data);
}

static int vox_http_ws_deliver_message(vox_http_ws_conn_t* ws, const void* data, size_t len, bool is_text) {
    if (!ws) return -1;
    if (ws->cbs.on_message) {
        ws->cbs.on_message(ws, data, len, is_text, ws->cbs.user_data);
    }
    return 0;
}

int vox_http_ws_internal_feed(vox_http_ws_conn_t* ws, const void* data, size_t len) {
    if (!ws) return -1;
    if (!data || len == 0) return 0;
    
    /* 使用 WebSocket 解析器 */
    if (vox_ws_parser_feed(ws->parser, data, len) != 0) {
        return -1;
    }

    /* 循环处理所有完整的帧 */
    vox_ws_frame_t frame;
    int frame_len;
    
    while ((frame_len = vox_ws_parser_parse_frame(ws->parser, &frame)) > 0) {
        /* 验证客户端必须使用掩码 */
        if (!frame.masked) {
            vox_http_ws_report_error(ws, "ws protocol error: unmasked client frame");
            return -1;
        }
        
        /* 解掩码（需要创建副本因为 payload 指向解析器内部缓冲区）*/
        uint8_t* payload = NULL;
        if (frame.payload_len > 0) {
            payload = (uint8_t*)vox_mpool_alloc(ws->mpool, frame.payload_len);
            if (!payload) return -1;
            memcpy(payload, frame.payload, frame.payload_len);
            vox_ws_mask_payload(payload, frame.payload_len, frame.mask_key);
        }

        /* 处理不同类型的帧 */
        if (frame.opcode == VOX_WS_OP_CLOSE) {
            /* 关闭帧 */
            int code = VOX_WS_CLOSE_NORMAL;
            const char* reason = "";
            
            /* RFC 6455: Close payload 必须是 0 或 >=2 字节 */
            if (frame.payload_len == 1) {
                vox_http_ws_report_error(ws, "ws protocol error: invalid close payload length");
                vox_string_remove(ws->parser->buffer, 0, (size_t)frame_len);
                return -1;
            }
            
            if (frame.payload_len >= 2) {
                code = ((int)payload[0] << 8) | (int)payload[1];
                
                /* 验证状态码 */
                if (!vox_http_ws_is_valid_close_code(code)) {
                    vox_http_ws_report_error(ws, "ws protocol error: invalid close code");
                    vox_string_remove(ws->parser->buffer, 0, (size_t)frame_len);
                    return -1;
                }
                
                if (frame.payload_len > 2) {
                    size_t rlen = frame.payload_len - 2;
                    
                    /* 验证 Close reason 的 UTF-8 编码 */
                    if (!vox_ws_validate_utf8(payload + 2, rlen)) {
                        vox_http_ws_report_error(ws, "ws protocol error: invalid UTF-8 in close reason");
                        vox_string_remove(ws->parser->buffer, 0, (size_t)frame_len);
                        return -1;
                    }
                    
                    char* r = (char*)vox_mpool_alloc(ws->mpool, rlen + 1);
                    if (r) {
                        memcpy(r, payload + 2, rlen);
                        r[rlen] = '\0';
                        reason = r;
                    }
                }
            }
            
            if (ws->cbs.on_close) {
                ws->cbs.on_close(ws, code, reason, ws->cbs.user_data);
            }
            
            /* 回复关闭帧并关闭连接 */
            (void)vox_http_ws_send_close_frame(ws, code, reason);
            vox_http_conn_ws_close(ws->conn);
            
            /* 消费帧后返回 */
            vox_string_remove(ws->parser->buffer, 0, (size_t)frame_len);
            return -1;
            
        } else if (frame.opcode == VOX_WS_OP_PING) {
            /* Ping -> 回复 Pong */
            (void)vox_http_ws_send_frame(ws, VOX_WS_OP_PONG, payload, frame.payload_len);
            
        } else if (frame.opcode == VOX_WS_OP_PONG) {
            /* Pong - 忽略 */
            
        } else if (frame.opcode == VOX_WS_OP_CONTINUATION) {
            /* 继续帧 */
            if (!ws->frag_active) {
                vox_http_ws_report_error(ws, "ws protocol error: unexpected continuation");
                vox_string_remove(ws->parser->buffer, 0, (size_t)frame_len);
                return -1;
            }
            
            if (frame.payload_len > 0) {
                vox_string_append_data(ws->frag, payload, frame.payload_len);
            }
            
            if (frame.fin) {
                /* 分片完成，传递完整消息 */
                bool is_text = ws->frag_is_text;
                const void* msg = vox_string_data(ws->frag);
                size_t mlen = vox_string_length(ws->frag);
                
                /* UTF-8 验证 */
                if (is_text && !vox_ws_validate_utf8((const uint8_t*)msg, mlen)) {
                    vox_http_ws_report_error(ws, "ws protocol error: invalid UTF-8");
                    vox_string_remove(ws->parser->buffer, 0, (size_t)frame_len);
                    return -1;
                }
                
                ws->frag_active = false;
                vox_http_ws_deliver_message(ws, msg, mlen, is_text);
                vox_string_clear(ws->frag);
            }
            
        } else if (frame.opcode == VOX_WS_OP_TEXT || frame.opcode == VOX_WS_OP_BINARY) {
            /* 文本或二进制消息 */
            bool is_text = (frame.opcode == VOX_WS_OP_TEXT);
            
            if (!frame.fin) {
                /* 开始分片 */
                ws->frag_active = true;
                ws->frag_is_text = is_text;
                vox_string_clear(ws->frag);
                if (frame.payload_len > 0) {
                    vox_string_append_data(ws->frag, payload, frame.payload_len);
                }
            } else {
                /* 完整消息 */
                /* UTF-8 验证 */
                if (is_text && !vox_ws_validate_utf8(payload, frame.payload_len)) {
                    vox_http_ws_report_error(ws, "ws protocol error: invalid UTF-8");
                    vox_string_remove(ws->parser->buffer, 0, (size_t)frame_len);
                    return -1;
                }
                
                vox_http_ws_deliver_message(ws, payload, frame.payload_len, is_text);
            }
            
        } else {
            /* 未知操作码 */
            vox_http_ws_report_error(ws, "ws protocol error: unknown opcode");
            vox_string_remove(ws->parser->buffer, 0, (size_t)frame_len);
            return -1;
        }

        /* 消费已处理的帧 */
        vox_string_remove(ws->parser->buffer, 0, (size_t)frame_len);
    }
    
    /* frame_len < 0 表示协议错误 */
    if (frame_len < 0) {
        vox_http_ws_report_error(ws, "ws protocol error: invalid frame");
        return -1;
    }
    
    /* frame_len == 0 表示需要更多数据 */
    return 0;
}

