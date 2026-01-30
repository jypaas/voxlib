/*
 * multipart_parser_example.c - vox_multipart_parser 使用示例
 * 演示 multipart/form-data 解析，覆盖各种正常和异常场景
 */

#include "../http/vox_http_multipart_parser.h"
#include "../vox_mpool.h"
#include "../vox_string.h"
#include <stdio.h>
#include <string.h>

/* ===== 测试上下文 ===== */

typedef struct {
    int part_count;
    int field_count;
    int data_chunk_count;
    int error_count;
    vox_string_t* current_header_name;
    vox_string_t* current_header_value;
    vox_string_t* current_field_name;
    vox_string_t* current_filename;
    vox_string_t* current_data;
} test_context_t;

/* ===== 回调函数 ===== */

static int on_part_begin(void* parser) {
    void* user_data = vox_multipart_parser_get_user_data((vox_multipart_parser_t*)parser);
    test_context_t* ctx = (test_context_t*)user_data;
    
    if (ctx) {
        ctx->part_count++;
        printf("\n[Part %d 开始]\n", ctx->part_count);
        
        if (ctx->current_header_name) vox_string_clear(ctx->current_header_name);
        if (ctx->current_header_value) vox_string_clear(ctx->current_header_value);
        if (ctx->current_field_name) vox_string_clear(ctx->current_field_name);
        if (ctx->current_filename) vox_string_clear(ctx->current_filename);
        if (ctx->current_data) {
            vox_string_clear(ctx->current_data);
        }
    }
    
    return 0;
}

static int flush_header_if_ready(test_context_t* ctx) {
    if (!ctx || !ctx->current_header_name || !ctx->current_header_value) return 0;
    if (vox_string_length(ctx->current_header_name) == 0) return 0;
    /* 允许 value 为空 */
    printf("  [头部] %s: %s\n",
           vox_string_cstr(ctx->current_header_name),
           vox_string_cstr(ctx->current_header_value));
    vox_string_clear(ctx->current_header_name);
    vox_string_clear(ctx->current_header_value);
    return 0;
}

static int on_header_field(void* parser, const char* data, size_t len) {
    void* user_data = vox_multipart_parser_get_user_data((vox_multipart_parser_t*)parser);
    test_context_t* ctx = (test_context_t*)user_data;

    if (!ctx || !data || len == 0) return 0;

    /* 进入新 header name 前，把上一条 header（name/value）输出并清空 */
    if (ctx->current_header_value && vox_string_length(ctx->current_header_value) > 0) {
        flush_header_if_ready(ctx);
    }

    if (ctx->current_header_name) {
        vox_string_append_data(ctx->current_header_name, data, len);
    }

    return 0;
}

static int on_header_value(void* parser, const char* data, size_t len) {
    void* user_data = vox_multipart_parser_get_user_data((vox_multipart_parser_t*)parser);
    test_context_t* ctx = (test_context_t*)user_data;

    if (!ctx || !data || len == 0) return 0;
    if (ctx->current_header_value) {
        vox_string_append_data(ctx->current_header_value, data, len);
    }
    return 0;
}

static int on_name(void* parser, const char* data, size_t len) {
    void* user_data = vox_multipart_parser_get_user_data((vox_multipart_parser_t*)parser);
    test_context_t* ctx = (test_context_t*)user_data;

    if (!ctx || !data || len == 0) return 0;
    if (ctx->current_field_name) {
        vox_string_append_data(ctx->current_field_name, data, len);
    }
    return 0;
}

static int on_filename(void* parser, const char* data, size_t len) {
    void* user_data = vox_multipart_parser_get_user_data((vox_multipart_parser_t*)parser);
    test_context_t* ctx = (test_context_t*)user_data;

    if (!ctx || !data || len == 0) return 0;
    if (ctx->current_filename) {
        vox_string_append_data(ctx->current_filename, data, len);
    }
    return 0;
}

static int on_headers_complete(void* parser) {
    void* user_data = vox_multipart_parser_get_user_data((vox_multipart_parser_t*)parser);
    test_context_t* ctx = (test_context_t*)user_data;

    /* 收尾输出最后一个 header */
    flush_header_if_ready(ctx);

    printf("  [字段信息]\n");
    if (ctx && ctx->current_field_name && vox_string_length(ctx->current_field_name) > 0) {
        printf("    名称: %s\n", vox_string_cstr(ctx->current_field_name));
    }
    if (ctx && ctx->current_filename && vox_string_length(ctx->current_filename) > 0) {
        printf("    文件名: %s\n", vox_string_cstr(ctx->current_filename));
    }

    if (ctx) {
        ctx->field_count++;
    }
    
    return 0;
}

