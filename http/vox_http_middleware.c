/*
 * vox_http_middleware.c - HTTP 常用中间件实现
 */

#include "vox_http_middleware.h"
#include "vox_http_context.h"
#include "vox_http_internal.h"
#include "../vox_log.h"
#include "../vox_time.h"
#include "../vox_mpool.h"
#include "../vox_crypto.h"
#include "../vox_htable.h"
#include "../vox_mutex.h"
#include "../vox_atomic.h"
#include "../vox_os.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* 辅助宏：将微秒转换为毫秒 */
#define vox_time_now_ms() ((int64_t)(vox_time_now() / 1000))

/* ===== 日志中间件 ===== */

/* 获取HTTP方法字符串 */
static const char* get_method_string(vox_http_method_t method) {
    switch (method) {
        case VOX_HTTP_METHOD_GET: return "GET";
        case VOX_HTTP_METHOD_POST: return "POST";
        case VOX_HTTP_METHOD_PUT: return "PUT";
        case VOX_HTTP_METHOD_DELETE: return "DELETE";
        case VOX_HTTP_METHOD_PATCH: return "PATCH";
        case VOX_HTTP_METHOD_HEAD: return "HEAD";
        case VOX_HTTP_METHOD_OPTIONS: return "OPTIONS";
        case VOX_HTTP_METHOD_CONNECT: return "CONNECT";
        case VOX_HTTP_METHOD_TRACE: return "TRACE";
        default: return "UNKNOWN";
    }
}

/* 获取请求头值（用于日志） */
static const char* get_header_value(const vox_http_context_t* ctx, const char* name, char* buf, size_t buf_size) {
    if (!ctx || !name || !buf || buf_size == 0) {
        if (buf && buf_size > 0) buf[0] = '\0';
        return buf;
    }
    
    vox_strview_t value = vox_http_context_get_header(ctx, name);
    if (value.ptr && value.len > 0) {
        size_t copy_len = (value.len < buf_size - 1) ? value.len : (buf_size - 1);
        memcpy(buf, value.ptr, copy_len);
        buf[copy_len] = '\0';
        return buf;
    }
    
    buf[0] = '\0';
    return buf;
}

/* 格式化日志输出（Combined Log Format风格） */
void vox_http_middleware_logger(vox_http_context_t* ctx) {
    if (!ctx) {
        vox_http_context_next(ctx);
        return;
    }
    
    const vox_http_request_t* req = vox_http_context_request(ctx);
    if (!req) {
        vox_http_context_next(ctx);
        return;
    }
    
    /* 记录请求开始时间 */
    int64_t start_time = vox_time_now_ms();
    vox_http_context_set_user_data(ctx, (void*)(intptr_t)start_time);
    
    /* 获取客户端IP（使用缓存，避免系统调用） */
    char client_ip[64] = {0};
    if (vox_http_conn_get_client_ip(ctx->conn, client_ip, sizeof(client_ip)) != 0) {
        strncpy(client_ip, "-", sizeof(client_ip) - 1);
    }
    
    /* 获取User-Agent */
    char user_agent[256] = {0};
    get_header_value(ctx, "User-Agent", user_agent, sizeof(user_agent));
    if (user_agent[0] == '\0') {
        strncpy(user_agent, "-", sizeof(user_agent) - 1);
    }
    
    /* 获取Referer */
    char referer[256] = {0};
    get_header_value(ctx, "Referer", referer, sizeof(referer));
    if (referer[0] == '\0') {
        strncpy(referer, "-", sizeof(referer) - 1);
    }
    
    /* 获取请求方法 */
    const char* method = get_method_string(req->method);
    
    /* 构建完整URL（包含查询字符串） */
    char full_url[512] = {0};
    if (req->path.ptr && req->path.len > 0) {
        size_t url_len = 0;
        if (req->path.len < sizeof(full_url) - 1) {
            memcpy(full_url, req->path.ptr, req->path.len);
            url_len = req->path.len;
        }
        
        /* 添加查询字符串 */
        if (req->query.ptr && req->query.len > 0 && url_len + req->query.len + 1 < sizeof(full_url) - 1) {
            full_url[url_len++] = '?';
            memcpy(full_url + url_len, req->query.ptr, req->query.len);
            url_len += req->query.len;
        }
        full_url[url_len] = '\0';
    } else {
        strncpy(full_url, "-", sizeof(full_url) - 1);
    }
    
    /* 获取HTTP版本 */
    char http_version[16] = {0};
    snprintf(http_version, sizeof(http_version), "%d.%d", req->http_major, req->http_minor);
    
    /* 继续执行后续中间件 */
    vox_http_context_next(ctx);
    
    /* 记录响应信息（请求完成） */
    vox_http_response_t* res = vox_http_context_response(ctx);
    if (res) {
        int64_t end_time = vox_time_now_ms();
        int64_t duration = end_time - start_time;
        int status = res->status ? res->status : 200;
        size_t response_size = res->body ? vox_string_length(res->body) : 0;
        
        /* 使用Combined Log Format风格输出（不包含时间戳，日志系统自带）：
         * client_ip "method url protocol" status response_size duration "referer" "user-agent"
         */
        VOX_LOG_INFO("[HTTP] %s \"%s %s HTTP/%s\" %d %zu %lld \"%s\" \"%s\"",
                    client_ip, method, full_url, http_version, status, response_size,
                    (long long)duration, referer, user_agent);
    }
}

