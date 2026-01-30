/*
 * http_middleware_example.c - HTTP 中间件使用示例
 */

#include "../vox_loop.h"
#include "../vox_socket.h"
#include "../vox_log.h"

#include "../http/vox_http_engine.h"
#include "../http/vox_http_server.h"
#include "../http/vox_http_context.h"
#include "../http/vox_http_middleware.h"

#include <stdio.h>
#include <string.h>

/* 示例处理器 */
static void hello_handler(vox_http_context_t* ctx) {
    vox_http_context_status(ctx, 200);
    vox_http_context_write_cstr(ctx, "Hello, World!\n");
}

static void api_handler(vox_http_context_t* ctx) {
    vox_http_context_status(ctx, 200);
    vox_http_context_header(ctx, "Content-Type", "application/json");
    vox_http_context_write_cstr(ctx, "{\"message\": \"API endpoint\"}\n");
}

static void protected_handler(vox_http_context_t* ctx) {
    vox_http_context_status(ctx, 200);
    vox_http_context_write_cstr(ctx, "This is a protected resource\n");
}

/* Bearer Token 验证函数示例 */
static bool token_validator(const char* token, void* user_data) {
    VOX_UNUSED(user_data);
    /* 简单的 token 验证：检查是否是 "secret-token" */
    return (token && strcmp(token, "secret-token") == 0);
}

int main(void) {
    if (vox_socket_init() != 0) {
        fprintf(stderr, "vox_socket_init failed\n");
        return 1;
    }

    vox_log_set_level(VOX_LOG_INFO);

    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "vox_loop_create failed\n");
        return 1;
    }

    vox_http_engine_t* engine = vox_http_engine_create(loop);
    if (!engine) {
        fprintf(stderr, "vox_http_engine_create failed\n");
        return 1;
    }

    /* 获取 engine 的内存池用于中间件配置 */
    vox_mpool_t* mpool = vox_http_engine_get_mpool(engine);

    /* 添加全局中间件 */
    vox_http_engine_use(engine, vox_http_middleware_logger);      /* 日志中间件 */
    vox_http_engine_use(engine, vox_http_middleware_cors);        /* CORS 中间件 */
    vox_http_engine_use(engine, vox_http_middleware_error_handler); /* 错误处理中间件 */

    /* 添加路由 */
    {
        vox_http_handler_cb hs[] = { hello_handler };
        vox_http_engine_get(engine, "/hello", hs, sizeof(hs) / sizeof(hs[0]));
    }

    {
        vox_http_handler_cb hs[] = { api_handler };
        vox_http_engine_get(engine, "/api", hs, sizeof(hs) / sizeof(hs[0]));
    }

    /* 创建 Basic Auth 中间件 */
    vox_http_basic_auth_config_t basic_auth_config = {
        .username = "admin",
        .password = "password",
        .realm = "Protected Area"
    };
    vox_http_handler_cb basic_auth_mw = vox_http_middleware_basic_auth_create(mpool, &basic_auth_config);
    if (basic_auth_mw) {
        vox_http_handler_cb hs[] = { basic_auth_mw, protected_handler };
        vox_http_engine_get(engine, "/protected/basic", hs, sizeof(hs) / sizeof(hs[0]));
    }

    /* 创建 Bearer Token 认证中间件 */
    vox_http_bearer_auth_config_t bearer_auth_config = {
        .validator = token_validator,
        .validator_data = NULL,
        .realm = "API"
    };
    vox_http_handler_cb bearer_auth_mw = vox_http_middleware_bearer_auth_create(mpool, &bearer_auth_config);
    if (bearer_auth_mw) {
        vox_http_handler_cb hs[] = { bearer_auth_mw, protected_handler };
        vox_http_engine_get(engine, "/protected/bearer", hs, sizeof(hs) / sizeof(hs[0]));
    }

    /* 创建请求体大小限制中间件（限制为 1MB） */
    vox_http_handler_cb body_limit_mw = vox_http_middleware_body_limit_create(mpool, 1024 * 1024);
    if (body_limit_mw) {
        vox_http_handler_cb hs[] = { body_limit_mw, api_handler };
        vox_http_engine_post(engine, "/api/upload", hs, sizeof(hs) / sizeof(hs[0]));
    }

    /* 创建限流中间件（每秒最多10个请求） */
    vox_http_rate_limit_config_t rate_limit_config = {
        .max_requests = 10,
        .window_ms = 1000,  /* 1秒 */
        .message = "Rate limit exceeded. Please try again later."
    };
    vox_http_handler_cb rate_limit_mw = vox_http_middleware_rate_limit_create(mpool, &rate_limit_config);
    if (rate_limit_mw) {
        vox_http_handler_cb hs[] = { rate_limit_mw, hello_handler };
        vox_http_engine_get(engine, "/rate-limited", hs, sizeof(hs) / sizeof(hs[0]));
    }

    vox_http_server_t* server = vox_http_server_create(engine);
    if (!server) {
        fprintf(stderr, "vox_http_server_create failed\n");
        return 1;
    }

    vox_socket_addr_t addr;
    if (vox_socket_parse_address("0.0.0.0", 8080, &addr) != 0) {
        fprintf(stderr, "vox_socket_parse_address failed\n");
        return 1;
    }

    if (vox_http_server_listen_tcp(server, &addr, 128) != 0) {
        fprintf(stderr, "listen tcp failed\n");
        return 1;
    }

    VOX_LOG_INFO("HTTP server with middleware listening on 0.0.0.0:8080");
    VOX_LOG_INFO("Endpoints:");
    VOX_LOG_INFO("  GET  /hello - Public endpoint");
    VOX_LOG_INFO("  GET  /api - Public API endpoint");
    VOX_LOG_INFO("  GET  /protected/basic - Basic Auth protected (admin:password)");
    VOX_LOG_INFO("  GET  /protected/bearer - Bearer Token protected (token: secret-token)");
    VOX_LOG_INFO("  POST /api/upload - Body limit 1MB");
    VOX_LOG_INFO("  GET  /rate-limited - Rate limited (10 req/s per IP)");
    
    return vox_loop_run(loop, VOX_RUN_DEFAULT);
}
