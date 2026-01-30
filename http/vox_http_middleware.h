/*
 * vox_http_middleware.h - HTTP 中间件/处理器定义
 * 参考 Gin 的 handler chain 设计：middleware 通过 ctx->next() 驱动后续执行
 */

#ifndef VOX_HTTP_MIDDLEWARE_H
#define VOX_HTTP_MIDDLEWARE_H

#include "../vox_os.h"
#include "../vox_mpool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_http_context vox_http_context_t;

/* 处理器/中间件回调 */
typedef void (*vox_http_handler_cb)(vox_http_context_t* ctx);

/* ===== 常用中间件 ===== */

/**
 * 日志中间件（Combined Log Format风格）
 * 记录以下信息：
 * - 客户端IP地址（使用缓存，避免系统调用）
 * - HTTP方法、URL（包含查询字符串）、协议版本
 * - 响应状态码、响应大小、处理时间（毫秒）
 * - Referer和User-Agent
 * 
 * 注意：时间戳由日志系统自动输出，此处不包含
 * 
 * 日志格式：
 * [HTTP] client_ip "method url protocol" status response_size duration "referer" "user-agent"
 */
void vox_http_middleware_logger(vox_http_context_t* ctx);

/**
 * CORS 中间件（默认配置）
 * - 允许所有来源 (*)
 * - 允许方法：GET, POST, PUT, DELETE, PATCH, OPTIONS
 * - 允许头：Content-Type, Authorization
 * - 不允许凭证
 */
void vox_http_middleware_cors(vox_http_context_t* ctx);

/**
 * 错误处理中间件
 * 为错误状态码（>= 400）自动生成默认错误消息（如果响应体为空）
 */
void vox_http_middleware_error_handler(vox_http_context_t* ctx);

/* ===== 认证中间件配置 ===== */

typedef struct {
    const char* username;
    const char* password;
    const char* realm;  /* 可选，默认 "Restricted" */
} vox_http_basic_auth_config_t;

/**
 * 创建 Basic 认证中间件
 * @param mpool 内存池（用于分配配置数据）
 * @param config 认证配置（username, password, realm）
 * @return 中间件回调函数
 */
vox_http_handler_cb vox_http_middleware_basic_auth_create(vox_mpool_t* mpool, const vox_http_basic_auth_config_t* config);

/**
 * Bearer Token 验证函数类型
 * @param token 待验证的 token
 * @param user_data 用户数据
 * @return 验证通过返回 true，否则返回 false
 */
typedef bool (*vox_http_token_validator_t)(const char* token, void* user_data);

typedef struct {
    vox_http_token_validator_t validator;
    void* validator_data;
    const char* realm;  /* 可选，默认 "Restricted" */
} vox_http_bearer_auth_config_t;

/**
 * 创建 Bearer Token 认证中间件
 * @param mpool 内存池（用于分配配置数据）
 * @param config 认证配置（validator, validator_data, realm）
 * @return 中间件回调函数
 */
vox_http_handler_cb vox_http_middleware_bearer_auth_create(vox_mpool_t* mpool, const vox_http_bearer_auth_config_t* config);

/* ===== 请求体大小限制中间件 ===== */

/**
 * 创建请求体大小限制中间件
 * @param mpool 内存池（用于分配配置数据）
 * @param max_size 最大请求体大小（字节）
 * @return 中间件回调函数
 */
vox_http_handler_cb vox_http_middleware_body_limit_create(vox_mpool_t* mpool, size_t max_size);

/* ===== 限流中间件 ===== */

/**
 * 限流配置
 */
typedef struct {
    size_t max_requests;      /* 时间窗口内最大请求数 */
    int64_t window_ms;        /* 时间窗口大小（毫秒），例如 1000 表示每秒 */
    const char* message;      /* 限流时的错误消息，NULL 使用默认消息 */
} vox_http_rate_limit_config_t;

/**
 * 创建限流中间件
 * @param mpool 内存池（用于分配配置数据和IP记录哈希表）
 * @param config 限流配置（max_requests, window_ms, message）
 * @return 中间件回调函数
 */
vox_http_handler_cb vox_http_middleware_rate_limit_create(vox_mpool_t* mpool, const vox_http_rate_limit_config_t* config);

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTTP_MIDDLEWARE_H */
