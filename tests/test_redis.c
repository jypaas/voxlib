/*
 * test_redis.c - Redis 客户端单元测试
 */

#include "../redis/vox_redis_client.h"
#include "../redis/vox_redis_parser.h"
#include "../redis/vox_redis_pool.h"
#include "../vox_loop.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ===== 辅助回调（用于错误路径测试）===== */

static int g_error_called = 0;

static void test_err_cb(vox_redis_client_t* c, const char* msg, void* ud) {
    (void)c;
    (void)msg;
    int* p = (int*)ud;
    (*p)++;
    g_error_called++;
}

static void test_resp_unexpected(vox_redis_client_t* c, const vox_redis_response_t* r, void* ud) {
    (void)c;
    (void)r;
    (void)ud;
    assert(!"unexpected response callback");
}

/* ===== RESP 解析器测试 ===== */

static void test_parser_simple_string() {
    printf("Testing RESP parser - Simple String... ");
    
    vox_mpool_t* mpool = vox_mpool_create();
    assert(mpool != NULL);
    
    vox_redis_parser_config_t config = {0};
    vox_redis_parser_callbacks_t callbacks = {0};
    
    int simple_string_called = 0;
    callbacks.on_simple_string = NULL;  /* 简化测试 */
    callbacks.user_data = &simple_string_called;
    
    vox_redis_parser_t* parser = vox_redis_parser_create(mpool, &config, &callbacks);
    assert(parser != NULL);
    
    const char* input = "+OK\r\n";
    ssize_t n = vox_redis_parser_execute(parser, input, strlen(input));
    assert(n > 0);
    assert(vox_redis_parser_is_complete(parser));
    
    vox_redis_parser_destroy(parser);
    vox_mpool_destroy(mpool);
    
    printf("PASSED\n");
}

static void test_parser_bulk_string() {
    printf("Testing RESP parser - Bulk String... ");
    
    vox_mpool_t* mpool = vox_mpool_create();
    assert(mpool != NULL);
    
    vox_redis_parser_config_t config = {0};
    vox_redis_parser_callbacks_t callbacks = {0};
    
    vox_redis_parser_t* parser = vox_redis_parser_create(mpool, &config, &callbacks);
    assert(parser != NULL);
    
    const char* input = "$5\r\nhello\r\n";
    ssize_t n = vox_redis_parser_execute(parser, input, strlen(input));
    assert(n > 0);
    assert(vox_redis_parser_is_complete(parser));
    
    vox_redis_parser_destroy(parser);
    vox_mpool_destroy(mpool);
    
    printf("PASSED\n");
}

static void test_parser_null_bulk_string() {
    printf("Testing RESP parser - Null Bulk String... ");
    
    vox_mpool_t* mpool = vox_mpool_create();
    assert(mpool != NULL);
    
    vox_redis_parser_config_t config = {0};
    vox_redis_parser_callbacks_t callbacks = {0};
    
    vox_redis_parser_t* parser = vox_redis_parser_create(mpool, &config, &callbacks);
    assert(parser != NULL);
    
    const char* input = "$-1\r\n";
    ssize_t n = vox_redis_parser_execute(parser, input, strlen(input));
    assert(n > 0);
    assert(vox_redis_parser_is_complete(parser));
    
    vox_redis_parser_destroy(parser);
    vox_mpool_destroy(mpool);
    
    printf("PASSED\n");
}

static void test_parser_integer() {
    printf("Testing RESP parser - Integer... ");
    
    vox_mpool_t* mpool = vox_mpool_create();
    assert(mpool != NULL);
    
    vox_redis_parser_config_t config = {0};
    vox_redis_parser_callbacks_t callbacks = {0};
    
    vox_redis_parser_t* parser = vox_redis_parser_create(mpool, &config, &callbacks);
    assert(parser != NULL);
    
    const char* input = ":1234\r\n";
    ssize_t n = vox_redis_parser_execute(parser, input, strlen(input));
    assert(n > 0);
    assert(vox_redis_parser_is_complete(parser));
    
    vox_redis_parser_destroy(parser);
    vox_mpool_destroy(mpool);
    
    printf("PASSED\n");
}

