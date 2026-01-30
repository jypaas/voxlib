/*
 * scanner_example.c - 零拷贝字符串扫描器示例程序
 * 演示 vox_scanner 的基本用法
 */

#include "../vox_scanner.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 打印字符串视图 */
static void print_strview(const char* label, const vox_strview_t* sv) {
    printf("%s: ", label);
    if (sv && sv->ptr && sv->len > 0) {
        printf("\"%.*s\" (长度: %zu)\n", (int)sv->len, sv->ptr, sv->len);
    } else {
        printf("(空)\n");
    }
}

/* 示例1: 基本扫描操作 */
static void example_basic_scanning(void) {
    printf("=== 示例1: 基本扫描操作 ===\n");
    
    /* 准备缓冲区（注意：末尾必须有'\0'） */
    char buffer[256];
    const char* input = "Hello, World! This is a test.";
    size_t len = strlen(input);
    memcpy(buffer, input, len);
    buffer[len] = '\0';  /* 确保末尾有'\0' */
    
    /* 创建扫描器 */
    vox_scanner_t scanner;
    if (vox_scanner_init(&scanner, buffer, len, VOX_SCANNER_NONE) != 0) {
        fprintf(stderr, "初始化扫描器失败\n");
        return;
    }
    
    printf("输入字符串: \"%s\"\n", input);
    printf("当前位置: %zu\n", vox_scanner_offset(&scanner));
    printf("剩余长度: %zu\n", vox_scanner_remaining(&scanner));
    
    /* 查看当前字符 */
    int ch = vox_scanner_peek_char(&scanner);
    if (ch >= 0) {
        printf("当前字符: '%c' (0x%02x)\n", (char)ch, ch);
    }
    
    /* 获取前5个字符 */
    vox_strview_t sv;
    if (vox_scanner_get(&scanner, 5, &sv) == 0) {
        print_strview("获取5个字符", &sv);
    }
    
    /* 跳过逗号 */
    if (vox_scanner_peek_char(&scanner) == ',') {
        vox_scanner_get_char(&scanner);
    }
    
    /* 获取直到感叹号（跳过空格） */
    vox_scanner_skip_ws(&scanner);
    
    /* 获取直到感叹号 */
    if (vox_scanner_get_until_char(&scanner, '!', true, &sv) == 0) {
        print_strview("获取直到感叹号（包含）", &sv);
    }
    
    printf("\n");
    vox_scanner_destroy(&scanner);
}

/* 示例2: 使用字符集 */
static void example_charset(void) {
    printf("=== 示例2: 使用字符集 ===\n");
    
    char buffer[256];
    const char* input = "name=value&key=123&flag=true";
    size_t len = strlen(input);
    memcpy(buffer, input, len);
    buffer[len] = '\0';
    
    vox_scanner_t scanner;
    if (vox_scanner_init(&scanner, buffer, len, VOX_SCANNER_NONE) != 0) {
        fprintf(stderr, "初始化扫描器失败\n");
        return;
    }
    
    printf("输入字符串: \"%s\"\n", input);
    
    /* 初始化字符集 */
    //vox_charset_t alnum;
    //vox_charset_init(&alnum);
    //vox_charset_add_alnum(&alnum);
    
    vox_charset_t delimiter;
    vox_charset_init(&delimiter);
    vox_charset_add_char(&delimiter, '=');
    vox_charset_add_char(&delimiter, '&');
    
    /* 解析键值对 */
    printf("\n解析键值对:\n");
    vox_strview_t key, value;
    int pair_count = 0;
    
    while (!vox_scanner_eof(&scanner)) {
        /* 获取键（字母数字字符） */
        if (vox_scanner_get_until_charset(&scanner, &delimiter, false, &key) == 0) {
            print_strview("  键", &key);
            
            /* 跳过分隔符 */
            char sep = (char)vox_scanner_get_char(&scanner);
            if (sep == '=') {
                /* 获取值（直到&或末尾） */
                if (vox_scanner_get_until_char(&scanner, '&', false, &value) == 0) {
                    print_strview("  值", &value);
                    pair_count++;
                }
                
                /* 跳过&（如果存在） */
                if (!vox_scanner_eof(&scanner) && 
                    vox_scanner_peek_char(&scanner) == '&') {
                    vox_scanner_get_char(&scanner);
                }
            }
        } else {
            break;
        }
    }
    
    printf("共解析 %d 个键值对\n", pair_count);
    
    vox_scanner_destroy(&scanner);
    printf("\n");
}

