/*
 * test_http_parser_refactored.c - HTTP 解析器重构验证测试
 * 
 * 本测试文件验证重构后的 HTTP 解析器是否正确工作，包括：
 * 1. 基本功能测试（请求行、头部、消息体）
 * 2. 流式解析测试（增量输入）
 * 3. 边界条件测试
 * 4. 错误处理测试
 * 5. 性能对比测试（重构前后）
 */

#include "../http/vox_http_parser.h"
#include "../http/vox_http_parser_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ===== 测试辅助函数 ===== */

typedef struct {
    int message_begin_called;
    int message_complete_called;
    int headers_complete_called;
    
    char method[32];
    char url[256];
    char headers[10][2][256];  /* [index][0=name, 1=value] */
    int header_count;
    
    char body[4096];
    size_t body_len;
    
    int http_major;
    int http_minor;
} test_context_t;

static int on_message_begin(void* data) {
    test_context_t* ctx = (test_context_t*)data;
    ctx->message_begin_called = 1;
    printf("  [CB] on_message_begin\n");
    return 0;
}

static int on_url(void* data, const char* at, size_t length) {
    test_context_t* ctx = (test_context_t*)data;
    strncat(ctx->url, at, length);
    printf("  [CB] on_url: \"%.*s\"\n", (int)length, at);
    return 0;
}

static int on_header_field(void* data, const char* at, size_t length) {
    test_context_t* ctx = (test_context_t*)data;
    int idx = ctx->header_count;
    strncat(ctx->headers[idx][0], at, length);
    printf("  [CB] on_header_field: \"%.*s\"\n", (int)length, at);
    return 0;
}

static int on_header_value(void* data, const char* at, size_t length) {
    test_context_t* ctx = (test_context_t*)data;
    int idx = ctx->header_count;
    strncat(ctx->headers[idx][1], at, length);
    printf("  [CB] on_header_value: \"%.*s\"\n", (int)length, at);
    ctx->header_count++;
    return 0;
}

static int on_headers_complete(void* data) {
    test_context_t* ctx = (test_context_t*)data;
    vox_http_parser_t* parser = (vox_http_parser_t*)((char*)data - offsetof(vox_http_parser_t, userdata));
    
    ctx->headers_complete_called = 1;
    ctx->http_major = parser->http_major;
    ctx->http_minor = parser->http_minor;
    
    printf("  [CB] on_headers_complete (HTTP/%d.%d)\n", 
           parser->http_major, parser->http_minor);
    return 0;
}

static int on_body(void* data, const char* at, size_t length) {
    test_context_t* ctx = (test_context_t*)data;
    
    if (ctx->body_len + length < sizeof(ctx->body)) {
        memcpy(ctx->body + ctx->body_len, at, length);
        ctx->body_len += length;
    }
    
    printf("  [CB] on_body: %zu bytes\n", length);
    return 0;
}

static int on_message_complete(void* data) {
    test_context_t* ctx = (test_context_t*)data;
    ctx->message_complete_called = 1;
    printf("  [CB] on_message_complete\n");
    return 0;
}

static void setup_callbacks(vox_http_parser_t* parser) {
    parser->callbacks.on_message_begin = on_message_begin;
    parser->callbacks.on_url = on_url;
    parser->callbacks.on_header_field = on_header_field;
    parser->callbacks.on_header_value = on_header_value;
    parser->callbacks.on_headers_complete = on_headers_complete;
    parser->callbacks.on_body = on_body;
    parser->callbacks.on_message_complete = on_message_complete;
}

static void reset_test_context(test_context_t* ctx) {
    memset(ctx, 0, sizeof(test_context_t));
}

/* ===== 测试用例 ===== */

/**
 * 测试 1: 基本的 GET 请求解析
 */
