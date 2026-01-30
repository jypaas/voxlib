/*
 * vox_websocket.h - WebSocket 协议核心定义和帧处理
 * 提供 WebSocket 协议的底层帧编解码和状态管理
 */

#ifndef VOX_WEBSOCKET_H
#define VOX_WEBSOCKET_H

#include "../vox_os.h"
#include "../vox_mpool.h"
#include "../vox_string.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WebSocket 操作码 */
typedef enum {
    VOX_WS_OP_CONTINUATION = 0x0,  /* 继续帧 */
    VOX_WS_OP_TEXT = 0x1,          /* 文本帧 */
    VOX_WS_OP_BINARY = 0x2,        /* 二进制帧 */
    VOX_WS_OP_CLOSE = 0x8,         /* 关闭帧 */
    VOX_WS_OP_PING = 0x9,          /* Ping 帧 */
    VOX_WS_OP_PONG = 0xA           /* Pong 帧 */
} vox_ws_opcode_t;

/* WebSocket 关闭状态码 */
typedef enum {
    VOX_WS_CLOSE_NORMAL = 1000,           /* 正常关闭 */
    VOX_WS_CLOSE_GOING_AWAY = 1001,       /* 端点离开 */
    VOX_WS_CLOSE_PROTOCOL_ERROR = 1002,   /* 协议错误 */
    VOX_WS_CLOSE_UNSUPPORTED_DATA = 1003, /* 不支持的数据类型 */
    VOX_WS_CLOSE_NO_STATUS = 1005,        /* 没有状态码 */
    VOX_WS_CLOSE_ABNORMAL = 1006,         /* 异常关闭 */
    VOX_WS_CLOSE_INVALID_DATA = 1007,     /* 无效数据 */
    VOX_WS_CLOSE_POLICY_VIOLATION = 1008, /* 策略违规 */
    VOX_WS_CLOSE_MESSAGE_TOO_BIG = 1009,  /* 消息太大 */
    VOX_WS_CLOSE_INTERNAL_ERROR = 1011    /* 内部错误 */
} vox_ws_close_code_t;

/* WebSocket 帧结构 */
typedef struct {
    bool fin;                   /* FIN 标志 */
    uint8_t opcode;            /* 操作码 */
    bool masked;               /* 是否掩码 */
    uint64_t payload_len;      /* 负载长度 */
    uint8_t mask_key[4];       /* 掩码密钥 */
    const uint8_t* payload;    /* 负载数据指针 */
} vox_ws_frame_t;

/* WebSocket 消息类型 */
typedef enum {
    VOX_WS_MSG_TEXT,           /* 文本消息 */
    VOX_WS_MSG_BINARY          /* 二进制消息 */
} vox_ws_message_type_t;

/* WebSocket 帧解析器状态 */
typedef struct {
    vox_mpool_t* mpool;        /* 内存池 */
    vox_string_t* buffer;      /* 输入缓冲区 */
    vox_string_t* fragment;    /* 分片重组缓冲区 */
    bool in_fragment;          /* 是否处于分片状态 */
    vox_ws_message_type_t fragment_type; /* 分片类型 */
} vox_ws_parser_t;

/**
 * 创建 WebSocket 帧解析器
 * @param mpool 内存池
 * @return 成功返回解析器指针，失败返回 NULL
 */
vox_ws_parser_t* vox_ws_parser_create(vox_mpool_t* mpool);

/**
 * 销毁 WebSocket 帧解析器
 * @param parser 解析器指针
 */
void vox_ws_parser_destroy(vox_ws_parser_t* parser);

/**
 * 重置解析器状态
 * @param parser 解析器指针
 */
void vox_ws_parser_reset(vox_ws_parser_t* parser);

/**
 * 输入数据到解析器
 * @param parser 解析器指针
 * @param data 数据指针
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int vox_ws_parser_feed(vox_ws_parser_t* parser, const void* data, size_t len);

/**
 * 解析下一个 WebSocket 帧
 * @param parser 解析器指针
 * @param frame 输出帧结构指针
 * @return 成功返回帧长度（含头和负载），0表示数据不足，-1表示错误
 */
int vox_ws_parser_parse_frame(vox_ws_parser_t* parser, vox_ws_frame_t* frame);

/**
 * 构建 WebSocket 帧
 * @param mpool 内存池
 * @param opcode 操作码
 * @param payload 负载数据
 * @param payload_len 负载长度
 * @param masked 是否使用掩码（客户端必须为 true）
 * @param out_frame 输出缓冲区指针
 * @param out_len 输出缓冲区长度指针
 * @return 成功返回0，失败返回-1
 */
int vox_ws_build_frame(vox_mpool_t* mpool, uint8_t opcode, const void* payload, 
                       size_t payload_len, bool masked, void** out_frame, size_t* out_len);

/**
 * 构建 WebSocket 关闭帧
 * @param mpool 内存池
 * @param code 关闭状态码
 * @param reason 关闭原因（可以为 NULL）
 * @param masked 是否使用掩码
 * @param out_frame 输出缓冲区指针
 * @param out_len 输出缓冲区长度指针
 * @return 成功返回0，失败返回-1
 */
int vox_ws_build_close_frame(vox_mpool_t* mpool, uint16_t code, const char* reason,
                              bool masked, void** out_frame, size_t* out_len);

/**
 * 掩码/解掩码负载数据（就地操作）
 * @param payload 负载数据
 * @param len 数据长度
 * @param mask_key 掩码密钥（4字节）
 */
void vox_ws_mask_payload(uint8_t* payload, size_t len, const uint8_t mask_key[4]);

/**
 * 生成随机掩码密钥
 * @param mask_key 输出掩码密钥（4字节）
 */
void vox_ws_generate_mask_key(uint8_t mask_key[4]);

/**
 * 验证 UTF-8 编码
 * @param data 数据指针
 * @param len 数据长度
 * @return 有效返回 true，否则返回 false
 */
bool vox_ws_validate_utf8(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* VOX_WEBSOCKET_H */