/* 示例3: 自动跳过空白字符 */
static void example_autoskip_ws(void) {
    printf("=== 示例3: 自动跳过空白字符 ===\n");
    
    char buffer[256];
    const char* input = "  Hello   World  !  ";
    size_t len = strlen(input);
    memcpy(buffer, input, len);
    buffer[len] = '\0';
    
    vox_scanner_t scanner;
    /* 启用自动跳过空白字符 */
    if (vox_scanner_init(&scanner, buffer, len, VOX_SCANNER_AUTOSKIP_WS) != 0) {
        fprintf(stderr, "初始化扫描器失败\n");
        return;
    }
    
    printf("输入字符串: \"%s\"\n", input);
    printf("（已启用自动跳过空白字符）\n");
    
    vox_strview_t sv;
    int word_count = 0;
    
    /* 初始化字母字符集 */
    vox_charset_t alpha;
    vox_charset_init(&alpha);
    vox_charset_add_alpha(&alpha);
    
    /* 初始化空白字符集 */
    vox_charset_t ws;
    vox_charset_init(&ws);
    vox_charset_add_space(&ws);
    
    /* 获取单词 */
    while (!vox_scanner_eof(&scanner)) {
        if (vox_scanner_get_charset(&scanner, &alpha, &sv) == 0 && sv.len > 0) {
            word_count++;
            print_strview("单词", &sv);
        } else {
            /* 如果获取不到字母字符，跳过当前字符（可能是标点符号等） */
            if (!vox_scanner_eof(&scanner)) {
                vox_scanner_get_char(&scanner);
            }
        }
        /* 跳过空白字符（虽然已经自动跳过，但为了保险） */
        vox_scanner_skip_ws(&scanner);
    }
    
    printf("共找到 %d 个单词\n", word_count);
    
    vox_scanner_destroy(&scanner);
    printf("\n");
}

/* 示例4: 状态保存和恢复 */
static void example_save_restore_state(void) {
    printf("=== 示例4: 状态保存和恢复 ===\n");
    
    char buffer[256];
    const char* input = "123+456-789";
    size_t len = strlen(input);
    memcpy(buffer, input, len);
    buffer[len] = '\0';
    
    vox_scanner_t scanner;
    if (vox_scanner_init(&scanner, buffer, len, VOX_SCANNER_NONE) != 0) {
        fprintf(stderr, "初始化扫描器失败\n");
        return;
    }
    
    printf("输入字符串: \"%s\"\n", input);
    
    /* 初始化数字字符集 */
    vox_charset_t digit;
    vox_charset_init(&digit);
    vox_charset_add_digit(&digit);
    
    vox_strview_t sv;
    
    /* 保存初始状态 */
    vox_scanner_state_t state1;
    vox_scanner_save_state(&scanner, &state1);
    
    /* 获取第一个数字 */
    if (vox_scanner_get_charset(&scanner, &digit, &sv) == 0) {
        print_strview("第一个数字", &sv);
    }
    
    /* 保存当前位置 */
    vox_scanner_state_t state2;
    vox_scanner_save_state(&scanner, &state2);
    
    /* 跳过运算符 */
    char op1 = (char)vox_scanner_get_char(&scanner);
    printf("运算符1: '%c'\n", op1);
    
    /* 获取第二个数字 */
    if (vox_scanner_get_charset(&scanner, &digit, &sv) == 0) {
        print_strview("第二个数字", &sv);
    }
    
    /* 恢复到最后保存的状态（第二个数字之前） */
    printf("\n恢复到第二个数字之前的状态:\n");
    vox_scanner_restore_state(&scanner, &state2);
    printf("当前位置: %zu\n", vox_scanner_offset(&scanner));
    
    /* 跳过运算符（因为状态2保存时在运算符位置） */
    char op2 = (char)vox_scanner_get_char(&scanner);
    printf("运算符2: '%c'\n", op2);
    
    /* 重新获取第二个数字 */
    if (vox_scanner_get_charset(&scanner, &digit, &sv) == 0) {
        print_strview("重新获取第二个数字", &sv);
    }
    
    /* 恢复到初始状态 */
    printf("\n恢复到初始状态:\n");
    vox_scanner_restore_state(&scanner, &state1);
    printf("当前位置: %zu\n", vox_scanner_offset(&scanner));
    
    /* 重新开始解析 */
    if (vox_scanner_get_charset(&scanner, &digit, &sv) == 0) {
        print_strview("重新获取第一个数字", &sv);
    }
    
    vox_scanner_destroy(&scanner);
    printf("\n");
}