/* ===== CORS 中间件 ===== */

typedef struct {
    const char* allow_origin;
    const char* allow_methods;
    const char* allow_headers;
    bool allow_credentials;
} vox_http_cors_config_t;

static void vox_http_middleware_cors_impl(vox_http_context_t* ctx, const vox_http_cors_config_t* config) {
    if (!ctx || !config) {
        vox_http_context_next(ctx);
        return;
    }
    
    const vox_http_request_t* req = vox_http_context_request(ctx);
    if (!req) {
        vox_http_context_next(ctx);
        return;
    }
    
    /* 处理 OPTIONS 预检请求 */
    if (req->method == VOX_HTTP_METHOD_OPTIONS) {
        vox_http_context_status(ctx, 204);
        
        if (config->allow_origin) {
            vox_http_context_header(ctx, "Access-Control-Allow-Origin", config->allow_origin);
        }
        if (config->allow_methods) {
            vox_http_context_header(ctx, "Access-Control-Allow-Methods", config->allow_methods);
        }
        if (config->allow_headers) {
            vox_http_context_header(ctx, "Access-Control-Allow-Headers", config->allow_headers);
        }
        if (config->allow_credentials) {
            vox_http_context_header(ctx, "Access-Control-Allow-Credentials", "true");
        }
        vox_http_context_header(ctx, "Access-Control-Max-Age", "86400");
        return; /* 不继续执行后续中间件 */
    }
    
    /* 普通请求：添加 CORS 响应头 */
    if (config->allow_origin) {
        vox_http_context_header(ctx, "Access-Control-Allow-Origin", config->allow_origin);
    }
    if (config->allow_methods) {
        vox_http_context_header(ctx, "Access-Control-Allow-Methods", config->allow_methods);
    }
    if (config->allow_headers) {
        vox_http_context_header(ctx, "Access-Control-Allow-Headers", config->allow_headers);
    }
    if (config->allow_credentials) {
        vox_http_context_header(ctx, "Access-Control-Allow-Credentials", "true");
    }
    
    vox_http_context_next(ctx);
}

/* 默认 CORS 配置：允许所有来源 */
static const vox_http_cors_config_t vox_http_cors_default = {
    .allow_origin = "*",
    .allow_methods = "GET, POST, PUT, DELETE, PATCH, OPTIONS",
    .allow_headers = "Content-Type, Authorization",
    .allow_credentials = false
};

void vox_http_middleware_cors(vox_http_context_t* ctx) {
    vox_http_middleware_cors_impl(ctx, &vox_http_cors_default);
}