static void test_parser_array() {
    printf("Testing RESP parser - Array... ");
    
    vox_mpool_t* mpool = vox_mpool_create();
    assert(mpool != NULL);
    
    vox_redis_parser_config_t config = {0};
    vox_redis_parser_callbacks_t callbacks = {0};
    
    vox_redis_parser_t* parser = vox_redis_parser_create(mpool, &config, &callbacks);
    assert(parser != NULL);
    
    const char* input = "*2\r\n$5\r\nhello\r\n$5\r\nworld\r\n";
    ssize_t n = vox_redis_parser_execute(parser, input, strlen(input));
    assert(n > 0);
    assert(vox_redis_parser_is_complete(parser));
    
    vox_redis_parser_destroy(parser);
    vox_mpool_destroy(mpool);
    
    printf("PASSED\n");
}

static void test_parser_error() {
    printf("Testing RESP parser - Error... ");
    
    vox_mpool_t* mpool = vox_mpool_create();
    assert(mpool != NULL);
    
    vox_redis_parser_config_t config = {0};
    vox_redis_parser_callbacks_t callbacks = {0};
    
    vox_redis_parser_t* parser = vox_redis_parser_create(mpool, &config, &callbacks);
    assert(parser != NULL);
    
    const char* input = "-ERR unknown command\r\n";
    ssize_t n = vox_redis_parser_execute(parser, input, strlen(input));
    assert(n > 0);
    assert(vox_redis_parser_is_complete(parser));
    
    vox_redis_parser_destroy(parser);
    vox_mpool_destroy(mpool);
    
    printf("PASSED\n");
}

static void test_parser_nested_array() {
    printf("Testing RESP parser - Nested Array... ");
    
    vox_mpool_t* mpool = vox_mpool_create();
    assert(mpool != NULL);
    
    vox_redis_parser_config_t config = {0};
    vox_redis_parser_callbacks_t callbacks = {0};
    
    vox_redis_parser_t* parser = vox_redis_parser_create(mpool, &config, &callbacks);
    assert(parser != NULL);
    
    /* 嵌套数组: [[1, 2], [3, 4]] */
    const char* input = "*2\r\n*2\r\n:1\r\n:2\r\n*2\r\n:3\r\n:4\r\n";
    ssize_t n = vox_redis_parser_execute(parser, input, strlen(input));
    assert(n > 0);
    assert(vox_redis_parser_is_complete(parser));
    
    vox_redis_parser_destroy(parser);
    vox_mpool_destroy(mpool);
    
    printf("PASSED\n");
}

static void test_parser_incremental() {
    printf("Testing RESP parser - Incremental Parsing... ");
    
    vox_mpool_t* mpool = vox_mpool_create();
    assert(mpool != NULL);
    
    vox_redis_parser_config_t config = {0};
    vox_redis_parser_callbacks_t callbacks = {0};
    
    vox_redis_parser_t* parser = vox_redis_parser_create(mpool, &config, &callbacks);
    assert(parser != NULL);
    
    /* 分多次解析 */
    const char* input1 = "+OK";
    const char* input2 = "\r\n";
    
    ssize_t n1 = vox_redis_parser_execute(parser, input1, strlen(input1));
    assert(n1 >= 0);
    assert(!vox_redis_parser_is_complete(parser));
    
    ssize_t n2 = vox_redis_parser_execute(parser, input2, strlen(input2));
    assert(n2 > 0);
    assert(vox_redis_parser_is_complete(parser));
    
    vox_redis_parser_destroy(parser);
    vox_mpool_destroy(mpool);
    
    printf("PASSED\n");
}

/* ===== 响应管理测试 ===== */

static void test_response_copy() {
    printf("Testing response copy... ");
    
    vox_mpool_t* mpool = vox_mpool_create();
    assert(mpool != NULL);
    
    /* 创建源响应 */
    vox_redis_response_t src;
    memset(&src, 0, sizeof(src));
    src.type = VOX_REDIS_RESPONSE_BULK_STRING;
    src.u.bulk_string.data = "test data";
    src.u.bulk_string.len = 9;
    src.u.bulk_string.is_null = false;
    
    /* 复制响应 */
    vox_redis_response_t dst;
    int ret = vox_redis_response_copy(mpool, &src, &dst);
    assert(ret == 0);
    assert(dst.type == VOX_REDIS_RESPONSE_BULK_STRING);
    assert(dst.u.bulk_string.len == 9);
    assert(strcmp(dst.u.bulk_string.data, "test data") == 0);
    assert(dst.u.bulk_string.data != src.u.bulk_string.data);  /* 不同的指针 */
    
    /* 释放 */
    vox_redis_response_free(mpool, &dst);
    vox_mpool_destroy(mpool);
    
    printf("PASSED\n");
}

