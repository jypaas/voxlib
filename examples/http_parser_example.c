/*
 * http_parser_example.c - vox_http_parser 使用示例
 * 演示 HTTP 请求和响应的解析，覆盖各种场景
 */

#include "../http/vox_http_parser.h"
#include "../vox_mpool.h"
#include "../vox_string.h"
#include "../vox_vector.h"
#include "../vox_os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== 解析上下文 ===== */

/**
 * HTTP头部字段结构（用于保存）
 */
typedef struct {
    vox_string_t* name;
    vox_string_t* value;
} saved_header_t;

/**
 * 解析上下文
 */
typedef struct {
    vox_mpool_t* mpool;
    vox_string_t* url;
    vox_string_t* reason_phrase;
    vox_vector_t* headers;  /* saved_header_t* */
    vox_string_t* current_header_name;
    vox_string_t* current_header_value;
} parse_context_t;

/* 将当前 header(name/value) 提交到 headers 列表 */
static void commit_current_header(parse_context_t* ctx) {
    if (!ctx || !ctx->headers || !ctx->current_header_name || !ctx->current_header_value) return;
    if (vox_string_length(ctx->current_header_name) == 0) return;

    saved_header_t* header = (saved_header_t*)vox_mpool_alloc(ctx->mpool, sizeof(saved_header_t));
    if (!header) return;

    header->name = ctx->current_header_name;
    header->value = ctx->current_header_value;

    if (vox_vector_push(ctx->headers, header) == 0) {
        ctx->current_header_name = vox_string_create(ctx->mpool);
        ctx->current_header_value = vox_string_create(ctx->mpool);
    } else {
        vox_mpool_free(ctx->mpool, header);
    }
}

/* 辅助函数：初始化解析上下文 */
static void init_parse_context(parse_context_t* ctx, vox_mpool_t* mpool) {
    memset(ctx, 0, sizeof(parse_context_t));
    ctx->mpool = mpool;
    ctx->url = vox_string_create(mpool);
    ctx->reason_phrase = vox_string_create(mpool);
    ctx->headers = vox_vector_create(mpool);
    ctx->current_header_name = vox_string_create(mpool);
    ctx->current_header_value = vox_string_create(mpool);
}

/* 辅助函数：在上下文中查找头部 */
static const char* find_header_in_context(parse_context_t* ctx, const char* name) {
    if (!ctx || !ctx->headers || !name) return NULL;
    
    size_t name_len = strlen(name);
    size_t header_count = vox_vector_size(ctx->headers);
    
    for (size_t i = 0; i < header_count; i++) {
        saved_header_t* header = (saved_header_t*)vox_vector_get(ctx->headers, i);
        if (!header || !header->name) continue;
        
        const char* header_name = vox_string_cstr(header->name);
        size_t header_name_len = vox_string_length(header->name);
        
        if (header_name_len == name_len) {
            if (strncasecmp(header_name, name, name_len) == 0) {
                return vox_string_cstr(header->value);
            }
        }
    }
    
    return NULL;
}

/* 辅助函数：将 HTTP 方法转换为字符串 */
static const char* method_to_string(vox_http_method_t method) {
    switch (method) {
        case VOX_HTTP_METHOD_GET: return "GET";
        case VOX_HTTP_METHOD_HEAD: return "HEAD";
        case VOX_HTTP_METHOD_POST: return "POST";
        case VOX_HTTP_METHOD_PUT: return "PUT";
        case VOX_HTTP_METHOD_DELETE: return "DELETE";
        case VOX_HTTP_METHOD_CONNECT: return "CONNECT";
        case VOX_HTTP_METHOD_OPTIONS: return "OPTIONS";
        case VOX_HTTP_METHOD_TRACE: return "TRACE";
        case VOX_HTTP_METHOD_PATCH: return "PATCH";
        default: return "UNKNOWN";
    }
}

/* 回调函数：消息开始 */
static int on_message_begin(void* parser) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    parse_context_t* ctx = (parse_context_t*)vox_http_parser_get_user_data(p);
    
    if (ctx) {
        if (ctx->url) vox_string_clear(ctx->url);
        if (ctx->reason_phrase) vox_string_clear(ctx->reason_phrase);
        if (ctx->headers) {
            while (vox_vector_size(ctx->headers) > 0) {
                saved_header_t* h = (saved_header_t*)vox_vector_pop(ctx->headers);
                if (h) {
                    if (h->name) vox_string_destroy(h->name);
                    if (h->value) vox_string_destroy(h->value);
                    vox_mpool_free(ctx->mpool, h);
                }
            }
        }
        if (ctx->current_header_name) vox_string_clear(ctx->current_header_name);
        if (ctx->current_header_value) vox_string_clear(ctx->current_header_value);
    }
    
    printf("[回调] 消息开始\n");
    return 0;
}