/* ===== Basic 认证中间件 ===== */
/* 注意：vox_http_basic_auth_config_t 已在 vox_http_middleware.h 中定义 */

static void vox_http_middleware_basic_auth_impl(vox_http_context_t* ctx, const vox_http_basic_auth_config_t* config) {
    if (!ctx || !config || !config->username || !config->password) {
        vox_http_context_next(ctx);
        return;
    }
    
    vox_strview_t auth_header = vox_http_context_get_header(ctx, "Authorization");
    if (!auth_header.ptr || auth_header.len == 0) {
        vox_http_context_status(ctx, 401);
        const char* realm = config->realm ? config->realm : "Restricted";
        char www_auth[256];
        snprintf(www_auth, sizeof(www_auth), "Basic realm=\"%s\"", realm);
        vox_http_context_header(ctx, "WWW-Authenticate", www_auth);
        vox_http_context_write_cstr(ctx, "Unauthorized");
        return;
    }
    
    /* 检查 "Basic " 前缀 */
    if (auth_header.len < 6 || strncmp(auth_header.ptr, "Basic ", 6) != 0) {
        vox_http_context_status(ctx, 401);
        vox_http_context_write_cstr(ctx, "Unauthorized");
        return;
    }
    
    /* 解码 Base64 - 使用 vox_crypto 的接口 */
    const char* encoded = auth_header.ptr + 6;
    size_t encoded_len = auth_header.len - 6;
    
    /* vox_base64_decode 需要以 '\0' 结尾的字符串，所以需要临时复制 */
    vox_mpool_t* mpool = vox_http_context_get_mpool(ctx);
    if (!mpool) {
        vox_http_context_status(ctx, 500);
        vox_http_context_write_cstr(ctx, "Internal Server Error");
        return;
    }
    
    char* encoded_str = (char*)vox_mpool_alloc(mpool, encoded_len + 1);
    if (!encoded_str) {
        vox_http_context_status(ctx, 500);
        vox_http_context_write_cstr(ctx, "Internal Server Error");
        return;
    }
    memcpy(encoded_str, encoded, encoded_len);
    encoded_str[encoded_len] = '\0';
    
    char decoded[256];
    int decoded_len = vox_base64_decode(encoded_str, decoded, sizeof(decoded));
    
    if (decoded_len < 0 || decoded_len == 0) {
        vox_http_context_status(ctx, 401);
        vox_http_context_write_cstr(ctx, "Unauthorized");
        return;
    }
    
    /* 查找冒号分隔符 */
    const char* colon = (const char*)memchr(decoded, ':', (size_t)decoded_len);
    if (!colon) {
        vox_http_context_status(ctx, 401);
        vox_http_context_write_cstr(ctx, "Unauthorized");
        return;
    }
    
    size_t username_len = (size_t)(colon - decoded);
    size_t password_len = (size_t)decoded_len - username_len - 1;
    
    /* 验证用户名和密码 */
    if (username_len != strlen(config->username) ||
        password_len != strlen(config->password) ||
        strncmp(decoded, config->username, username_len) != 0 ||
        strncmp(colon + 1, config->password, password_len) != 0) {
        vox_http_context_status(ctx, 401);
        vox_http_context_write_cstr(ctx, "Unauthorized");
        return;
    }
    
    /* 认证成功，继续执行 */
    vox_http_context_next(ctx);
}

/* ===== Bearer Token 认证中间件 ===== */

/* 注意：vox_http_token_validator_t 和 vox_http_bearer_auth_config_t 已在 vox_http_middleware.h 中定义 */

