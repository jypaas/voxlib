/*
 * vox_websocket.c - WebSocket 协议核心实现
 */

#include "vox_websocket.h"
#include "../vox_crypto.h"
#include <string.h>
#include <stdlib.h>

/* 创建帧解析器 */
vox_ws_parser_t* vox_ws_parser_create(vox_mpool_t* mpool) {
    if (!mpool) return NULL;
    
    vox_ws_parser_t* parser = (vox_ws_parser_t*)vox_mpool_alloc(mpool, sizeof(vox_ws_parser_t));
    if (!parser) return NULL;
    
    memset(parser, 0, sizeof(vox_ws_parser_t));
    parser->mpool = mpool;
    parser->buffer = vox_string_create(mpool);
    parser->fragment = vox_string_create(mpool);
    
    if (!parser->buffer || !parser->fragment) return NULL;
    
    return parser;
}

/* 销毁帧解析器 */
void vox_ws_parser_destroy(vox_ws_parser_t* parser) {
    if (!parser) return;
    
    if (parser->buffer) {
        vox_string_destroy(parser->buffer);
    }
    if (parser->fragment) {
        vox_string_destroy(parser->fragment);
    }
}

/* 重置解析器 */
void vox_ws_parser_reset(vox_ws_parser_t* parser) {
    if (!parser) return;
    
    if (parser->buffer) {
        vox_string_clear(parser->buffer);
    }
    if (parser->fragment) {
        vox_string_clear(parser->fragment);
    }
    parser->in_fragment = false;
}

/* 输入数据 */
int vox_ws_parser_feed(vox_ws_parser_t* parser, const void* data, size_t len) {
    if (!parser || !data || len == 0) return -1;
    return vox_string_append_data(parser->buffer, data, len);
}

/* 解析帧 */
int vox_ws_parser_parse_frame(vox_ws_parser_t* parser, vox_ws_frame_t* frame) {
    if (!parser || !frame) return -1;
    
    size_t buf_len = vox_string_length(parser->buffer);
    if (buf_len < 2) return 0; /* 需要至少2字节 */
    
    const uint8_t* buf = (const uint8_t*)vox_string_data(parser->buffer);
    
    /* 解析第一字节 */
    uint8_t byte0 = buf[0];
    frame->fin = (byte0 & 0x80) != 0;
    frame->opcode = byte0 & 0x0F;
    
    /* 解析第二字节 */
    uint8_t byte1 = buf[1];
    frame->masked = (byte1 & 0x80) != 0;
    uint64_t payload_len = byte1 & 0x7F;
    
    size_t header_len = 2;
    
    /* 解析扩展负载长度 */
    if (payload_len == 126) {
        if (buf_len < 4) return 0;
        payload_len = ((uint64_t)buf[2] << 8) | (uint64_t)buf[3];
        header_len = 4;
    } else if (payload_len == 127) {
        if (buf_len < 10) return 0;
        payload_len = ((uint64_t)buf[2] << 56) |
                      ((uint64_t)buf[3] << 48) |
                      ((uint64_t)buf[4] << 40) |
                      ((uint64_t)buf[5] << 32) |
                      ((uint64_t)buf[6] << 24) |
                      ((uint64_t)buf[7] << 16) |
                      ((uint64_t)buf[8] << 8)  |
                      ((uint64_t)buf[9]);
        header_len = 10;
    }
    
    frame->payload_len = payload_len;
    
    /* 解析掩码密钥 */
    if (frame->masked) {
        if (buf_len < header_len + 4) return 0;
        memcpy(frame->mask_key, buf + header_len, 4);
        header_len += 4;
    }
    
    /* 检查是否有完整的负载 */
    if (buf_len < header_len + payload_len) return 0;
    
    /* 设置负载指针 */
    frame->payload = buf + header_len;
    
    /* 控制帧验证 */
    if ((frame->opcode & 0x08) != 0) {
        if (!frame->fin || payload_len > 125) {
            return -1; /* 控制帧必须 FIN=1 且负载<=125 */
        }
    }
    
    return (int)(header_len + payload_len);
}

/* 掩码/解掩码 */
void vox_ws_mask_payload(uint8_t* payload, size_t len, const uint8_t mask_key[4]) {
    if (!payload || !mask_key) return;
    
    for (size_t i = 0; i < len; i++) {
        payload[i] ^= mask_key[i & 3];
    }
}

/* 生成随机掩码密钥 */
void vox_ws_generate_mask_key(uint8_t mask_key[4]) {
    if (!mask_key) return;
    vox_crypto_random_bytes(mask_key, 4);
}

