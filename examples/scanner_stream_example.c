/*
 * scanner_stream_example.c - 流式扫描器示例程序
 * 演示 vox_scanner_stream 的零拷贝流式解析用法
 */

#include "../vox_scanner.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <string.h>

/* 打印字符串视图 */
static void print_strview(const char* label, const vox_strview_t* sv) {
    printf("%s: ", label);
    if (sv && sv->ptr && sv->len > 0) {
        printf("\"%.*s\" (长度: %zu)\n", (int)sv->len, sv->ptr, sv->len);
    } else {
        printf("(空)\n");
    }
}

/* 示例1: 基本流式解析 */
static void example_basic_streaming(void) {
    printf("=== 示例1: 基本流式解析 ===\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 初始化流式扫描器 */
    vox_scanner_stream_t stream;
    if (vox_scanner_stream_init(&stream, mpool, VOX_SCANNER_NONE) != 0) {
        fprintf(stderr, "初始化流式扫描器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 模拟分块接收数据（零拷贝，不复制数据） */
    const char* chunk1 = "Hello, ";
    const char* chunk2 = "World! ";
    const char* chunk3 = "This is a ";
    const char* chunk4 = "streaming test.";
    
    printf("分块feed数据（零拷贝）:\n");
    printf("  Chunk 1: \"%s\"\n", chunk1);
    vox_scanner_stream_feed(&stream, chunk1, strlen(chunk1));
    
    printf("  Chunk 2: \"%s\"\n", chunk2);
    vox_scanner_stream_feed(&stream, chunk2, strlen(chunk2));
    
    printf("  Chunk 3: \"%s\"\n", chunk3);
    vox_scanner_stream_feed(&stream, chunk3, strlen(chunk3));
    
    printf("  Chunk 4: \"%s\"\n", chunk4);
    vox_scanner_stream_feed(&stream, chunk4, strlen(chunk4));
    
    printf("\n当前数据大小: %zu 字节\n", vox_scanner_stream_get_size(&stream));
    
    /* 使用标准扫描器API进行解析 */
    vox_scanner_t* scanner = vox_scanner_stream_get_scanner(&stream);
    printf("\n解析结果:\n");
    
    vox_strview_t sv;
    
    /* 获取第一个单词 */
    if (vox_scanner_get_until_char(scanner, ',', false, &sv) == 0) {
        print_strview("  第一个单词", &sv);
        vox_scanner_get_char(scanner);  /* 跳过逗号 */
    }
    
    /* 跳过空格 */
    vox_scanner_skip_ws(scanner);
    
    /* 获取第二个单词 */
    if (vox_scanner_get_until_char(scanner, '!', true, &sv) == 0) {
        print_strview("  第二个单词（包含!）", &sv);
    }
    
    /* 消费已处理的数据 */
    size_t consumed = vox_scanner_offset(scanner);
    printf("\n消费 %zu 字节已处理的数据\n", consumed);
    vox_scanner_stream_consume(&stream, consumed);
    printf("消费后数据大小: %zu 字节\n", vox_scanner_stream_get_size(&stream));
    
    /* 继续解析剩余数据 */
    vox_scanner_skip_ws(scanner);
    if (vox_scanner_get_until_char(scanner, '.', true, &sv) == 0) {
        print_strview("  剩余文本", &sv);
    }
    
    vox_scanner_stream_destroy(&stream);
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例2: 解析HTTP风格的请求行（跨片段匹配） */
static void example_http_request_line(void) {
    printf("=== 示例2: 解析HTTP请求行（跨片段） ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_scanner_stream_t stream;
    if (vox_scanner_stream_init(&stream, mpool, VOX_SCANNER_NONE) != 0) {
        fprintf(stderr, "初始化流式扫描器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 模拟HTTP请求行被分成多个片段 */
    const char* part1 = "GET /api/";
    const char* part2 = "users?page=1 HTTP/1.1\r\n";
    
    printf("Feed数据片段:\n");
    printf("  Part 1: \"%s\"\n", part1);
    vox_scanner_stream_feed(&stream, part1, strlen(part1));
    
    printf("  Part 2: \"%s\"\n", part2);
    vox_scanner_stream_feed(&stream, part2, strlen(part2));
    
    /* 解析HTTP请求行 */
    vox_scanner_t* scanner = vox_scanner_stream_get_scanner(&stream);
    printf("\n解析HTTP请求行:\n");
    
    vox_strview_t method, path, version;
    
    /* 解析方法 */
    if (vox_scanner_get_until_char(scanner, ' ', false, &method) == 0) {
        print_strview("  方法", &method);
        vox_scanner_get_char(scanner);  /* 跳过空格 */
    }
    
    /* 解析路径 */
    if (vox_scanner_get_until_char(scanner, ' ', false, &path) == 0) {
        print_strview("  路径", &path);
        vox_scanner_get_char(scanner);  /* 跳过空格 */
    }
    
    /* 解析版本（直到CRLF） */
    if (vox_scanner_get_until_str(scanner, "\r\n", false, &version) == 0) {
        print_strview("  版本", &version);
    }
    
    /* 跳过CRLF */
    vox_scanner_skip(scanner, 2);
    
    printf("\n解析完成，请求行格式正确\n");
    
    vox_scanner_stream_destroy(&stream);
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例3: 解析多行配置（逐行处理） */
static void example_multiline_config(void) {
    printf("=== 示例3: 解析多行配置（逐行处理） ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_scanner_stream_t stream;
    if (vox_scanner_stream_init(&stream, mpool, VOX_SCANNER_NONE) != 0) {
        fprintf(stderr, "初始化流式扫描器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 模拟逐行接收配置数据 */
    const char* lines[] = {
        "host=localhost\n",
        "port=8080\n",
        "timeout=30\n",
        "debug=true\n"
    };
    
    printf("逐行feed配置数据:\n");
    int line_count = 0;
    
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        printf("  Line %zu: %s", i + 1, lines[i]);
        vox_scanner_stream_feed(&stream, lines[i], strlen(lines[i]));
        
        /* 尝试解析完整的行 */
        vox_scanner_t* scanner = vox_scanner_stream_get_scanner(&stream);
        
        /* 检查是否有完整的行（以换行符结尾） */
        if (!vox_scanner_eof(scanner)) {
            vox_scanner_state_t state;
            vox_scanner_save_state(scanner, &state);
            
            vox_strview_t line;
            if (vox_scanner_get_until_char(scanner, '\n', false, &line) == 0) {
                /* 找到完整的行，解析键值对 */
                vox_scanner_t line_scanner;
                char line_buf[256];
                memcpy(line_buf, line.ptr, line.len);
                line_buf[line.len] = '\0';
                
                if (vox_scanner_init(&line_scanner, line_buf, line.len, VOX_SCANNER_NONE) == 0) {
                    vox_strview_t key, value;
                    
                    if (vox_scanner_get_until_char(&line_scanner, '=', false, &key) == 0) {
                        vox_scanner_get_char(&line_scanner);  /* 跳过= */
                        if (vox_scanner_get_until_char(&line_scanner, '\n', false, &value) == 0) {
                            line_count++;
                            printf("    [%d] ", line_count);
                            print_strview("键", &key);
                            printf("        ");
                            print_strview("值", &value);
                        }
                    }
                    
                    vox_scanner_destroy(&line_scanner);
                }
                
                /* 跳过换行符 */
                vox_scanner_get_char(scanner);
                
                /* 消费已处理的行 */
                size_t consumed = vox_scanner_offset(scanner);
                vox_scanner_stream_consume(&stream, consumed);
            } else {
                /* 没有完整的行，恢复状态 */
                vox_scanner_restore_state(scanner, &state);
            }
        }
    }
    
    printf("\n共解析 %d 行配置\n", line_count);
    
    vox_scanner_stream_destroy(&stream);
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例4: 处理跨片段字符串匹配 */
static void example_cross_chunk_matching(void) {
    printf("=== 示例4: 处理跨片段字符串匹配 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_scanner_stream_t stream;
    if (vox_scanner_stream_init(&stream, mpool, VOX_SCANNER_NONE) != 0) {
        fprintf(stderr, "初始化流式扫描器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 模拟目标字符串 "END" 被分成两个片段 */
    const char* chunk1 = "Hello, World! EN";
    const char* chunk2 = "D\nMore data here.";
    
    printf("Feed数据（目标字符串\"END\"跨片段）:\n");
    printf("  Chunk 1: \"%s\"\n", chunk1);
    vox_scanner_stream_feed(&stream, chunk1, strlen(chunk1));
    
    printf("  Chunk 2: \"%s\"\n", chunk2);
    vox_scanner_stream_feed(&stream, chunk2, strlen(chunk2));
    
    /* 检查部分匹配 */
    size_t partial_len = 0;
    bool has_partial = vox_scanner_stream_check_partial_match(&stream, "END", &partial_len);
    
    printf("\n检查部分匹配:\n");
    if (has_partial) {
        if (partial_len > 0) {
            printf("  发现部分匹配，长度: %zu\n", partial_len);
            printf("  说明: 需要继续feed数据才能完成匹配\n");
        } else {
            printf("  完全匹配（数据长度足够）\n");
        }
    } else {
        printf("  无匹配\n");
    }
    
    /* 尝试匹配字符串 */
    vox_scanner_t* scanner = vox_scanner_stream_get_scanner(&stream);
    vox_strview_t sv;
    
    if (vox_scanner_get_until_str(scanner, "END", false, &sv) == 0) {
        printf("\n匹配结果:\n");
        print_strview("  匹配前的文本", &sv);
        
        /* 跳过匹配的字符串 */
        vox_scanner_skip(scanner, 3);
        
        /* 获取剩余数据 */
        if (vox_scanner_get_until_char(scanner, '.', true, &sv) == 0) {
            print_strview("  匹配后的文本", &sv);
        }
    }
    
    vox_scanner_stream_destroy(&stream);
    vox_mpool_destroy(mpool);
    printf("\n");
}

/* 示例5: 重置和重用流式扫描器 */
static void example_reset_and_reuse(void) {
    printf("=== 示例5: 重置和重用流式扫描器 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_scanner_stream_t stream;
    if (vox_scanner_stream_init(&stream, mpool, VOX_SCANNER_NONE) != 0) {
        fprintf(stderr, "初始化流式扫描器失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 第一次使用 */
    printf("第一次使用:\n");
    const char* data1 = "First batch of data\n";
    vox_scanner_stream_feed(&stream, data1, strlen(data1));
    
    vox_scanner_t* scanner = vox_scanner_stream_get_scanner(&stream);
    vox_strview_t sv;
    if (vox_scanner_get_until_char(scanner, '\n', false, &sv) == 0) {
        print_strview("  解析结果", &sv);
    }
    
    printf("数据大小: %zu 字节\n", vox_scanner_stream_get_size(&stream));
    
    /* 重置扫描器 */
    printf("\n重置扫描器:\n");
    vox_scanner_stream_reset(&stream);
    printf("重置后数据大小: %zu 字节\n", vox_scanner_stream_get_size(&stream));
    
    /* 第二次使用 */
    printf("\n第二次使用:\n");
    const char* data2 = "Second batch of data\n";
    vox_scanner_stream_feed(&stream, data2, strlen(data2));
    
    scanner = vox_scanner_stream_get_scanner(&stream);
    if (vox_scanner_get_until_char(scanner, '\n', false, &sv) == 0) {
        print_strview("  解析结果", &sv);
    }
    
    printf("数据大小: %zu 字节\n", vox_scanner_stream_get_size(&stream));
    
    vox_scanner_stream_destroy(&stream);
    vox_mpool_destroy(mpool);
    printf("\n");
}

int main(void) {
    printf("========================================\n");
    printf("Vox Scanner Stream 流式解析示例\n");
    printf("========================================\n\n");
    
    example_basic_streaming();
    example_http_request_line();
    example_multiline_config();
    example_cross_chunk_matching();
    example_reset_and_reuse();
    
    printf("========================================\n");
    printf("所有示例执行完成\n");
    printf("========================================\n");
    
    return 0;
}