static void vox_http_middleware_bearer_auth_impl(vox_http_context_t* ctx, const vox_http_bearer_auth_config_t* config) {
    if (!ctx || !config || !config->validator) {
        vox_http_context_next(ctx);
        return;
    }
    
    vox_strview_t auth_header = vox_http_context_get_header(ctx, "Authorization");
    if (!auth_header.ptr || auth_header.len == 0) {
        vox_http_context_status(ctx, 401);
        const char* realm = config->realm ? config->realm : "Restricted";
        char www_auth[256];
        snprintf(www_auth, sizeof(www_auth), "Bearer realm=\"%s\"", realm);
        vox_http_context_header(ctx, "WWW-Authenticate", www_auth);
        vox_http_context_write_cstr(ctx, "Unauthorized");
        return;
    }
    
    /* 检查 "Bearer " 前缀 */
    if (auth_header.len < 7 || strncmp(auth_header.ptr, "Bearer ", 7) != 0) {
        vox_http_context_status(ctx, 401);
        vox_http_context_write_cstr(ctx, "Unauthorized");
        return;
    }
    
    /* 提取 token */
    const char* token = auth_header.ptr + 7;
    size_t token_len = auth_header.len - 7;
    
    /* 复制 token 到临时缓冲区（validator 需要 C 字符串） */
    vox_mpool_t* mpool = vox_http_context_get_mpool(ctx);
    if (!mpool) {
        vox_http_context_status(ctx, 500);
        vox_http_context_write_cstr(ctx, "Internal Server Error");
        return;
    }
    
    char* token_buf = (char*)vox_mpool_alloc(mpool, token_len + 1);
    if (!token_buf) {
        vox_http_context_status(ctx, 500);
        vox_http_context_write_cstr(ctx, "Internal Server Error");
        return;
    }
    memcpy(token_buf, token, token_len);
    token_buf[token_len] = '\0';
    
    /* 验证 token */
    if (!config->validator(token_buf, config->validator_data)) {
        vox_http_context_status(ctx, 401);
        vox_http_context_write_cstr(ctx, "Unauthorized");
        return;
    }
    
    /* 认证成功，继续执行 */
    vox_http_context_next(ctx);
}

/* ===== 错误处理中间件 ===== */

void vox_http_middleware_error_handler(vox_http_context_t* ctx) {
    if (!ctx) return;
    
    /* 继续执行后续中间件 */
    vox_http_context_next(ctx);
    
    /* 检查是否有错误状态码 */
    vox_http_response_t* res = vox_http_context_response(ctx);
    if (!res) return;
    
    int status = res->status ? res->status : 200;
    
    /* 如果是错误状态码且没有响应体，生成默认错误消息 */
    if (status >= 400 && (!res->body || vox_string_length(res->body) == 0)) {
        const char* message = NULL;
        switch (status) {
            case 400: message = "Bad Request"; break;
            case 401: message = "Unauthorized"; break;
            case 403: message = "Forbidden"; break;
            case 404: message = "Not Found"; break;
            case 405: message = "Method Not Allowed"; break;
            case 500: message = "Internal Server Error"; break;
            case 502: message = "Bad Gateway"; break;
            case 503: message = "Service Unavailable"; break;
            default: message = "Error"; break;
        }
        
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "%d %s", status, message);
        vox_http_context_write_cstr(ctx, error_msg);
        
        /* 设置 Content-Type */
        vox_strview_t content_type = vox_http_context_get_header(ctx, "Content-Type");
        if (!content_type.ptr || content_type.len == 0) {
            vox_http_context_header(ctx, "Content-Type", "text/plain; charset=utf-8");
        }
    }
}

/* ===== 请求体大小限制中间件 ===== */

typedef struct {
    size_t max_size;
} vox_http_body_limit_config_t;

static void vox_http_middleware_body_limit_impl(vox_http_context_t* ctx, const vox_http_body_limit_config_t* config) {
    if (!ctx || !config || config->max_size == 0) {
        vox_http_context_next(ctx);
        return;
    }
    
    const vox_http_request_t* req = vox_http_context_request(ctx);
    if (!req || !req->body) {
        vox_http_context_next(ctx);
        return;
    }
    
    size_t body_len = vox_string_length(req->body);
    if (body_len > config->max_size) {
        vox_http_context_status(ctx, 413);
        vox_http_context_write_cstr(ctx, "Request Entity Too Large");
        return;
    }
    
    vox_http_context_next(ctx);
}

/* ===== 中间件创建函数 ===== */

/* 中间件配置存储结构（使用 mpool 分配，避免全局静态变量） */