/* 回调函数：URL（数据回调，可能多次调用） */
static int on_url(void* parser, const char* data, size_t len) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    parse_context_t* ctx = (parse_context_t*)vox_http_parser_get_user_data(p);
    
    if (ctx && ctx->url && len > 0) {
        vox_string_append_data(ctx->url, data, len);
    }
    
    printf("[回调] URL 数据块 (%zu 字节): %.*s\n", len, (int)len, data);
    return 0;
}

/* 回调函数：状态行（数据回调，可能多次调用） */
static int on_status(void* parser, const char* data, size_t len) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    parse_context_t* ctx = (parse_context_t*)vox_http_parser_get_user_data(p);
    int status_code = vox_http_parser_get_status_code(p);
    
    if (ctx && ctx->reason_phrase && len > 0) {
        vox_string_append_data(ctx->reason_phrase, data, len);
    }
    
    printf("[回调] 状态码: %d, 原因短语数据块 (%zu 字节): %.*s\n", 
           status_code, len, (int)len, data);
    return 0;
}

/* 回调函数：头部字段名（数据回调，可能多次调用） */
static int on_header_field(void* parser, const char* data, size_t len) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    parse_context_t* ctx = (parse_context_t*)vox_http_parser_get_user_data(p);
    
    if (ctx && ctx->current_header_name && len > 0) {
        /* 进入新 header name 前，先提交上一条 header（如果已经收到了 value） */
        if (ctx->current_header_value && vox_string_length(ctx->current_header_value) > 0) {
            commit_current_header(ctx);
        }
        vox_string_append_data(ctx->current_header_name, data, len);
    }
    
    printf("[回调] 头部字段名数据块 (%zu 字节): %.*s\n", len, (int)len, data);
    return 0;
}

/* 回调函数：头部字段值（数据回调，可能多次调用） */
static int on_header_value(void* parser, const char* data, size_t len) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    parse_context_t* ctx = (parse_context_t*)vox_http_parser_get_user_data(p);
    
    if (ctx && ctx->current_header_value && len > 0) {
        vox_string_append_data(ctx->current_header_value, data, len);
    }
    
    printf("[回调] 头部字段值数据块 (%zu 字节): %.*s\n", len, (int)len, data);
    return 0;
}

/* 回调函数：头部解析完成 */
static int on_headers_complete(void* parser) {
    vox_http_parser_t* p = (vox_http_parser_t*)parser;
    parse_context_t* ctx = (parse_context_t*)vox_http_parser_get_user_data(p);
    
    /* 提交最后一条 header */
    if (ctx) {
        commit_current_header(ctx);
    }
    
    printf("[回调] 头部解析完成\n");
    return 0;
}

/* 回调函数：消息体 */
static int on_body(void* parser, const char* data, size_t len) {
    VOX_UNUSED(parser);
    /* 解析器已经在回调之前处理好了数据：
     * - 数据长度准确（不超过 Content-Length 或 chunk 大小）
     * - 数据指针有效
     * - 不会超出范围
     * 可以直接使用，无需额外验证 */
    printf("[回调] 消息体 (%zu 字节): %.*s\n", len, (int)len, data);
    return 0;
}

/* 回调函数：消息完成 */
static int on_message_complete(void* parser) {
    VOX_UNUSED(parser);
    printf("[回调] 消息完成\n");
    return 0;
}

/* 回调函数：错误 */
static int on_error(void* parser, const char* message) {
    VOX_UNUSED(parser);
    printf("[回调] 错误: %s\n", message);
    return 0;
}

/* 创建回调结构（user_data将在每个示例中设置） */
static vox_http_callbacks_t callbacks = {
    .on_message_begin = on_message_begin,
    .on_url = on_url,
    .on_status = on_status,
    .on_header_field = on_header_field,
    .on_header_value = on_header_value,
    .on_headers_complete = on_headers_complete,
    .on_body = on_body,
    .on_message_complete = on_message_complete,
    .on_error = on_error,
    .user_data = NULL
};