static void test_basic_get_request(void) {
    printf("\n[TEST] Basic GET Request\n");
    
    const char* request = 
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: TestClient/1.0\r\n"
        "\r\n";
    
    vox_http_parser_t parser;
    test_context_t ctx;
    
    vox_http_parser_init(&parser, VOX_HTTP_REQUEST);
    setup_callbacks(&parser);
    parser.userdata = &ctx;
    reset_test_context(&ctx);
    
    ssize_t consumed = vox_http_parser_execute_refactored(&parser, request, strlen(request));
    
    printf("  Consumed: %zd bytes\n", consumed);
    printf("  URL: %s\n", ctx.url);
    printf("  HTTP Version: %d.%d\n", ctx.http_major, ctx.http_minor);
    printf("  Headers:\n");
    for (int i = 0; i < ctx.header_count; i++) {
        printf("    %s: %s\n", ctx.headers[i][0], ctx.headers[i][1]);
    }
    
    /* 验证 */
    assert(consumed == strlen(request));
    assert(ctx.message_begin_called == 1);
    assert(ctx.headers_complete_called == 1);
    assert(ctx.message_complete_called == 1);
    assert(strcmp(ctx.url, "/index.html") == 0);
    assert(ctx.http_major == 1 && ctx.http_minor == 1);
    assert(ctx.header_count == 2);
    
    printf("  [PASS]\n");
}

/**
 * 测试 2: POST 请求，带消息体
 */
static void test_post_with_body(void) {
    printf("\n[TEST] POST Request with Body\n");
    
    const char* request = 
        "POST /api/data HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello, World!";
    
    vox_http_parser_t parser;
    test_context_t ctx;
    
    vox_http_parser_init(&parser, VOX_HTTP_REQUEST);
    setup_callbacks(&parser);
    parser.userdata = &ctx;
    reset_test_context(&ctx);
    
    ssize_t consumed = vox_http_parser_execute_refactored(&parser, request, strlen(request));
    
    printf("  Consumed: %zd bytes\n", consumed);
    printf("  Body: \"%.*s\" (%zu bytes)\n", (int)ctx.body_len, ctx.body, ctx.body_len);
    
    /* 验证 */
    assert(consumed == strlen(request));
    assert(ctx.message_complete_called == 1);
    assert(ctx.body_len == 13);
    assert(memcmp(ctx.body, "Hello, World!", 13) == 0);
    
    printf("  [PASS]\n");
}

/**
 * 测试 3: 流式解析（分多次输入）
 */
static void test_incremental_parsing(void) {
    printf("\n[TEST] Incremental Parsing\n");
    
    const char* chunks[] = {
        "GET /",
        "test",
        ".html ",
        "HTTP/1",
        ".1\r\n",
        "Host: exam",
        "ple.com\r\n",
        "\r\n",
        NULL
    };
    
    vox_http_parser_t parser;
    test_context_t ctx;
    
    vox_http_parser_init(&parser, VOX_HTTP_REQUEST);
    setup_callbacks(&parser);
    parser.userdata = &ctx;
    reset_test_context(&ctx);
    
    size_t total_consumed = 0;
    
    for (int i = 0; chunks[i] != NULL; i++) {
        printf("  Chunk %d: \"%s\"\n", i, chunks[i]);
        ssize_t consumed = vox_http_parser_execute_refactored(&parser, chunks[i], strlen(chunks[i]));
        
        if (consumed < 0) {
            printf("  [ERROR] Parse failed at chunk %d\n", i);
            assert(0);
        }
        
        total_consumed += consumed;
    }
    
    printf("  Total consumed: %zu bytes\n", total_consumed);
    printf("  Final URL: %s\n", ctx.url);
    
    /* 验证 */
    assert(ctx.message_complete_called == 1);
    assert(strcmp(ctx.url, "/test.html") == 0);
    
    printf("  [PASS]\n");
}

/**
 * 测试 4: Chunked 编码
 */
static void test_chunked_encoding(void) {
    printf("\n[TEST] Chunked Encoding\n");
    
    const char* request = 
        "POST /upload HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "Hello\r\n"
        "7\r\n"
        ", World\r\n"
        "0\r\n"
        "\r\n";
    
    vox_http_parser_t parser;
    test_context_t ctx;
    
    vox_http_parser_init(&parser, VOX_HTTP_REQUEST);
    setup_callbacks(&parser);
    parser.userdata = &ctx;
    reset_test_context(&ctx);
    
    ssize_t consumed = vox_http_parser_execute_refactored(&parser, request, strlen(request));
    
    printf("  Consumed: %zd bytes\n", consumed);
    printf("  Body: \"%.*s\" (%zu bytes)\n", (int)ctx.body_len, ctx.body, ctx.body_len);
    
    /* 验证 */
    assert(consumed == strlen(request));
    assert(ctx.message_complete_called == 1);
    assert(ctx.body_len == 12);
    assert(memcmp(ctx.body, "Hello, World", 12) == 0);
    
    printf("  [PASS]\n");
}