static void test_response_copy_array() {
    printf("Testing response copy - Array... ");
    
    vox_mpool_t* mpool = vox_mpool_create();
    assert(mpool != NULL);
    
    /* 创建数组响应 */
    vox_redis_response_t src;
    memset(&src, 0, sizeof(src));
    src.type = VOX_REDIS_RESPONSE_ARRAY;
    src.u.array.count = 2;
    src.u.array.elements = (vox_redis_response_t*)vox_mpool_alloc(
        mpool, sizeof(vox_redis_response_t) * 2);
    
    src.u.array.elements[0].type = VOX_REDIS_RESPONSE_INTEGER;
    src.u.array.elements[0].u.integer = 123;
    
    src.u.array.elements[1].type = VOX_REDIS_RESPONSE_SIMPLE_STRING;
    char* str = (char*)vox_mpool_alloc(mpool, 6);
    strcpy(str, "hello");
    src.u.array.elements[1].u.simple_string.data = str;
    src.u.array.elements[1].u.simple_string.len = 5;
    
    /* 复制 */
    vox_redis_response_t dst;
    int ret = vox_redis_response_copy(mpool, &src, &dst);
    assert(ret == 0);
    assert(dst.type == VOX_REDIS_RESPONSE_ARRAY);
    assert(dst.u.array.count == 2);
    assert(dst.u.array.elements[0].u.integer == 123);
    assert(strcmp(dst.u.array.elements[1].u.simple_string.data, "hello") == 0);
    
    /* 释放 */
    vox_redis_response_free(mpool, &dst);
    vox_redis_response_free(mpool, &src);
    vox_mpool_destroy(mpool);
    
    printf("PASSED\n");
}

/* ===== 命令构建测试 ===== */

static void test_commandv() {
    printf("Testing commandv API... ");
    
    vox_loop_t* loop = vox_loop_create();
    assert(loop != NULL);
    
    vox_redis_client_t* client = vox_redis_client_create(loop);
    assert(client != NULL);
    
    /* 测试 commandv（不连接，只测试构建） */
    const char* args[] = {"GET", "mykey"};
    int ret = vox_redis_client_commandv(client, NULL, NULL, NULL, 2, args);
    /* 预期失败（未连接） */
    assert(ret == -1);
    
    vox_redis_client_destroy(client);
    vox_loop_destroy(loop);
    
    printf("PASSED\n");
}

static void test_command_raw_not_connected() {
    printf("Testing command_raw (not connected)... ");
    
    vox_loop_t* loop = vox_loop_create();
    assert(loop != NULL);
    
    vox_redis_client_t* client = vox_redis_client_create(loop);
    assert(client != NULL);
    
    int error_called = 0;
    
    const char* cmd = "*1\r\n$4\r\nPING\r\n";
    int ret = vox_redis_client_command_raw(client, cmd, strlen(cmd), test_resp_unexpected, test_err_cb, &error_called);
    assert(ret == -1);
    assert(error_called == 1);
    
    vox_redis_client_destroy(client);
    vox_loop_destroy(loop);
    
    printf("PASSED\n");
}

static void test_parser_invalid_input() {
    printf("Testing RESP parser - Invalid Input... ");
    
    vox_mpool_t* mpool = vox_mpool_create();
    assert(mpool != NULL);
    
    vox_redis_parser_callbacks_t callbacks = {0};
    vox_redis_parser_t* parser = vox_redis_parser_create(mpool, NULL, &callbacks);
    assert(parser != NULL);
    
    const char* input = "$x\r\n"; /* 非法长度 */
    ssize_t n = vox_redis_parser_execute(parser, input, strlen(input));
    assert(n == -1);
    assert(vox_redis_parser_has_error(parser));
    
    vox_redis_parser_destroy(parser);
    vox_mpool_destroy(mpool);
    
    printf("PASSED\n");
}

/* ===== 主测试入口 ===== */

int main(void) {
    printf("=== Redis Module Tests ===\n\n");
    
    /* RESP 解析器测试 */
    printf("--- RESP Parser Tests ---\n");
    test_parser_simple_string();
    test_parser_bulk_string();
    test_parser_null_bulk_string();
    test_parser_integer();
    test_parser_array();
    test_parser_error();
    test_parser_nested_array();
    test_parser_incremental();
    test_parser_invalid_input();
    
    /* 响应管理测试 */
    printf("\n--- Response Management Tests ---\n");
    test_response_copy();
    test_response_copy_array();
    
    /* 命令 API 测试 */
    printf("\n--- Command API Tests ---\n");
    test_commandv();
    test_command_raw_not_connected();
    
    printf("\n=== All Tests PASSED ===\n");
    return 0;
}