/* Basic Auth 中间件配置存储 */
typedef struct {
    vox_http_basic_auth_config_t config;
    /* 确保字符串在内存池中有副本 */
    char* username_copy;
    char* password_copy;
    char* realm_copy;
} vox_http_basic_auth_storage_t;

/* Basic Auth 中间件包装器 */
VOX_UNUSED_FUNC static void vox_http_middleware_basic_auth_wrapper(vox_http_context_t* ctx) {
    vox_http_basic_auth_storage_t* storage = (vox_http_basic_auth_storage_t*)vox_http_context_get_user_data(ctx);
    if (storage) {
        /* 保存原始 user_data */
        void* original_user_data = vox_http_context_get_user_data(ctx);
        
        /* 临时清除 user_data 以防止被 impl 函数误用 */
        vox_http_context_set_user_data(ctx, NULL);
        
        vox_http_middleware_basic_auth_impl(ctx, &storage->config);
        
        /* 恢复原始 user_data */
        vox_http_context_set_user_data(ctx, original_user_data);
    } else {
        vox_http_context_next(ctx);
    }
}

vox_http_handler_cb vox_http_middleware_basic_auth_create(vox_mpool_t* mpool, const vox_http_basic_auth_config_t* config) {
    if (!mpool || !config || !config->username || !config->password) {
        return NULL;
    }
    
    /* 在内存池中分配配置存储 */
    vox_http_basic_auth_storage_t* storage = (vox_http_basic_auth_storage_t*)vox_mpool_alloc(mpool, sizeof(vox_http_basic_auth_storage_t));
    if (!storage) return NULL;
    
    memset(storage, 0, sizeof(*storage));
    
    /* 复制字符串到内存池 */
    size_t username_len = strlen(config->username);
    storage->username_copy = (char*)vox_mpool_alloc(mpool, username_len + 1);
    if (!storage->username_copy) return NULL;
    memcpy(storage->username_copy, config->username, username_len + 1);
    
    size_t password_len = strlen(config->password);
    storage->password_copy = (char*)vox_mpool_alloc(mpool, password_len + 1);
    if (!storage->password_copy) return NULL;
    memcpy(storage->password_copy, config->password, password_len + 1);
    
    if (config->realm) {
        size_t realm_len = strlen(config->realm);
        storage->realm_copy = (char*)vox_mpool_alloc(mpool, realm_len + 1);
        if (!storage->realm_copy) return NULL;
        memcpy(storage->realm_copy, config->realm, realm_len + 1);
        storage->config.realm = storage->realm_copy;
    } else {
        storage->config.realm = NULL;
    }
    
    storage->config.username = storage->username_copy;
    storage->config.password = storage->password_copy;
    
    /* 注意：我们不能直接返回 wrapper，因为它需要访问 storage
     * 这里需要一个机制将 storage 传递给 wrapper
     * 暂时返回 NULL 表示需要进一步设计 */
    (void)storage; /* TODO: 实现配置传递机制 */
    
    return NULL; /* TODO: 需要修改架构以支持配置传递 */
}

/* Bearer Auth 中间件配置存储 */
typedef struct {
    vox_http_bearer_auth_config_t config;
    char* realm_copy;
} vox_http_bearer_auth_storage_t;

/* Bearer Auth 中间件包装器 */
VOX_UNUSED_FUNC static void vox_http_middleware_bearer_auth_wrapper(vox_http_context_t* ctx) {
    vox_http_bearer_auth_storage_t* storage = (vox_http_bearer_auth_storage_t*)vox_http_context_get_user_data(ctx);
    if (storage) {
        void* original_user_data = vox_http_context_get_user_data(ctx);
        vox_http_context_set_user_data(ctx, NULL);
        vox_http_middleware_bearer_auth_impl(ctx, &storage->config);
        vox_http_context_set_user_data(ctx, original_user_data);
    } else {
        vox_http_context_next(ctx);
    }
}