/* 示例1：解析简单的 GET 请求 */
static void example_simple_get_request(void) {
    printf("\n=== 示例1：解析简单的 GET 请求 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }

    /* 创建解析上下文 */
    parse_context_t ctx;
    init_parse_context(&ctx, mpool);

    /* 创建回调结构（设置user_data） */
    vox_http_callbacks_t cb = callbacks;
    cb.user_data = &ctx;

    /* 创建解析器配置 */
    vox_http_parser_config_t config = {
        .type = VOX_HTTP_PARSER_TYPE_REQUEST,
        .max_header_size = 0,
        .max_headers = 0,
        .max_url_size = 0,
        .strict_mode = false
    };
    
    vox_http_parser_t* parser = vox_http_parser_create(mpool, &config, &cb);
    if (!parser) {
        fprintf(stderr, "创建解析器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_http_parser_set_user_data(parser, &ctx);
    
    const char* request = 
        "GET /index.html HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "User-Agent: Mozilla/5.0\r\n"
        "Accept: text/html\r\n"
        "\r\n";
    
    printf("解析请求:\n%s\n", request);
    
    ssize_t result = vox_http_parser_execute(parser, request, strlen(request));
    if (result < 0) {
        printf("解析失败: %s\n", vox_http_parser_get_error(parser));
    } else {
        printf("解析成功，已解析 %zd 字节\n", result);
        printf("方法: %s\n", method_to_string(vox_http_parser_get_method(parser)));
        printf("HTTP 版本: %d.%d\n", 
               vox_http_parser_get_http_major(parser),
               vox_http_parser_get_http_minor(parser));
        printf("完成: %s\n", vox_http_parser_is_complete(parser) ? "是" : "否");
    }
    
    vox_http_parser_destroy(parser);
    vox_mpool_destroy(mpool);
}

/* 示例2：解析 POST 请求（带消息体） */
static void example_post_request_with_body(void) {
    printf("\n=== 示例2：解析 POST 请求（带消息体） ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }

    /* 创建解析上下文 */
    parse_context_t ctx;
    init_parse_context(&ctx, mpool);

    /* 创建回调结构（设置user_data） */
    vox_http_callbacks_t cb = callbacks;
    cb.user_data = &ctx;

    /* 创建解析器配置 */
    vox_http_parser_config_t config = {
        .type = VOX_HTTP_PARSER_TYPE_REQUEST,
        .max_header_size = 0,
        .max_headers = 0,
        .max_url_size = 0,
        .strict_mode = false
    };

    vox_http_parser_t* parser = vox_http_parser_create(mpool, &config, &cb);
    if (!parser) {
        fprintf(stderr, "创建解析器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_http_parser_set_user_data(parser, &ctx);
    
    const char* request = 
        "POST /api/users HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 42\r\n"
        "\r\n"
        "{\"name\":\"John\",\"email\":\"john@example.com\"}";
    
    printf("解析请求:\n%s\n", request);
    
    ssize_t result = vox_http_parser_execute(parser, request, strlen(request));
    if (result < 0) {
        printf("解析失败: %s\n", vox_http_parser_get_error(parser));
    } else {
        printf("解析成功，已解析 %zd 字节\n", result);
        printf("方法: %s\n", method_to_string(vox_http_parser_get_method(parser)));
        
        /* 查找头部 */
        const char* content_type = find_header_in_context(&ctx, "Content-Type");
        if (content_type) {
            printf("Content-Type: %s\n", content_type);
        }
        
        printf("完成: %s\n", vox_http_parser_is_complete(parser) ? "是" : "否");
    }
    
    vox_http_parser_destroy(parser);
    vox_mpool_destroy(mpool);
}

/* 示例3：解析 HTTP 响应 */
static void example_http_response(void) {
    printf("\n=== 示例3：解析 HTTP 响应 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }

    /* 创建解析上下文 */
    parse_context_t ctx;
    init_parse_context(&ctx, mpool);

    /* 创建回调结构（设置user_data） */
    vox_http_callbacks_t cb = callbacks;
    cb.user_data = &ctx;

    vox_http_parser_config_t config = {
        .type = VOX_HTTP_PARSER_TYPE_RESPONSE
    };
    vox_http_parser_t* parser = vox_http_parser_create(mpool, &config, &cb);
    if (!parser) {
        fprintf(stderr, "创建解析器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_http_parser_set_user_data(parser, &ctx);
    
    const char* response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: 37\r\n"
        "Server: nginx/1.18.0\r\n"
        "Date: Mon, 20 Jan 2025 12:00:00 GMT\r\n"
        "\r\n"
        "<html><body>Hello World</body></html>";
    
    printf("解析响应:\n%s\n", response);
    
    ssize_t result = vox_http_parser_execute(parser, response, strlen(response));
    if (result < 0) {
        printf("解析失败: %s\n", vox_http_parser_get_error(parser));
    } else {
        printf("解析成功，已解析 %zd 字节\n", result);
        printf("状态码: %d\n", vox_http_parser_get_status_code(parser));
        printf("HTTP 版本: %d.%d\n", 
               vox_http_parser_get_http_major(parser),
               vox_http_parser_get_http_minor(parser));
        
        /* 获取所有头部 */
        if (ctx.headers) {
            printf("头部数量: %zu\n", vox_vector_size(ctx.headers));
            for (size_t i = 0; i < vox_vector_size(ctx.headers); i++) {
                saved_header_t* header = (saved_header_t*)vox_vector_get(ctx.headers, i);
                if (header && header->name && header->value) {
                    printf("  %s: %s\n", 
                           vox_string_cstr(header->name),
                           vox_string_cstr(header->value));
                }
            }
        }
        
        printf("完成: %s\n", vox_http_parser_is_complete(parser) ? "是" : "否");
    }
    
    vox_http_parser_destroy(parser);
    vox_mpool_destroy(mpool);
}

/* 示例4：解析 Chunked 编码响应 */
static void example_chunked_response(void) {
    printf("\n=== 示例4：解析 Chunked 编码响应 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }

    /* 创建解析上下文 */
    parse_context_t ctx;
    init_parse_context(&ctx, mpool);

    /* 创建回调结构（设置user_data） */
    vox_http_callbacks_t cb = callbacks;
    cb.user_data = &ctx;
    
    vox_http_parser_config_t config = {
        .type = VOX_HTTP_PARSER_TYPE_RESPONSE
    };
    vox_http_parser_t* parser = vox_http_parser_create(mpool, &config, &cb);
    if (!parser) {
        fprintf(stderr, "创建解析器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_http_parser_set_user_data(parser, &ctx);
    
    const char* response = 
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "5\r\n"
        "Hello\r\n"
        "6\r\n"
        " World\r\n"
        "0\r\n"
        "\r\n";
    
    printf("解析 Chunked 响应:\n%s\n", response);
    
    ssize_t result = vox_http_parser_execute(parser, response, strlen(response));
    if (result < 0) {
        printf("解析失败: %s\n", vox_http_parser_get_error(parser));
    } else {
        printf("解析成功，已解析 %zd 字节\n", result);
        printf("完成: %s\n", vox_http_parser_is_complete(parser) ? "是" : "否");
    }
    
    vox_http_parser_destroy(parser);
    vox_mpool_destroy(mpool);
}

/* 示例5：解析各种 HTTP 方法 */
static void example_various_methods(void) {
    printf("\n=== 示例5：解析各种 HTTP 方法 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }

    /* 创建解析上下文 */
    parse_context_t ctx;
    init_parse_context(&ctx, mpool);

    /* 创建回调结构（设置user_data） */
    vox_http_callbacks_t cb = callbacks;
    cb.user_data = &ctx;

    vox_http_parser_config_t config = {
        .type = VOX_HTTP_PARSER_TYPE_REQUEST
    };
    
    const char* requests[] = {
        "GET /resource HTTP/1.1\r\nHost: example.com\r\n\r\n",
        "POST /api/data HTTP/1.1\r\nHost: example.com\r\nContent-Length: 0\r\n\r\n",
        "PUT /api/users/1 HTTP/1.1\r\nHost: example.com\r\nContent-Length: 0\r\n\r\n",
        "DELETE /api/users/1 HTTP/1.1\r\nHost: example.com\r\n\r\n",
        "HEAD /resource HTTP/1.1\r\nHost: example.com\r\n\r\n",
        "OPTIONS /resource HTTP/1.1\r\nHost: example.com\r\n\r\n",
        "PATCH /api/users/1 HTTP/1.1\r\nHost: example.com\r\nContent-Length: 0\r\n\r\n",
        "TRACE /resource HTTP/1.1\r\nHost: example.com\r\n\r\n",
        "CONNECT proxy.example.com:443 HTTP/1.1\r\nHost: proxy.example.com\r\n\r\n"
    };
    
    const char* method_names[] = {
        "GET", "POST", "PUT", "DELETE", "HEAD", 
        "OPTIONS", "PATCH", "TRACE", "CONNECT"
    };
    
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        vox_http_parser_t* parser = vox_http_parser_create(mpool, &config, &cb);
        if (!parser) {
            continue;
        }
        
        vox_http_parser_set_user_data(parser, &ctx);
        
        ssize_t result = vox_http_parser_execute(parser, requests[i], strlen(requests[i]));
        if (result >= 0) {
            vox_http_method_t method = vox_http_parser_get_method(parser);
            printf("%s: %s\n", method_names[i], method_to_string(method));
        }
        
        vox_http_parser_destroy(parser);
    }
    
    vox_mpool_destroy(mpool);
}

/* 示例6：解析各种状态码 */
static void example_various_status_codes(void) {
    printf("\n=== 示例6：解析各种状态码 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }

    /* 创建解析上下文 */
    parse_context_t ctx;
    init_parse_context(&ctx, mpool);

    /* 创建回调结构（设置user_data） */
    vox_http_callbacks_t cb = callbacks;
    cb.user_data = &ctx;

    vox_http_parser_config_t config = {
        .type = VOX_HTTP_PARSER_TYPE_RESPONSE
    };
    
    const char* responses[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 301 Moved Permanently\r\nLocation: /new\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n"
    };
    
    int status_codes[] = {200, 201, 301, 400, 401, 404, 500, 503};
    
    for (size_t i = 0; i < sizeof(responses) / sizeof(responses[0]); i++) {
        vox_http_parser_t* parser = vox_http_parser_create(mpool, &config, &cb);
        if (!parser) {
            continue;
        }
        
        vox_http_parser_set_user_data(parser, &ctx);
        
        ssize_t result = vox_http_parser_execute(parser, responses[i], strlen(responses[i]));
        if (result >= 0) {
            int code = vox_http_parser_get_status_code(parser);
            printf("期望状态码: %d, 实际: %d %s\n", 
                   status_codes[i], code, (code == status_codes[i]) ? "✓" : "✗");
        }
        
        vox_http_parser_destroy(parser);
    }
    
    vox_mpool_destroy(mpool);
}

/* 示例7：分块解析（流式解析） */
static void example_streaming_parse(void) {
    printf("\n=== 示例7：分块解析（流式解析） ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }

    /* 创建解析上下文 */
    parse_context_t ctx;
    init_parse_context(&ctx, mpool);

    /* 创建回调结构（设置user_data） */
    vox_http_callbacks_t cb = callbacks;
    cb.user_data = &ctx;

    vox_http_parser_config_t config = {
        .type = VOX_HTTP_PARSER_TYPE_REQUEST
    };
    vox_http_parser_t* parser = vox_http_parser_create(mpool, &config, &cb);
    if (!parser) {
        fprintf(stderr, "创建解析器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_http_parser_set_user_data(parser, &ctx);
    
    /* 分块发送数据 */
    const char* chunks[] = {
        "GET /index.html HTTP/1.1\r\n",
        "Host: www.example.com\r\n",
        "User-Agent: Mozilla/5.0\r\n",
        "\r\n"
    };
    
    printf("分块解析请求:\n");
    size_t total_parsed = 0;
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        printf("  块 %zu: %s", i + 1, chunks[i]);
        ssize_t result = vox_http_parser_execute(parser, chunks[i], strlen(chunks[i]));
        if (result < 0) {
            printf("解析失败: %s\n", vox_http_parser_get_error(parser));
            break;
        }
        total_parsed += result;
        printf("    已解析 %zd 字节，完成: %s\n", 
               result, vox_http_parser_is_complete(parser) ? "是" : "否");
    }
    
    printf("总共解析 %zu 字节\n", total_parsed);
    if (vox_http_parser_is_complete(parser)) {
        printf("方法: %s\n", method_to_string(vox_http_parser_get_method(parser)));
    }
    
    vox_http_parser_destroy(parser);
    vox_mpool_destroy(mpool);
}

/* 示例8：解析器重置和重用 */
static void example_parser_reset(void) {
    printf("\n=== 示例8：解析器重置和重用 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }

    /* 创建解析上下文 */
    parse_context_t ctx;
    init_parse_context(&ctx, mpool);

    /* 创建回调结构（设置user_data） */
    vox_http_callbacks_t cb = callbacks;
    cb.user_data = &ctx;

    vox_http_parser_config_t config = {
        .type = VOX_HTTP_PARSER_TYPE_BOTH  /* 自动检测 */
    };
    vox_http_parser_t* parser = vox_http_parser_create(mpool, &config, &cb);
    if (!parser) {
        fprintf(stderr, "创建解析器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_http_parser_set_user_data(parser, &ctx);
    
    /* 解析请求 */
    const char* request = "GET /test HTTP/1.1\r\nHost: example.com\r\n\r\n";
    printf("解析请求: %s", request);
    ssize_t result = vox_http_parser_execute(parser, request, strlen(request));
    if (result >= 0) {
        printf("  方法: %s\n", method_to_string(vox_http_parser_get_method(parser)));
    }
    
    /* 重置解析器 */
    vox_http_parser_reset(parser);
    printf("解析器已重置\n");
    
    /* 解析响应 */
    const char* response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    printf("解析响应: %s", response);
    result = vox_http_parser_execute(parser, response, strlen(response));
    if (result >= 0) {
        printf("  状态码: %d\n", vox_http_parser_get_status_code(parser));
    }
    
    vox_http_parser_destroy(parser);
    vox_mpool_destroy(mpool);
}

/* 示例9：错误处理 */
static void example_error_handling(void) {
    printf("\n=== 示例9：错误处理 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }

    /* 创建解析上下文 */
    parse_context_t ctx;
    init_parse_context(&ctx, mpool);

    /* 创建回调结构（设置user_data） */
    vox_http_callbacks_t cb = callbacks;
    cb.user_data = &ctx;
    
    /* 测试无效的请求 */
    printf("--- 无效请求测试 ---\n");
    vox_http_parser_config_t config_req = {
        .type = VOX_HTTP_PARSER_TYPE_REQUEST
    };
    vox_http_parser_t* parser = vox_http_parser_create(mpool, &config_req, &cb);
    if (!parser) {
        fprintf(stderr, "创建解析器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_http_parser_set_user_data(parser, &ctx);
    
    struct {
        const char* name;
        const char* data;
    } invalid_requests[] = {
        {"无效的HTTP方法", "INVALID REQUEST\r\n"},
        {"缺少HTTP版本", "GET /test\r\n"},
        {"缺少CRLF", "GET /test HTTP/1.1"},
        {"缺少最后的CRLF", "GET /test HTTP/1.1\r\nHost: example.com"},
        {"无效的HTTP版本格式", "GET /test HTTP/2.0\r\n\r\n"},
        {"无效的HTTP版本字符", "GET /test HTTP/1.x\r\n\r\n"},
        {"URL包含无效字符", "GET /test<script>alert(1)</script> HTTP/1.1\r\n\r\n"},
        {"头部字段包含无效字符", "GET /test HTTP/1.1\r\nHost\x00: example.com\r\n\r\n"},
        {"头部值包含无效字符", "GET /test HTTP/1.1\r\nHost: example.com\x00\r\n\r\n"},
        {"Content-Length为负数", "POST /test HTTP/1.1\r\nContent-Length: -1\r\n\r\n"},
        {"Content-Length格式错误", "POST /test HTTP/1.1\r\nContent-Length: abc\r\n\r\n"},
        {"缺少方法", "/test HTTP/1.1\r\n\r\n"},
        {"缺少URL", "GET  HTTP/1.1\r\n\r\n"},
        {"URL包含控制字符", "GET /test\x01 HTTP/1.1\r\n\r\n"},
    };
    
    for (size_t i = 0; i < sizeof(invalid_requests) / sizeof(invalid_requests[0]); i++) {
        vox_http_parser_reset(parser);
        printf("测试 %zu: %s\n", i + 1, invalid_requests[i].name);
        printf("  数据: %s\n", invalid_requests[i].data);
        ssize_t result = vox_http_parser_execute(parser, invalid_requests[i].data, 
                                                  strlen(invalid_requests[i].data));
        if (result < 0 || vox_http_parser_has_error(parser)) {
            printf("  结果: 检测到错误 - %s\n", vox_http_parser_get_error(parser) ? 
                   vox_http_parser_get_error(parser) : "未知错误");
        } else {
            printf("  结果: 未检测到错误（可能不完整或已接受）\n");
        }
        printf("\n");
    }
    
    vox_http_parser_destroy(parser);
    
    /* 测试无效的响应 */
    printf("--- 无效响应测试 ---\n");
    vox_http_parser_config_t config_res = {
        .type = VOX_HTTP_PARSER_TYPE_RESPONSE
    };
    parser = vox_http_parser_create(mpool, &config_res, &cb);
    if (!parser) {
        fprintf(stderr, "创建解析器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_http_parser_set_user_data(parser, &ctx);
    
    struct {
        const char* name;
        const char* data;
    } invalid_responses[] = {
        {"无效的HTTP版本", "HTTP/2.0 200 OK\r\n\r\n"},
        {"缺少状态码", "HTTP/1.1  OK\r\n\r\n"},
        {"状态码格式错误", "HTTP/1.1 2xx OK\r\n\r\n"},
        {"状态码为负数", "HTTP/1.1 -1 OK\r\n\r\n"},
        {"缺少原因短语", "HTTP/1.1 200\r\n\r\n"},
        {"原因短语包含无效字符", "HTTP/1.1 200 OK\x00\r\n\r\n"},
        {"Chunked编码格式错误", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nHello\r\n"},
        {"Chunk大小格式错误", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nabc\r\nHello\r\n"},
        {"Chunk大小超出范围", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n99999999999999999999\r\nHello\r\n"},
        {"Content-Length与数据不匹配", "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nHello"},
    };
    
    for (size_t i = 0; i < sizeof(invalid_responses) / sizeof(invalid_responses[0]); i++) {
        vox_http_parser_reset(parser);
        printf("测试 %zu: %s\n", i + 1, invalid_responses[i].name);
        printf("  数据: %s\n", invalid_responses[i].data);
        ssize_t result = vox_http_parser_execute(parser, invalid_responses[i].data, 
                                                  strlen(invalid_responses[i].data));
        if (result < 0 || vox_http_parser_has_error(parser)) {
            printf("  结果: 检测到错误 - %s\n", vox_http_parser_get_error(parser) ? 
                   vox_http_parser_get_error(parser) : "未知错误");
        } else {
            printf("  结果: 未检测到错误（可能不完整或已接受）\n");
        }
        printf("\n");
    }
    
    vox_http_parser_destroy(parser);
    
    /* 测试边界情况 */
    printf("--- 边界情况测试 ---\n");
    parser = vox_http_parser_create(mpool, &config_req, &cb);
    if (!parser) {
        fprintf(stderr, "创建解析器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_http_parser_set_user_data(parser, &ctx);
    
    /* 构造超长URL和头部 */
    char long_url[2048];
    char long_header[2048];
    memset(long_url, 'x', sizeof(long_url) - 1);
    long_url[sizeof(long_url) - 1] = '\0';
    memset(long_header, 'y', sizeof(long_header) - 1);
    long_header[sizeof(long_header) - 1] = '\0';
    
    char long_url_request[4096];
    char long_header_request[4096];
    snprintf(long_url_request, sizeof(long_url_request), "GET /%s HTTP/1.1\r\n\r\n", long_url);
    snprintf(long_header_request, sizeof(long_header_request), "GET /test HTTP/1.1\r\nHost: %s\r\n\r\n", long_header);
    
    struct {
        const char* name;
        const char* data;
    } edge_cases[] = {
        {"空请求", ""},
        {"只有换行符", "\r\n"},
        {"只有空格", "   "},
        {"超长URL", long_url_request},
        {"超长头部值", long_header_request},
        {"多个连续CRLF", "GET /test HTTP/1.1\r\n\r\n\r\n\r\n"},
        {"头部值包含换行符", "GET /test HTTP/1.1\r\nHost: example.com\r\ntest\r\n\r\n"},
        {"URL包含空格（未编码）", "GET /test path HTTP/1.1\r\n\r\n"},
        {"头部字段名包含空格", "GET /test HTTP/1.1\r\nHost Name: example.com\r\n\r\n"},
        {"头部值前有多余空格", "GET /test HTTP/1.1\r\nHost:  example.com\r\n\r\n"},
    };
    
    for (size_t i = 0; i < sizeof(edge_cases) / sizeof(edge_cases[0]); i++) {
        vox_http_parser_reset(parser);
        printf("测试 %zu: %s\n", i + 1, edge_cases[i].name);
        if (strlen(edge_cases[i].data) > 0) {
            printf("  数据: %s\n", edge_cases[i].data);
        } else {
            printf("  数据: (空)\n");
        }
        ssize_t result = vox_http_parser_execute(parser, edge_cases[i].data, 
                                                  strlen(edge_cases[i].data));
        if (result < 0 || vox_http_parser_has_error(parser)) {
            printf("  结果: 检测到错误 - %s\n", vox_http_parser_get_error(parser) ? 
                   vox_http_parser_get_error(parser) : "未知错误");
        } else {
            printf("  结果: 未检测到错误（可能不完整或已接受）\n");
        }
        printf("\n");
    }
    
    vox_http_parser_destroy(parser);
    vox_mpool_destroy(mpool);
}

/* 示例10：HTTP/1.0 和 HTTP/1.1 */
static void example_http_versions(void) {
    printf("\n=== 示例10：HTTP/1.0 和 HTTP/1.1 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }

    /* 创建解析上下文 */
    parse_context_t ctx;
    init_parse_context(&ctx, mpool);

    /* 创建回调结构（设置user_data） */
    vox_http_callbacks_t cb = callbacks;
    cb.user_data = &ctx;

    vox_http_parser_config_t config = {
        .type = VOX_HTTP_PARSER_TYPE_REQUEST
    };
    
    const char* requests[] = {
        "GET /test HTTP/1.0\r\nHost: example.com\r\n\r\n",
        "GET /test HTTP/1.1\r\nHost: example.com\r\n\r\n"
    };
    
    const char* versions[] = {"HTTP/1.0", "HTTP/1.1"};
    
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        vox_http_parser_t* parser = vox_http_parser_create(mpool, &config, &cb);
        if (!parser) {
            continue;
        }
        
        vox_http_parser_set_user_data(parser, &ctx);
        
        ssize_t result = vox_http_parser_execute(parser, requests[i], strlen(requests[i]));
        if (result >= 0) {
            int major = vox_http_parser_get_http_major(parser);
            int minor = vox_http_parser_get_http_minor(parser);
            printf("%s: HTTP/%d.%d\n", versions[i], major, minor);
        }
        
        vox_http_parser_destroy(parser);
    }
    
    vox_mpool_destroy(mpool);
}

/* 示例11：复杂头部 */
static void example_complex_headers(void) {
    printf("\n=== 示例11：复杂头部 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }

    /* 创建解析上下文 */
    parse_context_t ctx;
    init_parse_context(&ctx, mpool);

    /* 创建回调结构（设置user_data） */
    vox_http_callbacks_t cb = callbacks;
    cb.user_data = &ctx;

    vox_http_parser_config_t config = {
        .type = VOX_HTTP_PARSER_TYPE_REQUEST
    };
    vox_http_parser_t* parser = vox_http_parser_create(mpool, &config, &cb);
    if (!parser) {
        fprintf(stderr, "创建解析器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    vox_http_parser_set_user_data(parser, &ctx);
    
    const char* request = 
        "GET /api/data HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
        "Accept-Language: en-US,en;q=0.5\r\n"
        "Accept-Encoding: gzip, deflate, br\r\n"
        "Connection: keep-alive\r\n"
        "Cookie: session=abc123; user=john\r\n"
        "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9\r\n"
        "\r\n";
    
    printf("解析复杂头部请求:\n%s\n", request);
    
    ssize_t result = vox_http_parser_execute(parser, request, strlen(request));
    if (result >= 0) {
        if (ctx.headers) {
            printf("头部数量: %zu\n", vox_vector_size(ctx.headers));
            for (size_t i = 0; i < vox_vector_size(ctx.headers); i++) {
                saved_header_t* header = (saved_header_t*)vox_vector_get(ctx.headers, i);
                if (header && header->name && header->value) {
                    printf("  %s: %s\n", 
                           vox_string_cstr(header->name),
                           vox_string_cstr(header->value));
                }
            }
        }
        
        /* 查找特定头部 */
        const char* cookie = find_header_in_context(&ctx, "Cookie");
        if (cookie) {
            printf("Cookie: %s\n", cookie);
        }
    }
    
    vox_http_parser_destroy(parser);
    vox_mpool_destroy(mpool);
}

int main(int argc, char* argv[]) {
    printf("=== vox_http_parser 使用示例 ===\n");
    printf("演示 HTTP 请求和响应的解析，覆盖各种场景\n\n");
    
    if (argc > 1) {
        int example_num = atoi(argv[1]);
        switch (example_num) {
            case 1: example_simple_get_request(); break;
            case 2: example_post_request_with_body(); break;
            case 3: example_http_response(); break;
            case 4: example_chunked_response(); break;
            case 5: example_various_methods(); break;
            case 6: example_various_status_codes(); break;
            case 7: example_streaming_parse(); break;
            case 8: example_parser_reset(); break;
            case 9: example_error_handling(); break;
            case 10: example_http_versions(); break;
            case 11: example_complex_headers(); break;
            default:
                fprintf(stderr, "未知示例编号: %d\n", example_num);
                return 1;
        }
    } else {
        /* 运行所有示例 */
        example_simple_get_request();
        example_post_request_with_body();
        example_http_response();
        example_chunked_response();
        example_various_methods();
        example_various_status_codes();
        example_streaming_parse();
        example_parser_reset();
        example_error_handling();
        example_http_versions();
        example_complex_headers();
    }
    
    printf("\n所有示例完成！\n");
    return 0;
}