/**
 * 测试 5: 错误处理 - 无效的方法
 */
static void test_error_invalid_method(void) {
    printf("\n[TEST] Error Handling - Invalid Method\n");
    
    const char* request = "INVALID!@# /test HTTP/1.1\r\n\r\n";
    
    vox_http_parser_t parser;
    test_context_t ctx;
    
    vox_http_parser_init(&parser, VOX_HTTP_REQUEST);
    setup_callbacks(&parser);
    parser.userdata = &ctx;
    reset_test_context(&ctx);
    parser.strict_mode = 1;
    
    ssize_t consumed = vox_http_parser_execute_refactored(&parser, request, strlen(request));
    
    printf("  Consumed: %zd bytes\n", consumed);
    printf("  Has Error: %d\n", parser.has_error);
    
    /* 验证 */
    assert(consumed < 0 || parser.has_error);
    
    printf("  [PASS]\n");
}

/**
 * 测试 6: 大请求（测试缓冲区管理）
 */
static void test_large_request(void) {
    printf("\n[TEST] Large Request\n");
    
    /* 构造一个大的 URL */
    char request[4096];
    int offset = 0;
    
    offset += snprintf(request + offset, sizeof(request) - offset, "GET /");
    
    /* 添加 1000 个字符的路径 */
    for (int i = 0; i < 100; i++) {
        offset += snprintf(request + offset, sizeof(request) - offset, "segment%d/", i);
    }
    
    offset += snprintf(request + offset, sizeof(request) - offset, " HTTP/1.1\r\n");
    offset += snprintf(request + offset, sizeof(request) - offset, "Host: example.com\r\n");
    offset += snprintf(request + offset, sizeof(request) - offset, "\r\n");
    
    vox_http_parser_t parser;
    test_context_t ctx;
    
    vox_http_parser_init(&parser, VOX_HTTP_REQUEST);
    setup_callbacks(&parser);
    parser.userdata = &ctx;
    reset_test_context(&ctx);
    
    ssize_t consumed = vox_http_parser_execute_refactored(&parser, request, offset);
    
    printf("  Request size: %d bytes\n", offset);
    printf("  Consumed: %zd bytes\n", consumed);
    printf("  URL length: %zu\n", strlen(ctx.url));
    
    /* 验证 */
    assert(consumed == offset);
    assert(ctx.message_complete_called == 1);
    
    printf("  [PASS]\n");
}

/**
 * 测试 7: 性能基准测试
 */
static void test_performance_benchmark(void) {
    printf("\n[TEST] Performance Benchmark\n");
    
    const char* request = 
        "GET /benchmark HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: Benchmark/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    
    const int iterations = 100000;
    
    vox_http_parser_t parser;
    test_context_t ctx;
    
    vox_http_parser_init(&parser, VOX_HTTP_REQUEST);
    setup_callbacks(&parser);
    parser.userdata = &ctx;
    
    printf("  Parsing %d requests...\n", iterations);
    
    /* 开始计时 */
    clock_t start = clock();
    
    for (int i = 0; i < iterations; i++) {
        vox_http_parser_reset(&parser);
        reset_test_context(&ctx);
        
        ssize_t consumed = vox_http_parser_execute_refactored(&parser, request, strlen(request));
        assert(consumed == strlen(request));
    }
    
    clock_t end = clock();
    
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double req_per_sec = iterations / elapsed;
    double mb_per_sec = (iterations * strlen(request)) / (elapsed * 1024 * 1024);
    
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Throughput: %.2f req/sec\n", req_per_sec);
    printf("  Bandwidth: %.2f MB/sec\n", mb_per_sec);
    
    printf("  [PASS]\n");
}

/* ===== 主函数 ===== */

int main(void) {
    printf("=====================================\n");
    printf("HTTP Parser Refactored - Unit Tests\n");
    printf("=====================================\n");
    
    test_basic_get_request();
    test_post_with_body();
    test_incremental_parsing();
    test_chunked_encoding();
    test_error_invalid_method();
    test_large_request();
    test_performance_benchmark();
    
    printf("\n=====================================\n");
    printf("All Tests Passed! ✓\n");
    printf("=====================================\n");
    
    return 0;
}
