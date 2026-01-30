/*
 * regex_example.c - 正则表达式API使用示例
 */

#include "../vox_regex.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "Failed to create memory pool\n");
        return 1;
    }
    
    printf("=== 正则表达式示例 ===\n\n");
    
    /* 示例1: 基本匹配 */
    printf("示例1: 基本匹配\n");
    const char* pattern1 = "hello";
    const char* text1 = "hello world";
    
    vox_regex_t* regex1 = vox_regex_compile(mpool, pattern1, VOX_REGEX_NONE);
    if (regex1) {
        vox_regex_match_t match;
        bool matched = vox_regex_search(regex1, text1, strlen(text1), 0, &match);
        printf("模式: %s\n", pattern1);
        printf("文本: %s\n", text1);
        if (matched) {
            printf("匹配结果: 成功 (位置 %zu-%zu)\n\n", match.start, match.end);
        } else {
            printf("匹配结果: 失败\n\n");
        }
        vox_regex_destroy(regex1);
    }
    
    /* 示例2: 字符类匹配 */
    printf("示例2: 字符类匹配\n");
    const char* pattern2 = "[0-9]+";
    const char* text2 = "abc123def456";
    
    vox_regex_t* regex2 = vox_regex_compile(mpool, pattern2, VOX_REGEX_NONE);
    if (regex2) {
        vox_regex_match_t* matches = NULL;
        size_t match_count = 0;
        
        if (vox_regex_findall(regex2, text2, strlen(text2), &matches, &match_count) == 0) {
            printf("模式: %s\n", pattern2);
            printf("文本: %s\n", text2);
            printf("找到 %zu 个匹配:\n", match_count);
            for (size_t i = 0; i < match_count; i++) {
                printf("  匹配 %zu: ", i + 1);
                for (size_t j = matches[i].start; j < matches[i].end; j++) {
                    putchar(text2[j]);
                }
                printf(" (位置 %zu-%zu)\n", matches[i].start, matches[i].end);
            }
            printf("\n");
        }
        vox_regex_destroy(regex2);
    }
    
    /* 示例3: 查找所有匹配 */
    printf("示例3: 查找所有匹配\n");
    const char* pattern3 = "\\d+";
    const char* text3 = "abc123def456ghi789";
    
    vox_regex_t* regex3 = vox_regex_compile(mpool, pattern3, VOX_REGEX_NONE);
    if (regex3) {
        vox_regex_match_t* matches = NULL;
        size_t match_count = 0;
        
        if (vox_regex_findall(regex3, text3, strlen(text3), &matches, &match_count) == 0) {
            printf("模式: %s\n", pattern3);
            printf("文本: %s\n", text3);
            printf("找到 %zu 个匹配:\n", match_count);
            for (size_t i = 0; i < match_count; i++) {
                printf("  匹配 %zu: ", i + 1);
                for (size_t j = matches[i].start; j < matches[i].end; j++) {
                    putchar(text3[j]);
                }
                printf(" (位置 %zu-%zu)\n", matches[i].start, matches[i].end);
            }
            printf("\n");
        }
        vox_regex_destroy(regex3);
    }
    
    /* 示例4: 忽略大小写匹配 */
    printf("示例4: 忽略大小写匹配\n");
    const char* pattern4 = "hello";
    const char* text4 = "HELLO world";
    
    vox_regex_t* regex4 = vox_regex_compile(mpool, pattern4, VOX_REGEX_IGNORE_CASE);
    if (regex4) {
        vox_regex_match_t match;
        bool matched = vox_regex_search(regex4, text4, strlen(text4), 0, &match);
        printf("模式: %s (忽略大小写)\n", pattern4);
        printf("文本: %s\n", text4);
        if (matched) {
            printf("匹配结果: 成功 (位置 %zu-%zu)\n\n", match.start, match.end);
        } else {
            printf("匹配结果: 失败\n\n");
        }
        vox_regex_destroy(regex4);
    }
    
    /* 示例5: 量词匹配 */
    printf("示例5: 量词匹配\n");
    const char* pattern5 = "a+b*";
    const char* text5 = "aaabbb";
    
    vox_regex_t* regex5 = vox_regex_compile(mpool, pattern5, VOX_REGEX_NONE);
    if (regex5) {
        bool matched = vox_regex_match(regex5, text5, strlen(text5), NULL);
        printf("模式: %s\n", pattern5);
        printf("文本: %s\n", text5);
        printf("匹配结果: %s\n\n", matched ? "成功" : "失败");
        vox_regex_destroy(regex5);
    }
    
    /* 示例6: 任意字符匹配 */
    printf("示例6: 任意字符匹配\n");
    const char* pattern6 = "h.llo";
    const char* text6 = "hello";
    
    vox_regex_t* regex6 = vox_regex_compile(mpool, pattern6, VOX_REGEX_NONE);
    if (regex6) {
        bool matched = vox_regex_match(regex6, text6, strlen(text6), NULL);
        printf("模式: %s\n", pattern6);
        printf("文本: %s\n", text6);
        printf("匹配结果: %s\n\n", matched ? "成功" : "失败");
        vox_regex_destroy(regex6);
    }
    
    /* 示例7: 转义序列 */
    printf("示例7: 转义序列\n");
    const char* pattern7 = "\\w+";
    const char* text7 = "hello123";
    
    vox_regex_t* regex7 = vox_regex_compile(mpool, pattern7, VOX_REGEX_NONE);
    if (regex7) {
        bool matched = vox_regex_match(regex7, text7, strlen(text7), NULL);
        printf("模式: %s (单词字符)\n", pattern7);
        printf("文本: %s\n", text7);
        printf("匹配结果: %s\n\n", matched ? "成功" : "失败");
        vox_regex_destroy(regex7);
    }
    
    /* 示例8: 替换 */
    printf("示例8: 替换\n");
    const char* pattern8 = "\\d+";
    const char* text8 = "abc123def456";
    const char* replacement = "NUM";
    char output[256];
    size_t output_len;
    
    vox_regex_t* regex8 = vox_regex_compile(mpool, pattern8, VOX_REGEX_NONE);
    if (regex8) {
        if (vox_regex_replace(regex8, text8, strlen(text8), replacement, 
                              output, sizeof(output), &output_len) == 0) {
            printf("模式: %s\n", pattern8);
            printf("原始文本: %s\n", text8);
            printf("替换为: %s\n", replacement);
            printf("结果: %s\n\n", output);
        }
        vox_regex_destroy(regex8);
    }
    
    /* ===== 全面测试 ===== */
    printf("=== 全面功能测试 ===\n\n");
    
    /* 测试9: 字符类 - 单个字符 */
    printf("测试9: 字符类 [abc]\n");
    const char* pattern9 = "[abc]";
    const char* text9 = "xyzabc";
    vox_regex_t* regex9 = vox_regex_compile(mpool, pattern9, VOX_REGEX_NONE);
    if (regex9) {
        vox_regex_match_t match9;
        if (vox_regex_search(regex9, text9, strlen(text9), 0, &match9)) {
            printf("模式: %s\n", pattern9);
            printf("文本: %s\n", text9);
            printf("匹配: ");
            for (size_t i = match9.start; i < match9.end; i++) {
                putchar(text9[i]);
            }
            printf(" (位置 %zu-%zu)\n\n", match9.start, match9.end);
        }
        vox_regex_destroy(regex9);
    }
    
    /* 测试10: 否定字符类 */
    printf("测试10: 否定字符类 [^0-9]\n");
    const char* pattern10 = "[^0-9]+";
    const char* text10 = "abc123def";
    vox_regex_t* regex10 = vox_regex_compile(mpool, pattern10, VOX_REGEX_NONE);
    if (regex10) {
        vox_regex_match_t match10;
        if (vox_regex_search(regex10, text10, strlen(text10), 0, &match10)) {
            printf("模式: %s\n", pattern10);
            printf("文本: %s\n", text10);
            printf("匹配: ");
            for (size_t i = match10.start; i < match10.end; i++) {
                putchar(text10[i]);
            }
            printf(" (位置 %zu-%zu)\n\n", match10.start, match10.end);
        }
        vox_regex_destroy(regex10);
    }
    
    /* 测试11: 字符范围 */
    printf("测试11: 字符范围 [a-z]\n");
    const char* pattern11 = "[a-z]+";
    const char* text11 = "ABCdefGHI";
    vox_regex_t* regex11 = vox_regex_compile(mpool, pattern11, VOX_REGEX_NONE);
    if (regex11) {
        vox_regex_match_t match11;
        if (vox_regex_search(regex11, text11, strlen(text11), 0, &match11)) {
            printf("模式: %s\n", pattern11);
            printf("文本: %s\n", text11);
            printf("匹配: ");
            for (size_t i = match11.start; i < match11.end; i++) {
                putchar(text11[i]);
            }
            printf(" (位置 %zu-%zu)\n\n", match11.start, match11.end);
        }
        vox_regex_destroy(regex11);
    }
    
    /* 测试12: 转义序列 \D (非数字) */
    printf("测试12: 转义序列 \\D (非数字)\n");
    const char* pattern12 = "\\D+";
    const char* text12 = "123abc456";
    vox_regex_t* regex12 = vox_regex_compile(mpool, pattern12, VOX_REGEX_NONE);
    if (regex12) {
        vox_regex_match_t match12;
        if (vox_regex_search(regex12, text12, strlen(text12), 0, &match12)) {
            printf("模式: %s\n", pattern12);
            printf("文本: %s\n", text12);
            printf("匹配: ");
            for (size_t i = match12.start; i < match12.end; i++) {
                putchar(text12[i]);
            }
            printf(" (位置 %zu-%zu)\n\n", match12.start, match12.end);
        }
        vox_regex_destroy(regex12);
    }
    
    /* 测试13: 转义序列 \W (非单词字符) */
    printf("测试13: 转义序列 \\W (非单词字符)\n");
    const char* pattern13 = "\\W+";
    const char* text13 = "hello world!";
    vox_regex_t* regex13 = vox_regex_compile(mpool, pattern13, VOX_REGEX_NONE);
    if (regex13) {
        vox_regex_match_t match13;
        if (vox_regex_search(regex13, text13, strlen(text13), 0, &match13)) {
            printf("模式: %s\n", pattern13);
            printf("文本: %s\n", text13);
            printf("匹配: ");
            for (size_t i = match13.start; i < match13.end; i++) {
                putchar(text13[i]);
            }
            printf(" (位置 %zu-%zu)\n\n", match13.start, match13.end);
        }
        vox_regex_destroy(regex13);
    }
    
    /* 测试14: 转义序列 \s (空白字符) */
    printf("测试14: 转义序列 \\s (空白字符)\n");
    const char* pattern14 = "\\s+";
    const char* text14 = "hello world";
    vox_regex_t* regex14 = vox_regex_compile(mpool, pattern14, VOX_REGEX_NONE);
    if (regex14) {
        vox_regex_match_t match14;
        if (vox_regex_search(regex14, text14, strlen(text14), 0, &match14)) {
            printf("模式: %s\n", pattern14);
            printf("文本: %s\n", text14);
            printf("匹配: '");
            for (size_t i = match14.start; i < match14.end; i++) {
                if (text14[i] == ' ') {
                    printf(" ");
                } else {
                    printf("\\x%02x", (unsigned char)text14[i]);
                }
            }
            printf("' (位置 %zu-%zu)\n\n", match14.start, match14.end);
        }
        vox_regex_destroy(regex14);
    }
    
    /* 测试15: 转义序列 \S (非空白字符) */
    printf("测试15: 转义序列 \\S (非空白字符)\n");
    const char* pattern15 = "\\S+";
    const char* text15 = "hello world";
    vox_regex_t* regex15 = vox_regex_compile(mpool, pattern15, VOX_REGEX_NONE);
    if (regex15) {
        vox_regex_match_t match15;
        if (vox_regex_search(regex15, text15, strlen(text15), 0, &match15)) {
            printf("模式: %s\n", pattern15);
            printf("文本: %s\n", text15);
            printf("匹配: ");
            for (size_t i = match15.start; i < match15.end; i++) {
                putchar(text15[i]);
            }
            printf(" (位置 %zu-%zu)\n\n", match15.start, match15.end);
        }
        vox_regex_destroy(regex15);
    }
    
    /* 测试16: 量词 * (0次或多次) */
    printf("测试16: 量词 * (0次或多次)\n");
    const char* pattern16 = "ab*c";
    const char* text16_1 = "ac";
    const char* text16_2 = "abc";
    const char* text16_3 = "abbc";
    vox_regex_t* regex16 = vox_regex_compile(mpool, pattern16, VOX_REGEX_NONE);
    if (regex16) {
        printf("模式: %s\n", pattern16);
        bool m1 = vox_regex_match(regex16, text16_1, strlen(text16_1), NULL);
        bool m2 = vox_regex_match(regex16, text16_2, strlen(text16_2), NULL);
        bool m3 = vox_regex_match(regex16, text16_3, strlen(text16_3), NULL);
        printf("文本 '%s': %s\n", text16_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n", text16_2, m2 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text16_3, m3 ? "匹配" : "不匹配");
        vox_regex_destroy(regex16);
    }
    
    /* 测试17: 量词 ? (0次或1次) */
    printf("测试17: 量词 ? (0次或1次)\n");
    const char* pattern17 = "colou?r";
    const char* text17_1 = "color";
    const char* text17_2 = "colour";
    const char* text17_3 = "colouur";
    vox_regex_t* regex17 = vox_regex_compile(mpool, pattern17, VOX_REGEX_NONE);
    if (regex17) {
        printf("模式: %s\n", pattern17);
        bool m1 = vox_regex_match(regex17, text17_1, strlen(text17_1), NULL);
        bool m2 = vox_regex_match(regex17, text17_2, strlen(text17_2), NULL);
        bool m3 = vox_regex_match(regex17, text17_3, strlen(text17_3), NULL);
        printf("文本 '%s': %s\n", text17_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n", text17_2, m2 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text17_3, m3 ? "匹配" : "不匹配");
        vox_regex_destroy(regex17);
    }
    
    /* 测试18: 量词 {n} (恰好n次) */
    printf("测试18: 量词 {n} (恰好n次)\n");
    const char* pattern18 = "a{3}";
    const char* text18_1 = "aa";
    const char* text18_2 = "aaa";
    const char* text18_3 = "aaaa";
    vox_regex_t* regex18 = vox_regex_compile(mpool, pattern18, VOX_REGEX_NONE);
    if (regex18) {
        printf("模式: %s\n", pattern18);
        bool m1 = vox_regex_match(regex18, text18_1, strlen(text18_1), NULL);
        bool m2 = vox_regex_match(regex18, text18_2, strlen(text18_2), NULL);
        bool m3 = vox_regex_match(regex18, text18_3, strlen(text18_3), NULL);
        printf("文本 '%s': %s\n", text18_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n", text18_2, m2 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text18_3, m3 ? "匹配" : "不匹配");
        vox_regex_destroy(regex18);
    }
    
    /* 测试19: 量词 {n,} (至少n次) */
    printf("测试19: 量词 {n,} (至少n次)\n");
    const char* pattern19 = "a{2,}";
    const char* text19_1 = "a";
    const char* text19_2 = "aa";
    const char* text19_3 = "aaa";
    vox_regex_t* regex19 = vox_regex_compile(mpool, pattern19, VOX_REGEX_NONE);
    if (regex19) {
        printf("模式: %s\n", pattern19);
        bool m1 = vox_regex_match(regex19, text19_1, strlen(text19_1), NULL);
        bool m2 = vox_regex_match(regex19, text19_2, strlen(text19_2), NULL);
        bool m3 = vox_regex_match(regex19, text19_3, strlen(text19_3), NULL);
        printf("文本 '%s': %s\n", text19_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n", text19_2, m2 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text19_3, m3 ? "匹配" : "不匹配");
        vox_regex_destroy(regex19);
    }
    
    /* 测试20: 量词 {n,m} (n到m次) */
    printf("测试20: 量词 {n,m} (n到m次)\n");
    const char* pattern20 = "a{2,4}";
    const char* text20_1 = "a";
    const char* text20_2 = "aa";
    const char* text20_3 = "aaa";
    const char* text20_4 = "aaaa";
    const char* text20_5 = "aaaaa";
    vox_regex_t* regex20 = vox_regex_compile(mpool, pattern20, VOX_REGEX_NONE);
    if (regex20) {
        printf("模式: %s\n", pattern20);
        bool m1 = vox_regex_match(regex20, text20_1, strlen(text20_1), NULL);
        bool m2 = vox_regex_match(regex20, text20_2, strlen(text20_2), NULL);
        bool m3 = vox_regex_match(regex20, text20_3, strlen(text20_3), NULL);
        bool m4 = vox_regex_match(regex20, text20_4, strlen(text20_4), NULL);
        bool m5 = vox_regex_match(regex20, text20_5, strlen(text20_5), NULL);
        printf("文本 '%s': %s\n", text20_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n", text20_2, m2 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n", text20_3, m3 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n", text20_4, m4 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text20_5, m5 ? "匹配" : "不匹配");
        vox_regex_destroy(regex20);
    }
    
    /* 测试21: 选择 | */
    printf("测试21: 选择 |\n");
    const char* pattern21 = "cat|dog";
    const char* text21_1 = "cat";
    const char* text21_2 = "dog";
    const char* text21_3 = "bird";
    vox_regex_t* regex21 = vox_regex_compile(mpool, pattern21, VOX_REGEX_NONE);
    if (regex21) {
        printf("模式: %s\n", pattern21);
        bool m1 = vox_regex_match(regex21, text21_1, strlen(text21_1), NULL);
        bool m2 = vox_regex_match(regex21, text21_2, strlen(text21_2), NULL);
        bool m3 = vox_regex_match(regex21, text21_3, strlen(text21_3), NULL);
        printf("文本 '%s': %s\n", text21_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n", text21_2, m2 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text21_3, m3 ? "匹配" : "不匹配");
        vox_regex_destroy(regex21);
    }
    
    /* 测试22: 任意字符 . */
    printf("测试22: 任意字符 .\n");
    const char* pattern22 = "h.llo";
    const char* text22_1 = "hello";
    const char* text22_2 = "hallo";
    const char* text22_3 = "hxllo";
    vox_regex_t* regex22 = vox_regex_compile(mpool, pattern22, VOX_REGEX_NONE);
    if (regex22) {
        printf("模式: %s\n", pattern22);
        bool m1 = vox_regex_match(regex22, text22_1, strlen(text22_1), NULL);
        bool m2 = vox_regex_match(regex22, text22_2, strlen(text22_2), NULL);
        bool m3 = vox_regex_match(regex22, text22_3, strlen(text22_3), NULL);
        printf("文本 '%s': %s\n", text22_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n", text22_2, m2 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text22_3, m3 ? "匹配" : "不匹配");
        vox_regex_destroy(regex22);
    }
    
    /* 测试23: 组合测试 - 复杂模式 */
    printf("测试23: 组合测试 - 复杂模式\n");
    const char* pattern23 = "[a-z]+\\d{2,4}[A-Z]*";
    const char* text23_1 = "abc123XYZ";
    const char* text23_2 = "hello12";
    const char* text23_3 = "test1234ABC";
    vox_regex_t* regex23 = vox_regex_compile(mpool, pattern23, VOX_REGEX_NONE);
    if (regex23) {
        printf("模式: %s\n", pattern23);
        bool m1 = vox_regex_match(regex23, text23_1, strlen(text23_1), NULL);
        bool m2 = vox_regex_match(regex23, text23_2, strlen(text23_2), NULL);
        bool m3 = vox_regex_match(regex23, text23_3, strlen(text23_3), NULL);
        printf("文本 '%s': %s\n", text23_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n", text23_2, m2 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text23_3, m3 ? "匹配" : "不匹配");
        vox_regex_destroy(regex23);
    }
    
    /* 测试24: 忽略大小写 - 字符类 */
    printf("测试24: 忽略大小写 - 字符类\n");
    const char* pattern24 = "[a-z]+";
    const char* text24 = "HELLO";
    vox_regex_t* regex24 = vox_regex_compile(mpool, pattern24, VOX_REGEX_IGNORE_CASE);
    if (regex24) {
        vox_regex_match_t match24;
        if (vox_regex_search(regex24, text24, strlen(text24), 0, &match24)) {
            printf("模式: %s (忽略大小写)\n", pattern24);
            printf("文本: %s\n", text24);
            printf("匹配: ");
            for (size_t i = match24.start; i < match24.end; i++) {
                putchar(text24[i]);
            }
            printf(" (位置 %zu-%zu)\n\n", match24.start, match24.end);
        }
        vox_regex_destroy(regex24);
    }
    
    /* 测试25: 转义字符 */
    printf("测试25: 转义字符\n");
    const char* pattern25 = "\\.";
    const char* text25 = "hello.world";
    vox_regex_t* regex25 = vox_regex_compile(mpool, pattern25, VOX_REGEX_NONE);
    if (regex25) {
        vox_regex_match_t match25;
        if (vox_regex_search(regex25, text25, strlen(text25), 0, &match25)) {
            printf("模式: %s\n", pattern25);
            printf("文本: %s\n", text25);
            printf("匹配: ");
            for (size_t i = match25.start; i < match25.end; i++) {
                putchar(text25[i]);
            }
            printf(" (位置 %zu-%zu)\n\n", match25.start, match25.end);
        }
        vox_regex_destroy(regex25);
    }
    
    /* 测试26: 复杂查找所有匹配 */
    printf("测试26: 复杂查找所有匹配\n");
    const char* pattern26 = "[a-z]{2,}";
    const char* text26 = "abc def ghi jkl";
    vox_regex_t* regex26 = vox_regex_compile(mpool, pattern26, VOX_REGEX_NONE);
    if (regex26) {
        vox_regex_match_t* matches26 = NULL;
        size_t match_count26 = 0;
        if (vox_regex_findall(regex26, text26, strlen(text26), &matches26, &match_count26) == 0) {
            printf("模式: %s\n", pattern26);
            printf("文本: %s\n", text26);
            printf("找到 %zu 个匹配:\n", match_count26);
            for (size_t i = 0; i < match_count26; i++) {
                printf("  匹配 %zu: ", i + 1);
                for (size_t j = matches26[i].start; j < matches26[i].end; j++) {
                    putchar(text26[j]);
                }
                printf(" (位置 %zu-%zu)\n", matches26[i].start, matches26[i].end);
            }
            printf("\n");
        }
        vox_regex_destroy(regex26);
    }
    
    /* 测试27: 边界情况 - 空字符串 */
    printf("测试27: 边界情况 - 空字符串匹配\n");
    const char* pattern27 = "a*";
    const char* text27 = "";
    vox_regex_t* regex27 = vox_regex_compile(mpool, pattern27, VOX_REGEX_NONE);
    if (regex27) {
        bool matched = vox_regex_match(regex27, text27, 0, NULL);
        printf("模式: %s\n", pattern27);
        printf("文本: (空字符串)\n");
        printf("匹配结果: %s\n\n", matched ? "成功" : "失败");
        vox_regex_destroy(regex27);
    }
    
    /* 测试28: 边界情况 - 不匹配 */
    printf("测试28: 边界情况 - 不匹配\n");
    const char* pattern28 = "xyz";
    const char* text28 = "abc";
    vox_regex_t* regex28 = vox_regex_compile(mpool, pattern28, VOX_REGEX_NONE);
    if (regex28) {
        bool matched = vox_regex_search(regex28, text28, strlen(text28), 0, NULL);
        printf("模式: %s\n", pattern28);
        printf("文本: %s\n", text28);
        printf("匹配结果: %s\n\n", matched ? "成功" : "失败");
        vox_regex_destroy(regex28);
    }
    
    /* 测试29: 行首锚点 ^ */
    printf("测试29: 行首锚点 ^\n");
    const char* pattern29 = "^hello";
    const char* text29_1 = "hello world";
    const char* text29_2 = "say hello";
    vox_regex_t* regex29 = vox_regex_compile(mpool, pattern29, VOX_REGEX_NONE);
    if (regex29) {
        vox_regex_match_t match29_1, match29_2;
        bool m1 = vox_regex_search(regex29, text29_1, strlen(text29_1), 0, &match29_1);
        bool m2 = vox_regex_search(regex29, text29_2, strlen(text29_2), 0, &match29_2);
        printf("模式: %s\n", pattern29);
        printf("文本 '%s': %s\n", text29_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text29_2, m2 ? "匹配" : "不匹配");
        vox_regex_destroy(regex29);
    }
    
    /* 测试30: 行尾锚点 $ */
    printf("测试30: 行尾锚点 $\n");
    const char* pattern30 = "world$";
    const char* text30_1 = "hello world";
    const char* text30_2 = "world peace";
    vox_regex_t* regex30 = vox_regex_compile(mpool, pattern30, VOX_REGEX_NONE);
    if (regex30) {
        vox_regex_match_t match30_1, match30_2;
        bool m1 = vox_regex_search(regex30, text30_1, strlen(text30_1), 0, &match30_1);
        bool m2 = vox_regex_search(regex30, text30_2, strlen(text30_2), 0, &match30_2);
        printf("模式: %s\n", pattern30);
        printf("文本 '%s': %s\n", text30_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text30_2, m2 ? "匹配" : "不匹配");
        vox_regex_destroy(regex30);
    }
    
    /* 测试31: 组合使用 ^ 和 $ */
    printf("测试31: 组合使用 ^ 和 $\n");
    const char* pattern31 = "^hello$";
    const char* text31_1 = "hello";
    const char* text31_2 = "hello world";
    const char* text31_3 = "say hello";
    vox_regex_t* regex31 = vox_regex_compile(mpool, pattern31, VOX_REGEX_NONE);
    if (regex31) {
        bool m1 = vox_regex_match(regex31, text31_1, strlen(text31_1), NULL);
        bool m2 = vox_regex_match(regex31, text31_2, strlen(text31_2), NULL);
        bool m3 = vox_regex_match(regex31, text31_3, strlen(text31_3), NULL);
        printf("模式: %s\n", pattern31);
        printf("文本 '%s': %s\n", text31_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n", text31_2, m2 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text31_3, m3 ? "匹配" : "不匹配");
        vox_regex_destroy(regex31);
    }
    
    /* 测试32: MULTILINE 模式 - ^ 匹配行首 */
    printf("测试32: MULTILINE 模式 - ^ 匹配行首\n");
    const char* pattern32 = "^hello";
    const char* text32 = "world\nhello\nworld";
    vox_regex_t* regex32 = vox_regex_compile(mpool, pattern32, VOX_REGEX_MULTILINE);
    if (regex32) {
        vox_regex_match_t* matches32 = NULL;
        size_t match_count32 = 0;
        if (vox_regex_findall(regex32, text32, strlen(text32), &matches32, &match_count32) == 0) {
            printf("模式: %s (MULTILINE)\n", pattern32);
            printf("文本: %s\n", text32);
            printf("找到 %zu 个匹配:\n", match_count32);
            for (size_t i = 0; i < match_count32; i++) {
                printf("  匹配 %zu: ", i + 1);
                for (size_t j = matches32[i].start; j < matches32[i].end; j++) {
                    putchar(text32[j]);
                }
                printf(" (位置 %zu-%zu)\n", matches32[i].start, matches32[i].end);
            }
            printf("\n");
        }
        vox_regex_destroy(regex32);
    }
    
    /* 测试33: MULTILINE 模式 - $ 匹配行尾 */
    printf("测试33: MULTILINE 模式 - $ 匹配行尾\n");
    const char* pattern33 = "world$";
    const char* text33 = "hello\nworld\nhello";
    vox_regex_t* regex33 = vox_regex_compile(mpool, pattern33, VOX_REGEX_MULTILINE);
    if (regex33) {
        vox_regex_match_t* matches33 = NULL;
        size_t match_count33 = 0;
        if (vox_regex_findall(regex33, text33, strlen(text33), &matches33, &match_count33) == 0) {
            printf("模式: %s (MULTILINE)\n", pattern33);
            printf("文本: %s\n", text33);
            printf("找到 %zu 个匹配:\n", match_count33);
            for (size_t i = 0; i < match_count33; i++) {
                printf("  匹配 %zu: ", i + 1);
                for (size_t j = matches33[i].start; j < matches33[i].end; j++) {
                    putchar(text33[j]);
                }
                printf(" (位置 %zu-%zu)\n", matches33[i].start, matches33[i].end);
            }
            printf("\n");
        }
        vox_regex_destroy(regex33);
    }
    
    /* 测试34: DOTALL 模式 - . 匹配换行符 */
    printf("测试34: DOTALL 模式 - . 匹配换行符\n");
    const char* pattern34 = "a.b";
    const char* text34_1 = "a\nb";
    const char* text34_2 = "axb";
    vox_regex_t* regex34 = vox_regex_compile(mpool, pattern34, VOX_REGEX_DOTALL);
    if (regex34) {
        bool m1 = vox_regex_match(regex34, text34_1, strlen(text34_1), NULL);
        bool m2 = vox_regex_match(regex34, text34_2, strlen(text34_2), NULL);
        printf("模式: %s (DOTALL)\n", pattern34);
        printf("文本 '%s': %s\n", text34_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text34_2, m2 ? "匹配" : "不匹配");
        vox_regex_destroy(regex34);
    }
    
    /* 测试35: 默认模式 - . 不匹配换行符 */
    printf("测试35: 默认模式 - . 不匹配换行符\n");
    const char* pattern35 = "a.b";
    const char* text35_1 = "a\nb";
    const char* text35_2 = "axb";
    vox_regex_t* regex35 = vox_regex_compile(mpool, pattern35, VOX_REGEX_NONE);
    if (regex35) {
        bool m1 = vox_regex_match(regex35, text35_1, strlen(text35_1), NULL);
        bool m2 = vox_regex_match(regex35, text35_2, strlen(text35_2), NULL);
        printf("模式: %s (默认)\n", pattern35);
        printf("文本 '%s': %s\n", text35_1, m1 ? "匹配" : "不匹配");
        printf("文本 '%s': %s\n\n", text35_2, m2 ? "匹配" : "不匹配");
        vox_regex_destroy(regex35);
    }
    
    /* 测试36: 字符类转义序列 [\s\S] */
    printf("测试36: 字符类转义序列 [\\s\\S]\n");
    const char* pattern36 = "a[\\s\\S]b";
    const char* text36_1 = "a\nb";
    const char* text36_2 = "axb";
    vox_regex_t* regex36 = vox_regex_compile(mpool, pattern36, VOX_REGEX_NONE);
    if (regex36) {
        vox_regex_match_t match36_1, match36_2;
        bool m1 = vox_regex_search(regex36, text36_1, strlen(text36_1), 0, &match36_1);
        bool m2 = vox_regex_search(regex36, text36_2, strlen(text36_2), 0, &match36_2);
        printf("模式: %s\n", pattern36);
        printf("文本 '%s': %s", text36_1, m1 ? "匹配" : "不匹配");
        if (m1) {
            printf(" (位置 %zu-%zu)", match36_1.start, match36_1.end);
        }
        printf("\n");
        printf("文本 '%s': %s", text36_2, m2 ? "匹配" : "不匹配");
        if (m2) {
            printf(" (位置 %zu-%zu)", match36_2.start, match36_2.end);
        }
        printf("\n\n");
        vox_regex_destroy(regex36);
    }
    
    /* 测试37: 词边界 \b */
    printf("测试37: 词边界 \\b\n");
    const char* pattern37 = "\\bhello\\b";
    const char* text37_1 = "hello world";
    const char* text37_2 = "hello123";
    const char* text37_3 = "say hello";
    vox_regex_t* regex37 = vox_regex_compile(mpool, pattern37, VOX_REGEX_NONE);
    if (regex37) {
        vox_regex_match_t match37_1, match37_2, match37_3;
        bool m1 = vox_regex_search(regex37, text37_1, strlen(text37_1), 0, &match37_1);
        bool m2 = vox_regex_search(regex37, text37_2, strlen(text37_2), 0, &match37_2);
        bool m3 = vox_regex_search(regex37, text37_3, strlen(text37_3), 0, &match37_3);
        printf("模式: %s\n", pattern37);
        printf("文本 '%s': %s", text37_1, m1 ? "匹配" : "不匹配");
        if (m1) {
            printf(" (位置 %zu-%zu)", match37_1.start, match37_1.end);
        }
        printf("\n");
        printf("文本 '%s': %s", text37_2, m2 ? "匹配" : "不匹配");
        if (m2) {
            printf(" (位置 %zu-%zu)", match37_2.start, match37_2.end);
        }
        printf("\n");
        printf("文本 '%s': %s", text37_3, m3 ? "匹配" : "不匹配");
        if (m3) {
            printf(" (位置 %zu-%zu)", match37_3.start, match37_3.end);
        }
        printf("\n\n");
        vox_regex_destroy(regex37);
    }
    
    /* ===== 常见实用示例 ===== */
    printf("=== 常见实用示例 ===\n\n");
    
    /* 示例38: 邮箱地址验证 */
    printf("示例38: 邮箱地址验证\n");
    const char* pattern38 = "^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}$";
    const char* emails[] = {
        "user@example.com",
        "test.email@domain.co.uk",
        "invalid.email",
        "user@domain",
        "user.name@example.com"
    };
    vox_regex_t* regex38 = vox_regex_compile(mpool, pattern38, VOX_REGEX_NONE);
    if (regex38) {
        printf("模式: %s\n", pattern38);
        for (size_t i = 0; i < sizeof(emails) / sizeof(emails[0]); i++) {
            bool matched = vox_regex_match(regex38, emails[i], strlen(emails[i]), NULL);
            printf("  '%s': %s\n", emails[i], matched ? "有效" : "无效");
        }
        printf("\n");
        vox_regex_destroy(regex38);
    }
    
    /* 示例39: 中国手机号码 */
    printf("示例39: 中国手机号码验证\n");
    const char* pattern39 = "^1[3-9]\\d{9}$";
    const char* phones[] = {
        "13812345678",
        "15987654321",
        "18800001111",
        "12345678901",
        "1381234567",
        "138123456789"
    };
    vox_regex_t* regex39 = vox_regex_compile(mpool, pattern39, VOX_REGEX_NONE);
    if (regex39) {
        printf("模式: %s\n", pattern39);
        for (size_t i = 0; i < sizeof(phones) / sizeof(phones[0]); i++) {
            bool matched = vox_regex_match(regex39, phones[i], strlen(phones[i]), NULL);
            printf("  '%s': %s\n", phones[i], matched ? "有效" : "无效");
        }
        printf("\n");
        vox_regex_destroy(regex39);
    }
    
    /* 示例40: IP地址验证 */
    printf("示例40: IP地址验证 (IPv4)\n");
    /* 使用更简单的模式，避免非捕获组和量词组合可能的问题 */
    /* 注意：此模式只验证格式，不验证数值范围（0-255），256.1.1.1也会通过格式验证 */
    const char* pattern40 = "^[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}$";
    const char* ips[] = {
        "192.168.1.1",
        "10.0.0.1",
        "255.255.255.255",
        "256.1.1.1",
        "192.168.1",
        "192.168.1.1.1"
    };
    vox_regex_t* regex40 = vox_regex_compile(mpool, pattern40, VOX_REGEX_NONE);
    if (regex40) {
        printf("模式: %s\n", pattern40);
        printf("(注意：只验证格式，不验证数值范围)\n");
        for (size_t i = 0; i < sizeof(ips) / sizeof(ips[0]); i++) {
            bool matched = vox_regex_match(regex40, ips[i], strlen(ips[i]), NULL);
            printf("  '%s': %s\n", ips[i], matched ? "格式有效" : "格式无效");
        }
        printf("\n");
        vox_regex_destroy(regex40);
    }
    
    /* 示例41: URL验证 */
    printf("示例41: URL验证\n");
    /* 添加端口号支持 (:端口号) */
    const char* pattern41 = "^(https?|ftp)://[\\w\\-]+(\\.[\\w\\-]+)*(:[0-9]+)?([\\w\\-\\.,@?^=%&:/~\\+#]*[\\w\\-\\@?^=%&/~\\+#])?$";
    const char* urls[] = {
        "http://www.example.com",
        "https://example.com/path?query=1",
        "ftp://ftp.example.com",
        "invalid.url",
        "http://localhost:8080"
    };
    vox_regex_t* regex41 = vox_regex_compile(mpool, pattern41, VOX_REGEX_NONE);
    if (regex41) {
        printf("模式: %s\n", pattern41);
        for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
            bool matched = vox_regex_match(regex41, urls[i], strlen(urls[i]), NULL);
            printf("  '%s': %s\n", urls[i], matched ? "有效" : "无效");
        }
        printf("\n");
        vox_regex_destroy(regex41);
    }
    
    /* 示例42: 日期格式 (YYYY-MM-DD) */
    printf("示例42: 日期格式验证 (YYYY-MM-DD)\n");
    const char* pattern42 = "^\\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[12]\\d|3[01])$";
    const char* dates[] = {
        "2024-01-15",
        "2024-12-31",
        "2024-02-29",
        "2024-13-01",
        "2024-01-32",
        "24-01-15"
    };
    vox_regex_t* regex42 = vox_regex_compile(mpool, pattern42, VOX_REGEX_NONE);
    if (regex42) {
        printf("模式: %s\n", pattern42);
        for (size_t i = 0; i < sizeof(dates) / sizeof(dates[0]); i++) {
            bool matched = vox_regex_match(regex42, dates[i], strlen(dates[i]), NULL);
            printf("  '%s': %s\n", dates[i], matched ? "格式有效" : "格式无效");
        }
        printf("\n");
        vox_regex_destroy(regex42);
    }
    
    /* 示例43: 中国身份证号码 */
    printf("示例43: 中国身份证号码验证 (18位)\n");
    const char* pattern43 = "^[1-9]\\d{5}(18|19|20)\\d{2}(0[1-9]|1[0-2])(0[1-9]|[12]\\d|3[01])\\d{3}[0-9Xx]$";
    const char* ids[] = {
        "110101199001011234",
        "32010119851215123X",
        "123456789012345678",
        "11010119900101123",
        "1101011990010112345"
    };
    vox_regex_t* regex43 = vox_regex_compile(mpool, pattern43, VOX_REGEX_NONE);
    if (regex43) {
        printf("模式: %s\n", pattern43);
        for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
            bool matched = vox_regex_match(regex43, ids[i], strlen(ids[i]), NULL);
            printf("  '%s': %s\n", ids[i], matched ? "格式有效" : "格式无效");
        }
        printf("\n");
        vox_regex_destroy(regex43);
    }
    
    /* 示例44: 中国邮政编码 */
    printf("示例44: 中国邮政编码验证\n");
    const char* pattern44 = "^[1-9]\\d{5}$";
    const char* postcodes[] = {
        "100000",
        "200000",
        "310000",
        "012345",
        "12345",
        "1234567"
    };
    vox_regex_t* regex44 = vox_regex_compile(mpool, pattern44, VOX_REGEX_NONE);
    if (regex44) {
        printf("模式: %s\n", pattern44);
        for (size_t i = 0; i < sizeof(postcodes) / sizeof(postcodes[0]); i++) {
            bool matched = vox_regex_match(regex44, postcodes[i], strlen(postcodes[i]), NULL);
            printf("  '%s': %s\n", postcodes[i], matched ? "有效" : "无效");
        }
        printf("\n");
        vox_regex_destroy(regex44);
    }
    
    /* 示例45: 提取文本中的邮箱地址 */
    printf("示例45: 从文本中提取邮箱地址\n");
    const char* text45 = "联系我: user1@example.com 或 user2@test.org，也可以发到 admin@company.cn";
    const char* pattern45 = "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}";
    vox_regex_t* regex45 = vox_regex_compile(mpool, pattern45, VOX_REGEX_NONE);
    if (regex45) {
        vox_regex_match_t* matches45 = NULL;
        size_t match_count45 = 0;
        if (vox_regex_findall(regex45, text45, strlen(text45), &matches45, &match_count45) == 0) {
            printf("文本: %s\n", text45);
            printf("模式: %s\n", pattern45);
            printf("找到 %zu 个邮箱地址:\n", match_count45);
            for (size_t i = 0; i < match_count45; i++) {
                printf("  %zu. ", i + 1);
                for (size_t j = matches45[i].start; j < matches45[i].end; j++) {
                    putchar(text45[j]);
                }
                printf(" (位置 %zu-%zu)\n", matches45[i].start, matches45[i].end);
            }
            printf("\n");
        }
        vox_regex_destroy(regex45);
    }
    
    /* 示例46: 提取文本中的电话号码 */
    printf("示例46: 从文本中提取手机号码\n");
    const char* text46 = "我的电话是13812345678，备用号码15987654321，办公室电话010-12345678";
    const char* pattern46 = "1[3-9]\\d{9}";
    vox_regex_t* regex46 = vox_regex_compile(mpool, pattern46, VOX_REGEX_NONE);
    if (regex46) {
        vox_regex_match_t* matches46 = NULL;
        size_t match_count46 = 0;
        if (vox_regex_findall(regex46, text46, strlen(text46), &matches46, &match_count46) == 0) {
            printf("文本: %s\n", text46);
            printf("模式: %s\n", pattern46);
            printf("找到 %zu 个手机号码:\n", match_count46);
            for (size_t i = 0; i < match_count46; i++) {
                printf("  %zu. ", i + 1);
                for (size_t j = matches46[i].start; j < matches46[i].end; j++) {
                    putchar(text46[j]);
                }
                printf(" (位置 %zu-%zu)\n", matches46[i].start, matches46[i].end);
            }
            printf("\n");
        }
        vox_regex_destroy(regex46);
    }
    
    /* 示例47: 验证密码强度（至少8位，包含大小写字母和数字） */
    printf("示例47: 密码强度验证（至少8位，包含大小写字母和数字）\n");
    /* 使用正向先行断言确保包含：小写字母、大写字母、数字 */
    const char* pattern47 = "^(?=.*[a-z])(?=.*[A-Z])(?=.*\\d)[a-zA-Z\\d@$!%*?&]{8,}$";
    const char* passwords[] = {
        "Password123",  /* 有效：有大写、有小写、有数字、长度>=8 */
        "weak",         /* 无效：长度不足 */
        "12345678",     /* 无效：无字母 */
        "PASSWORD123",  /* 无效：无小写字母 */
        "password123",  /* 无效：无大写字母 */
        "Pass123"       /* 无效：长度不足 */
    };
    vox_regex_t* regex47 = vox_regex_compile(mpool, pattern47, VOX_REGEX_NONE);
    if (regex47) {
        printf("模式: %s\n", pattern47);
        for (size_t i = 0; i < sizeof(passwords) / sizeof(passwords[0]); i++) {
            bool matched = vox_regex_match(regex47, passwords[i], strlen(passwords[i]), NULL);
            printf("  '%s': %s\n", passwords[i], matched ? "格式有效" : "格式无效");
        }
        printf("\n");
        vox_regex_destroy(regex47);
    }
    
    /* 示例48: 提取HTML标签中的内容 */
    printf("示例48: 提取HTML标签中的内容\n");
    const char* text48 = "<title>网页标题</title><p>段落内容</p>";
    /* 直接匹配标签内容部分（在>和</之间），手动提取内容 */
    const char* pattern48 = ">[^<]+</";
    vox_regex_t* regex48 = vox_regex_compile(mpool, pattern48, VOX_REGEX_NONE);
    if (regex48) {
        printf("文本: %s\n", text48);
        printf("模式: %s (提取标签内容)\n", pattern48);
        size_t pos = 0;
        size_t count = 0;
        while (pos < strlen(text48)) {
            vox_regex_match_t match;
            if (vox_regex_search(regex48, text48, strlen(text48), pos, &match)) {
                count++;
                printf("  匹配 %zu: ", count);
                /* 手动提取内容（跳过开头的>，去掉结尾的</） */
                size_t content_start = match.start + 1;  /* 跳过 > */
                size_t content_end = match.end - 2;       /* 去掉 </ */
                for (size_t j = content_start; j < content_end; j++) {
                    putchar(text48[j]);
                }
                printf(" (位置 %zu-%zu)\n", content_start, content_end);
                pos = match.end;
            } else {
                break;
            }
        }
        if (count == 0) {
            printf("  未找到匹配\n");
        }
        printf("\n");
        vox_regex_destroy(regex48);
    }

    /* 示例49: 非贪婪量词 */
    printf("示例49: 非贪婪量词 (*?, +?, ??" "?)\n");
    const char* text49 = "<div>First</div><div>Second</div>";
    const char* pattern49_greedy = "<div>.*</div>";
    const char* pattern49_nongreedy = "<div>.*?</div>";
    
    printf("文本: %s\n", text49);
    
    /* 贪婪匹配 */
    vox_regex_t* regex49_g = vox_regex_compile(mpool, pattern49_greedy, VOX_REGEX_NONE);
    if (regex49_g) {
        vox_regex_match_t m;
        if (vox_regex_search(regex49_g, text49, strlen(text49), 0, &m)) {
            printf("  贪婪模式 (%s) 匹配: ", pattern49_greedy);
            for (size_t i = m.start; i < m.end; i++) putchar(text49[i]);
            printf(" (位置 %zu-%zu)\n", m.start, m.end);
        }
        vox_regex_destroy(regex49_g);
    }
    
    /* 非贪婪匹配 */
    vox_regex_t* regex49_ng = vox_regex_compile(mpool, pattern49_nongreedy, VOX_REGEX_NONE);
    if (regex49_ng) {
        vox_regex_match_t m;
        size_t pos = 0;
        printf("  非贪婪模式 (%s) 匹配结果:\n", pattern49_nongreedy);
        while (vox_regex_search(regex49_ng, text49, strlen(text49), pos, &m)) {
            printf("    找到: ");
            for (size_t i = m.start; i < m.end; i++) putchar(text49[i]);
            printf(" (位置 %zu-%zu)\n", m.start, m.end);
            pos = m.end;
        }
        vox_regex_destroy(regex49_ng);
    }
    printf("\n");

    /* 示例50: 后行断言 (Lookbehind) */
    printf("示例50: 正向后行断言 (?<=pattern)\n");
    const char* text50 = "Apple: $1.50, Orange: $2.00, Banana: free";
    /* 匹配紧跟在 '$' 之后的数字金额 */
    const char* pattern50 = "(?<=\\$)\\d+\\.\\d{2}";
    vox_regex_t* regex50 = vox_regex_compile(mpool, pattern50, VOX_REGEX_NONE);
    if (regex50) {
        printf("文本: %s\n", text50);
        printf("模式: %s (匹配紧跟在$后的价格)\n", pattern50);
        vox_regex_match_t* ms = NULL;
        size_t count = 0;
        if (vox_regex_findall(regex50, text50, strlen(text50), &ms, &count) == 0) {
            for (size_t i = 0; i < count; i++) {
                printf("  价格 %zu: ", i + 1);
                for (size_t j = ms[i].start; j < ms[i].end; j++) putchar(text50[j]);
                printf(" (位置 %zu-%zu)\n", ms[i].start, ms[i].end);
            }
        }
        vox_regex_destroy(regex50);
    }
    printf("\n");

    /* 示例51: 否定断言 (Negative Lookaround) */
    printf("示例51: 否定断言 (?!, (?<!))\n");
    const char* text51 = "bat cat rat mat";
    /* 匹配不以 'c' 开头的 at 结尾的单词 */
    const char* pattern51 = "\\b(?!c)[a-z]at\\b";
    vox_regex_t* regex51 = vox_regex_compile(mpool, pattern51, VOX_REGEX_NONE);
    if (regex51) {
        printf("文本: %s\n", text51);
        printf("模式: %s (不以c开头的at单词)\n", pattern51);
        vox_regex_match_t* ms = NULL;
        size_t count = 0;
        if (vox_regex_findall(regex51, text51, strlen(text51), &ms, &count) == 0) {
            for (size_t i = 0; i < count; i++) {
                printf("  匹配 %zu: ", i + 1);
                for (size_t j = ms[i].start; j < ms[i].end; j++) putchar(text51[j]);
                printf(" (位置 %zu-%zu)\n", ms[i].start, ms[i].end);
            }
        }
        vox_regex_destroy(regex51);
    }
    printf("\n");
    
    /* 清理 */
    vox_mpool_destroy(mpool);
    
    return 0;
}