vox_http_handler_cb vox_http_middleware_bearer_auth_create(vox_mpool_t* mpool, const vox_http_bearer_auth_config_t* config) {
    if (!mpool || !config || !config->validator) {
        return NULL;
    }
    
    vox_http_bearer_auth_storage_t* storage = (vox_http_bearer_auth_storage_t*)vox_mpool_alloc(mpool, sizeof(vox_http_bearer_auth_storage_t));
    if (!storage) return NULL;
    
    memset(storage, 0, sizeof(*storage));
    storage->config.validator = config->validator;
    storage->config.validator_data = config->validator_data;
    
    if (config->realm) {
        size_t realm_len = strlen(config->realm);
        storage->realm_copy = (char*)vox_mpool_alloc(mpool, realm_len + 1);
        if (!storage->realm_copy) return NULL;
        memcpy(storage->realm_copy, config->realm, realm_len + 1);
        storage->config.realm = storage->realm_copy;
    } else {
        storage->config.realm = NULL;
    }
    
    (void)storage;
    return NULL; /* TODO: 需要修改架构以支持配置传递 */
}

/* Body Limit 中间件配置存储 */
typedef struct {
    vox_http_body_limit_config_t config;
} vox_http_body_limit_storage_t;

/* Body Limit 中间件包装器 */
VOX_UNUSED_FUNC static void vox_http_middleware_body_limit_wrapper(vox_http_context_t* ctx) {
    vox_http_body_limit_storage_t* storage = (vox_http_body_limit_storage_t*)vox_http_context_get_user_data(ctx);
    if (storage && storage->config.max_size > 0) {
        void* original_user_data = vox_http_context_get_user_data(ctx);
        vox_http_context_set_user_data(ctx, NULL);
        vox_http_middleware_body_limit_impl(ctx, &storage->config);
        vox_http_context_set_user_data(ctx, original_user_data);
    } else {
        vox_http_context_next(ctx);
    }
}

vox_http_handler_cb vox_http_middleware_body_limit_create(vox_mpool_t* mpool, size_t max_size) {
    if (!mpool || max_size == 0) {
        return NULL;
    }
    
    vox_http_body_limit_storage_t* storage = (vox_http_body_limit_storage_t*)vox_mpool_alloc(mpool, sizeof(vox_http_body_limit_storage_t));
    if (!storage) return NULL;
    
    storage->config.max_size = max_size;
    
    (void)storage;
    return NULL; /* TODO: 需要修改架构以支持配置传递 */
}

/* ===== 限流中间件 ===== */

/* 滑动窗口限流记录（每个IP的限流信息） */
#define VOX_HTTP_RATE_LIMIT_MAX_TIMESTAMPS 100  /* 最多存储100个时间戳 */

typedef struct {
    int64_t timestamps[VOX_HTTP_RATE_LIMIT_MAX_TIMESTAMPS];  /* 请求时间戳数组（循环使用） */
    size_t count;          /* 当前时间戳数量 */
    size_t head;           /* 循环数组头部索引 */
    vox_mutex_t mutex;     /* 保护时间戳数组的互斥锁（每个IP独立锁） */
} vox_http_rate_limit_record_t;

/* 限流器结构（每个中间件实例独立） */
typedef struct {
    vox_http_rate_limit_config_t config;
    vox_htable_t* ip_records;  /* key: IP字符串, value: vox_http_rate_limit_record_t* */
    vox_mutex_t mutex;         /* 保护哈希表的互斥锁 */
    vox_mpool_t* mpool;        /* 内存池 */
    int64_t last_cleanup;      /* 上次清理时间 */
    char* message_copy;        /* 错误消息副本 */
} vox_http_rate_limiter_t;

/* 清理过期的限流记录 */
/* 注意：此函数当前未使用，保留用于将来实现定时清理功能 */
VOX_UNUSED_FUNC static void cleanup_expired_records(vox_http_rate_limiter_t* limiter) {
    if (!limiter || !limiter->ip_records) return;
    
    int64_t now = vox_time_now_ms();
    /* 每5秒清理一次 */
    if (now - limiter->last_cleanup < 5000) {
        return;
    }
    limiter->last_cleanup = now;
    
    /* 遍历哈希表，删除过期的记录 */
    /* 注意：vox_htable 没有提供迭代器，这里简化处理，只清理明显过期的 */
    /* 实际生产环境应该使用定时器定期清理 */
}