static int on_part_data(void* parser, const char* data, size_t len) {
    void* user_data = vox_multipart_parser_get_user_data((vox_multipart_parser_t*)parser);
    test_context_t* ctx = (test_context_t*)user_data;
    
    if (ctx) {
        ctx->data_chunk_count++;
        if (ctx->current_data) {
            vox_string_append_data(ctx->current_data, data, len);
        }
        
        /* 只显示前 100 字节 */
        size_t display_len = len > 100 ? 100 : len;
        printf("  [数据块 %d] %zu 字节: %.*s%s\n", 
               ctx->data_chunk_count, len, 
               (int)display_len, data,
               len > 100 ? "..." : "");
    }
    
    return 0;
}

static int on_part_complete(void* parser) {
    void* user_data = vox_multipart_parser_get_user_data((vox_multipart_parser_t*)parser);
    test_context_t* ctx = (test_context_t*)user_data;
    
    printf("[Part 完成]\n");
    
    if (ctx && ctx->current_data && ctx->current_field_name) {
        const char* field_name = vox_string_cstr(ctx->current_field_name);
        const char* data = vox_string_cstr(ctx->current_data);
        size_t data_len = vox_string_length(ctx->current_data);
        
        if (field_name && data) {
            printf("  完整字段 '%s' 数据 (%zu 字节):\n", field_name, data_len);
            if (data_len <= 200) {
                printf("    %.*s\n", (int)data_len, data);
            } else {
                printf("    %.*s...\n", 200, data);
            }
        }
    }
    
    return 0;
}

static int on_complete(void* parser) {
    void* user_data = vox_multipart_parser_get_user_data((vox_multipart_parser_t*)parser);
    test_context_t* ctx = (test_context_t*)user_data;
    
    printf("\n[解析完成]\n");
    if (ctx) {
        printf("  总 Part 数: %d\n", ctx->part_count);
        printf("  总字段数: %d\n", ctx->field_count);
        printf("  数据块数: %d\n", ctx->data_chunk_count);
    }
    
    return 0;
}

static int on_error(void* parser, const char* message) {
    void* user_data = vox_multipart_parser_get_user_data((vox_multipart_parser_t*)parser);
    test_context_t* ctx = (test_context_t*)user_data;
    
    printf("\n[错误] %s\n", message);
    if (ctx) {
        ctx->error_count++;
    }
    
    return 0;
}

/* ===== 测试函数 ===== */

static int test_multipart(const char* test_name, 
                         const char* boundary,
                         const char* data, 
                         size_t data_len,
                         bool expect_success) {
    printf("\n");
    printf("========================================\n");
    printf("测试: %s\n", test_name);
    printf("========================================\n");
    printf("Boundary: %s\n", boundary);
    printf("数据长度: %zu 字节\n", data_len);
    printf("期望结果: %s\n", expect_success ? "成功" : "失败");
    printf("----------------------------------------\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        printf("错误: 无法创建内存池\n");
        return -1;
    }
    
    /* 创建测试上下文 */
    test_context_t ctx = {0};
    ctx.current_header_name = vox_string_create(mpool);
    ctx.current_header_value = vox_string_create(mpool);
    ctx.current_field_name = vox_string_create(mpool);
    ctx.current_filename = vox_string_create(mpool);
    ctx.current_data = vox_string_create(mpool);
    
    /* 创建回调 */
    vox_multipart_callbacks_t callbacks = {0};
    callbacks.on_part_begin = on_part_begin;
    callbacks.on_header_field = on_header_field;
    callbacks.on_header_value = on_header_value;
    callbacks.on_name = on_name;
    callbacks.on_filename = on_filename;
    callbacks.on_headers_complete = on_headers_complete;
    callbacks.on_part_data = on_part_data;
    callbacks.on_part_complete = on_part_complete;
    callbacks.on_complete = on_complete;
    callbacks.on_error = on_error;
    callbacks.user_data = &ctx;
    
    /* 创建解析器 */
    vox_multipart_parser_t* parser = vox_multipart_parser_create(
        mpool, boundary, strlen(boundary), NULL, &callbacks);
    
    if (!parser) {
        printf("错误: 无法创建解析器\n");
        vox_mpool_destroy(mpool);
        return -1;
    }
    
    /* 用户数据已经在 callbacks.user_data 中设置，不需要再次设置 */
    
    /* 执行解析 */
    ssize_t parsed = vox_multipart_parser_execute(parser, data, data_len);
    
    /* 检查结果 */
    bool has_error = vox_multipart_parser_has_error(parser);
    bool is_complete = vox_multipart_parser_is_complete(parser);
    bool success = (parsed >= 0) && is_complete && !has_error;
    
    if (success != expect_success) {
        printf("\n[测试失败] 期望 %s，但得到 %s\n", 
               expect_success ? "成功" : "失败",
               success ? "成功" : "失败");
        printf("解析字节数: %zd (总长度: %zu)\n", parsed, data_len);
        printf("解析完成: %s\n", is_complete ? "是" : "否");
        printf("有错误: %s\n", has_error ? "是" : "否");
        if (has_error) {
            const char* error_msg = vox_multipart_parser_get_error(parser);
            if (error_msg) {
                printf("错误消息: %s\n", error_msg);
            }
        }
        vox_multipart_parser_destroy(parser);
        vox_mpool_destroy(mpool);
        return -1;
    }
    
    if (!success && has_error) {
        const char* error_msg = vox_multipart_parser_get_error(parser);
        if (error_msg) {
            printf("错误消息: %s\n", error_msg);
        }
    }
    
    printf("\n[测试通过]\n");
    printf("解析字节数: %zd\n", parsed);
    
    /* 清理 */
    vox_multipart_parser_destroy(parser);
    vox_mpool_destroy(mpool);
    
    return 0;
}