/* 示例5: 解析简单配置格式 */
static void example_parse_config(void) {
    printf("=== 示例5: 解析简单配置格式 ===\n");
    
    char buffer[512];
    const char* input = "host=localhost\nport=8080\ntimeout=30\n";
    size_t len = strlen(input);
    memcpy(buffer, input, len);
    buffer[len] = '\0';
    
    vox_scanner_t scanner;
    if (vox_scanner_init(&scanner, buffer, len, VOX_SCANNER_NONE) != 0) {
        fprintf(stderr, "初始化扫描器失败\n");
        return;
    }
    
    printf("输入配置:\n%s\n", input);
    
    /* 初始化字符集 */
    vox_charset_t alnum;
    vox_charset_init(&alnum);
    vox_charset_add_alnum(&alnum);
    
    vox_charset_t newline;
    vox_charset_init(&newline);
    vox_charset_add_char(&newline, '\n');
    vox_charset_add_char(&newline, '\r');
    
    printf("解析结果:\n");
    int line_count = 0;
    
    while (!vox_scanner_eof(&scanner)) {
        vox_strview_t key, value;
        
        /* 获取键 */
        if (vox_scanner_get_until_char(&scanner, '=', false, &key) == 0) {
            /* 跳过= */
            vox_scanner_get_char(&scanner);
            
            /* 获取值（直到换行符） */
            if (vox_scanner_get_until_charset(&scanner, &newline, false, &value) == 0) {
                line_count++;
                printf("  [%d] ", line_count);
                print_strview("键", &key);
                printf("      ");
                print_strview("值", &value);
                
                /* 跳过换行符 */
                vox_scanner_skip_newline(&scanner);
            }
        } else {
            break;
        }
    }
    
    printf("共解析 %d 行配置\n", line_count);
    
    vox_scanner_destroy(&scanner);
    printf("\n");
}

/* 示例6: 字符串视图操作 */
static void example_strview(void) {
    printf("=== 示例6: 字符串视图操作 ===\n");
    
    const char* str1 = "Hello";
    const char* str2 = "World";
    
    /* 创建字符串视图 */
    vox_strview_t sv1 = vox_strview_from_cstr(str1);
    vox_strview_t sv2 = vox_strview_from_cstr(str2);
    vox_strview_t sv3 = vox_strview_from_ptr(str1, 3);  /* "Hel" */
    
    print_strview("sv1 (Hello)", &sv1);
    print_strview("sv2 (World)", &sv2);
    print_strview("sv3 (Hel)", &sv3);
    
    /* 比较操作 */
    printf("\n比较操作:\n");
    int cmp = vox_strview_compare(&sv1, &sv2);
    printf("sv1 vs sv2: %d\n", cmp);
    
    cmp = vox_strview_compare(&sv1, &sv1);
    printf("sv1 vs sv1: %d\n", cmp);
    
    cmp = vox_strview_compare_cstr(&sv1, "Hello");
    printf("sv1 vs \"Hello\": %d\n", cmp);
    
    /* 检查是否为空 */
    vox_strview_t empty = VOX_STRVIEW_NULL;
    printf("\n空检查:\n");
    printf("sv1 是否为空: %s\n", vox_strview_empty(&sv1) ? "是" : "否");
    printf("empty 是否为空: %s\n", vox_strview_empty(&empty) ? "是" : "否");
    
    printf("\n");
}

int main(void) {
    printf("========================================\n");
    printf("Vox Scanner 零拷贝字符串扫描器示例\n");
    printf("========================================\n\n");
    
    example_basic_scanning();
    example_charset();
    example_autoskip_ws();
    example_save_restore_state();
    example_parse_config();
    example_strview();
    
    printf("========================================\n");
    printf("所有示例执行完成\n");
    printf("========================================\n");
    
    return 0;
}