/* 获取或创建限流记录 */
static vox_http_rate_limit_record_t* get_or_create_record(vox_http_rate_limiter_t* limiter, const char* ip) {
    if (!limiter || !ip) return NULL;
    
    vox_mutex_lock(&limiter->mutex);
    
    vox_http_rate_limit_record_t* record = (vox_http_rate_limit_record_t*)vox_htable_get(
        limiter->ip_records, ip, strlen(ip));
    
    if (!record) {
        /* 创建新记录 */
        record = (vox_http_rate_limit_record_t*)vox_mpool_alloc(limiter->mpool, sizeof(*record));
        if (record) {
            memset(record, 0, sizeof(*record));
            if (vox_mutex_create(&record->mutex) != 0) {
                vox_mpool_free(limiter->mpool, record);
                record = NULL;
            } else {
                /* 存储到哈希表 */
                if (vox_htable_set(limiter->ip_records, ip, strlen(ip), record) != 0) {
                    vox_mutex_destroy(&record->mutex);
                    vox_mpool_free(limiter->mpool, record);
                    record = NULL;
                }
            }
        }
    }
    
    vox_mutex_unlock(&limiter->mutex);
    
    return record;
}

/* 滑动窗口：移除过期的时间戳并添加新时间戳 */
static size_t sliding_window_update(vox_http_rate_limit_record_t* record, int64_t now, int64_t window_ms) {
    if (!record) return 0;
    
    vox_mutex_lock(&record->mutex);
    
    /* 移除过期的时间戳（时间戳 < now - window_ms） */
    int64_t window_start = now - window_ms;
    size_t valid_count = 0;
    size_t first_valid_idx = 0;
    bool found_valid = false;
    
    /* 找到第一个未过期的时间戳位置 */
    for (size_t i = 0; i < record->count; i++) {
        size_t idx = (record->head + i) % VOX_HTTP_RATE_LIMIT_MAX_TIMESTAMPS;
        if (record->timestamps[idx] >= window_start) {
            if (!found_valid) {
                first_valid_idx = i;
                found_valid = true;
            }
            valid_count++;
        }
    }
    
    /* 更新头部（指向第一个有效时间戳） */
    if (valid_count > 0) {
        record->head = (record->head + first_valid_idx) % VOX_HTTP_RATE_LIMIT_MAX_TIMESTAMPS;
    } else {
        record->head = 0;
    }
    
    /* 添加当前时间戳 */
    if (valid_count < VOX_HTTP_RATE_LIMIT_MAX_TIMESTAMPS) {
        /* 数组未满，追加到末尾 */
        size_t tail = (record->head + valid_count) % VOX_HTTP_RATE_LIMIT_MAX_TIMESTAMPS;
        record->timestamps[tail] = now;
        valid_count++;
    } else {
        /* 数组已满，覆盖最旧的时间戳（循环使用） */
        record->timestamps[record->head] = now;
        record->head = (record->head + 1) % VOX_HTTP_RATE_LIMIT_MAX_TIMESTAMPS;
        /* count 保持不变（仍然是 MAX_TIMESTAMPS） */
    }
    
    record->count = valid_count;
    
    size_t result = record->count;
    vox_mutex_unlock(&record->mutex);
    
    return result;
}