/* ===== 测试用例 ===== */

int main(void) {
    printf("=== vox_multipart_parser 测试示例 ===\n");
    
    int failed = 0;
    
    /* ===== 正常情况测试 ===== */
    
    /* 测试 1: 简单的文本字段 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"username\"\r\n"
            "\r\n"
            "john_doe\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        if (test_multipart("简单文本字段", boundary, data, strlen(data), true) != 0) {
            failed++;
        }
    }
    
    /* 测试 2: 多个文本字段 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"username\"\r\n"
            "\r\n"
            "john_doe\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"email\"\r\n"
            "\r\n"
            "john@example.com\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        if (test_multipart("多个文本字段", boundary, data, strlen(data), true) != 0) {
            failed++;
        }
    }
    
    /* 测试 3: 文件上传 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "This is a test file content.\n"
            "It has multiple lines.\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        if (test_multipart("文件上传", boundary, data, strlen(data), true) != 0) {
            failed++;
        }
    }
    
    /* 测试 4: 混合文本和文件 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"title\"\r\n"
            "\r\n"
            "My Document\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"file\"; filename=\"document.pdf\"\r\n"
            "Content-Type: application/pdf\r\n"
            "\r\n"
            "PDF content here...\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        if (test_multipart("混合文本和文件", boundary, data, strlen(data), true) != 0) {
            failed++;
        }
    }
    
    /* 测试 5: 空字段值 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"empty_field\"\r\n"
            "\r\n"
            "\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        if (test_multipart("空字段值", boundary, data, strlen(data), true) != 0) {
            failed++;
        }
    }
    
    /* 测试 6: 大字段值 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        
        /* 使用内存池和字符串构建数据 */
        vox_mpool_t* temp_mpool = vox_mpool_create();
        if (temp_mpool) {
            vox_string_t* data_str = vox_string_create(temp_mpool);
            if (data_str) {
                vox_string_append(data_str, "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n");
                vox_string_append(data_str, "Content-Disposition: form-data; name=\"large_field\"\r\n");
                vox_string_append(data_str, "\r\n");
                /* 添加大量数据 */
                for (int i = 0; i < 1000; i++) {
                    vox_string_append(data_str, "This is a large field value. ");
                }
                vox_string_append(data_str, "\r\n");
                vox_string_append(data_str, "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n");
                
                const char* data = vox_string_cstr(data_str);
                size_t data_len = vox_string_length(data_str);
                
                if (test_multipart("大字段值", boundary, data, data_len, true) != 0) {
                    failed++;
                }
            }
            vox_mpool_destroy(temp_mpool);
        }
    }
    
    /* 测试 7: 特殊字符 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"special\"\r\n"
            "\r\n"
            "Value with special chars: !@#$%^&*()_+-=[]{}|;':\",./<>?\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        if (test_multipart("特殊字符", boundary, data, strlen(data), true) != 0) {
            failed++;
        }
    }
    
    /* 测试 8: Unicode 字符 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"unicode\"\r\n"
            "\r\n"
            "中文测试 \xE2\x98\xBA Unicode: \xF0\x9F\x98\x80\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        if (test_multipart("Unicode 字符", boundary, data, strlen(data), true) != 0) {
            failed++;
        }
    }
    
    /* 测试 9: 多个文件 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"file1\"; filename=\"file1.txt\"\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Content of file 1\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"file2\"; filename=\"file2.txt\"\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Content of file 2\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        if (test_multipart("多个文件", boundary, data, strlen(data), true) != 0) {
            failed++;
        }
    }
    
    /* 测试 10: 没有 Content-Disposition 的 Part */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Some content without Content-Disposition\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        if (test_multipart("无 Content-Disposition", boundary, data, strlen(data), true) != 0) {
            failed++;
        }
    }
    
    /* ===== 异常情况测试 ===== */
    
    /* 测试 11: 缺少 boundary */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "Content-Disposition: form-data; name=\"test\"\r\n"
            "\r\n"
            "value\r\n";
        
        if (test_multipart("缺少 boundary", boundary, data, strlen(data), false) != 0) {
            failed++;
        }
    }
    
    /* 测试 12: 错误的 boundary */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WrongBoundary\r\n"
            "Content-Disposition: form-data; name=\"test\"\r\n"
            "\r\n"
            "value\r\n"
            "------WrongBoundary--\r\n";
        
        if (test_multipart("错误的 boundary", boundary, data, strlen(data), false) != 0) {
            failed++;
        }
    }
    
    /* 测试 13: 不完整的 boundary */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"test\"\r\n"
            "\r\n"
            "value\r\n";
        /* 缺少结束 boundary */
        
        if (test_multipart("不完整的 boundary", boundary, data, strlen(data), false) != 0) {
            failed++;
        }
    }
    
    /* 测试 14: 无效的头部格式 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Invalid Header Format\r\n"
            "\r\n"
            "value\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        /* 这个可能仍然能解析，取决于解析器的容错性 */
        if (test_multipart("无效头部格式", boundary, data, strlen(data), true) != 0) {
            failed++;
        }
    }
    
    /* 测试 15: 空数据 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = "";
        
        if (test_multipart("空数据", boundary, data, strlen(data), false) != 0) {
            failed++;
        }
    }
    
    /* 测试 16: 只有 boundary，没有内容 */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        if (test_multipart("只有 boundary", boundary, data, strlen(data), true) != 0) {
            failed++;
        }
    }
    
    /* 测试 17: 分块数据（模拟流式解析） */
    {
        const char* boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
        const char* full_data = 
            "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
            "Content-Disposition: form-data; name=\"chunked\"\r\n"
            "\r\n"
            "This is chunked data\r\n"
            "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
        
        printf("\n");
        printf("========================================\n");
        printf("测试: 分块数据解析\n");
        printf("========================================\n");
        
        vox_mpool_t* mpool = vox_mpool_create();
        test_context_t ctx = {0};
        ctx.current_header_name = vox_string_create(mpool);
        ctx.current_header_value = vox_string_create(mpool);
        ctx.current_field_name = vox_string_create(mpool);
        ctx.current_filename = vox_string_create(mpool);
        ctx.current_data = vox_string_create(mpool);
        
        vox_multipart_callbacks_t callbacks = {0};
        callbacks.on_part_begin = on_part_begin;
        callbacks.on_header_field = on_header_field;
        callbacks.on_header_value = on_header_value;
        callbacks.on_name = on_name;
        callbacks.on_filename = on_filename;
        callbacks.on_headers_complete = on_headers_complete;
        callbacks.on_part_data = on_part_data;
        callbacks.on_part_complete = on_part_complete;
        callbacks.on_complete = on_complete;
        callbacks.on_error = on_error;
        callbacks.user_data = &ctx;
        
        vox_multipart_parser_t* parser = vox_multipart_parser_create(
            mpool, boundary, strlen(boundary), NULL, &callbacks);
        
        if (parser) {
            vox_multipart_parser_set_user_data(parser, &ctx);
            
            /* 分块解析 */
            size_t chunk_size = 10;
            size_t total_len = strlen(full_data);
            size_t offset = 0;
            
            printf("分块解析（每块 %zu 字节）:\n", chunk_size);
            
            while (offset < total_len) {
                size_t len = (offset + chunk_size < total_len) ? chunk_size : (total_len - offset);
                ssize_t parsed = vox_multipart_parser_execute(parser, full_data + offset, len);
                
                if (parsed < 0) {
                    printf("解析错误在偏移 %zu\n", offset);
                    break;
                }
                
                offset += len;
                printf("已解析到偏移 %zu/%zu\n", offset, total_len);
                
                if (vox_multipart_parser_is_complete(parser)) {
                    printf("解析完成\n");
                    break;
                }
            }
            
            if (vox_multipart_parser_is_complete(parser) && !vox_multipart_parser_has_error(parser)) {
                printf("\n[测试通过]\n");
            } else {
                printf("\n[测试失败]\n");
                if (vox_multipart_parser_has_error(parser)) {
                    printf("错误: %s\n", vox_multipart_parser_get_error(parser));
                }
                failed++;
            }
            
            vox_multipart_parser_destroy(parser);
        }
        
        vox_mpool_destroy(mpool);
    }
    
    /* ===== 总结 ===== */
    
    printf("\n");
    printf("========================================\n");
    printf("测试总结\n");
    printf("========================================\n");
    printf("失败测试数: %d\n", failed);
    
    if (failed == 0) {
        printf("\n所有测试通过！\n");
        return 0;
    } else {
        printf("\n有 %d 个测试失败\n", failed);
        return 1;
    }
}