/* 构建帧 */
int vox_ws_build_frame(vox_mpool_t* mpool, uint8_t opcode, const void* payload,
                       size_t payload_len, bool masked, void** out_frame, size_t* out_len) {
    if (!mpool || !out_frame || !out_len) return -1;
    if (payload_len > 0 && !payload) return -1;
    
    /* 计算头部长度 */
    size_t header_len = 2;
    if (payload_len > 125) {
        header_len += (payload_len <= 0xFFFF) ? 2 : 8;
    }
    if (masked) {
        header_len += 4;
    }
    
    /* 分配内存 */
    size_t total_len = header_len + payload_len;
    uint8_t* frame = (uint8_t*)vox_mpool_alloc(mpool, total_len);
    if (!frame) return -1;
    
    /* 构建第一字节：FIN=1, RSV=0, opcode */
    frame[0] = 0x80 | (opcode & 0x0F);
    
    /* 构建第二字节和长度 */
    size_t pos = 2;
    if (payload_len <= 125) {
        frame[1] = (uint8_t)payload_len;
    } else if (payload_len <= 0xFFFF) {
        frame[1] = 126;
        frame[2] = (uint8_t)((payload_len >> 8) & 0xFF);
        frame[3] = (uint8_t)(payload_len & 0xFF);
        pos = 4;
    } else {
        frame[1] = 127;
        uint64_t len64 = (uint64_t)payload_len;
        frame[2] = (uint8_t)((len64 >> 56) & 0xFF);
        frame[3] = (uint8_t)((len64 >> 48) & 0xFF);
        frame[4] = (uint8_t)((len64 >> 40) & 0xFF);
        frame[5] = (uint8_t)((len64 >> 32) & 0xFF);
        frame[6] = (uint8_t)((len64 >> 24) & 0xFF);
        frame[7] = (uint8_t)((len64 >> 16) & 0xFF);
        frame[8] = (uint8_t)((len64 >> 8) & 0xFF);
        frame[9] = (uint8_t)(len64 & 0xFF);
        pos = 10;
    }
    
    /* 设置掩码标志和密钥 */
    uint8_t mask_key[4] = {0};
    if (masked) {
        frame[1] |= 0x80;
        vox_ws_generate_mask_key(mask_key);
        memcpy(frame + pos, mask_key, 4);
        pos += 4;
    }
    
    /* 复制负载 */
    if (payload_len > 0) {
        memcpy(frame + pos, payload, payload_len);
        
        /* 应用掩码 */
        if (masked) {
            vox_ws_mask_payload(frame + pos, payload_len, mask_key);
        }
    }
    
    *out_frame = frame;
    *out_len = total_len;
    return 0;
}

/* 验证关闭状态码 */
static bool vox_ws_is_valid_close_code(uint16_t code) {
    /* RFC 6455: 有效范围 1000-4999 */
    if (code < 1000 || code > 4999) {
        return false;
    }
    
    /* RFC 6455: 保留状态码不能在Close帧中使用 */
    if (code == 1004 || code == 1005 || code == 1006 || code == 1015) {
        return false;
    }
    
    return true;
}

/* 构建关闭帧 */
int vox_ws_build_close_frame(vox_mpool_t* mpool, uint16_t code, const char* reason,
                              bool masked, void** out_frame, size_t* out_len) {
    if (!mpool || !out_frame || !out_len) return -1;
    
    /* 验证状态码 */
    if (!vox_ws_is_valid_close_code(code)) {
        return -1;
    }
    
    size_t reason_len = reason ? strlen(reason) : 0;
    
    /* 验证 reason 的 UTF-8 编码 */
    if (reason_len > 0 && !vox_ws_validate_utf8((const uint8_t*)reason, reason_len)) {
        return -1;
    }
    
    /* RFC 6455: Close reason 不能超过 123 字节（控制帧最大 125 字节 - 2 字节状态码） */
    if (reason_len > 123) {
        return -1;
    }
    
    size_t payload_len = 2 + reason_len;
    
    /* 分配负载缓冲区 */
    uint8_t* payload = (uint8_t*)vox_mpool_alloc(mpool, payload_len);
    if (!payload) return -1;
    
    /* 构建负载：状态码 + 原因 */
    payload[0] = (uint8_t)((code >> 8) & 0xFF);
    payload[1] = (uint8_t)(code & 0xFF);
    if (reason_len > 0) {
        memcpy(payload + 2, reason, reason_len);
    }
    
    return vox_ws_build_frame(mpool, VOX_WS_OP_CLOSE, payload, payload_len, 
                              masked, out_frame, out_len);
}

/* UTF-8 验证 */
bool vox_ws_validate_utf8(const uint8_t* data, size_t len) {
    if (!data && len > 0) return false;
    
    size_t i = 0;
    while (i < len) {
        uint8_t byte = data[i];
        size_t char_len;
        
        if ((byte & 0x80) == 0) {
            /* 单字节字符 (0xxxxxxx) */
            char_len = 1;
        } else if ((byte & 0xE0) == 0xC0) {
            /* 双字节字符 (110xxxxx 10xxxxxx) */
            char_len = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            /* 三字节字符 (1110xxxx 10xxxxxx 10xxxxxx) */
            char_len = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            /* 四字节字符 (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx) */
            char_len = 4;
        } else {
            return false; /* 无效的起始字节 */
        }
        
        if (i + char_len > len) {
            return false; /* 不完整的字符 */
        }
        
        /* 验证后续字节 */
        for (size_t j = 1; j < char_len; j++) {
            if ((data[i + j] & 0xC0) != 0x80) {
                return false; /* 后续字节必须是 10xxxxxx */
            }
        }
        
        i += char_len;
    }
    
    return true;
}