/* 限流中间件实现（滑动窗口算法） */
static void vox_http_middleware_rate_limit_impl(vox_http_context_t* ctx, vox_http_rate_limiter_t* limiter) {
    if (!ctx || !limiter || limiter->config.max_requests == 0 || limiter->config.window_ms <= 0) {
        vox_http_context_next(ctx);
        return;
    }
    
    /* 获取客户端IP（使用缓存，避免系统调用） */
    char client_ip[64] = {0};
    if (vox_http_conn_get_client_ip(ctx->conn, client_ip, sizeof(client_ip)) != 0) {
        /* 无法获取IP，允许通过（或可以选择拒绝） */
        vox_http_context_next(ctx);
        return;
    }
    
    /* 获取限流记录 */
    vox_http_rate_limit_record_t* record = get_or_create_record(limiter, client_ip);
    if (!record) {
        /* 内存不足，允许通过 */
        vox_http_context_next(ctx);
        return;
    }
    
    /* 滑动窗口：移除过期时间戳并添加当前时间戳 */
    int64_t now = vox_time_now_ms();
    size_t current_count = sliding_window_update(record, now, limiter->config.window_ms);
    
    /* 检查是否超过限制 */
    bool exceeded = (current_count > limiter->config.max_requests);
    
    if (exceeded) {
        /* 超过限制，返回 429 Too Many Requests */
        vox_http_context_status(ctx, 429);
        const char* message = limiter->message_copy ? limiter->message_copy : "Too Many Requests";
        vox_http_context_write_cstr(ctx, message);
        
        /* 计算重试时间（窗口剩余时间） */
        int64_t retry_after_ms = 1;
        vox_mutex_lock(&record->mutex);
        if (record->count > 0) {
            int64_t oldest_timestamp = record->timestamps[record->head];
            int64_t window_end = oldest_timestamp + limiter->config.window_ms;
            retry_after_ms = (window_end - now + 999) / 1000; /* 向上取整到秒 */
            if (retry_after_ms < 1) retry_after_ms = 1;
        }
        vox_mutex_unlock(&record->mutex);
        
        /* 缓冲区足够大以容纳 int64_t 的最大值（19位数字 + 负号 + 空字符） */
        char retry_str[32];
        snprintf(retry_str, sizeof(retry_str), "%lld", (long long)retry_after_ms);
        vox_http_context_header(ctx, "Retry-After", retry_str);
        return;
    }
    
    /* 继续执行 */
    vox_http_context_next(ctx);
}

/* 限流中间件包装器 */
VOX_UNUSED_FUNC static void vox_http_middleware_rate_limit_wrapper(vox_http_context_t* ctx) {
    vox_http_rate_limiter_t* limiter = (vox_http_rate_limiter_t*)vox_http_context_get_user_data(ctx);
    if (limiter) {
        void* original_user_data = vox_http_context_get_user_data(ctx);
        vox_http_context_set_user_data(ctx, NULL);
        vox_http_middleware_rate_limit_impl(ctx, limiter);
        vox_http_context_set_user_data(ctx, original_user_data);
    } else {
        vox_http_context_next(ctx);
    }
}

vox_http_handler_cb vox_http_middleware_rate_limit_create(vox_mpool_t* mpool, const vox_http_rate_limit_config_t* config) {
    if (!mpool || !config || config->max_requests == 0 || config->window_ms <= 0) {
        return NULL;
    }
    
    /* 创建限流器实例 */
    vox_http_rate_limiter_t* limiter = (vox_http_rate_limiter_t*)vox_mpool_alloc(mpool, sizeof(*limiter));
    if (!limiter) return NULL;
    
    memset(limiter, 0, sizeof(*limiter));
    limiter->mpool = mpool;
    limiter->config = *config;
    
    /* 复制错误消息 */
    if (config->message) {
        size_t msg_len = strlen(config->message);
        limiter->message_copy = (char*)vox_mpool_alloc(mpool, msg_len + 1);
        if (!limiter->message_copy) return NULL;
        memcpy(limiter->message_copy, config->message, msg_len + 1);
        limiter->config.message = limiter->message_copy;
    }
    
    if (vox_mutex_create(&limiter->mutex) != 0) {
        return NULL;
    }
    
    limiter->ip_records = vox_htable_create(mpool);
    if (!limiter->ip_records) {
        vox_mutex_destroy(&limiter->mutex);
        return NULL;
    }
    
    limiter->last_cleanup = vox_time_now_ms();
    
    (void)limiter;
    return NULL; /* TODO: 需要修改架构以支持配置传递 */
}
