/*
 * vox_regex.h - 高性能正则表达式引擎
 * 使用NFA（非确定性有限自动机）实现，支持基本正则表达式功能
 */

#ifndef VOX_REGEX_H
#define VOX_REGEX_H

#include "vox_os.h"
#include "vox_mpool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 正则表达式对象 ===== */

/* 正则表达式不透明类型 */
typedef struct vox_regex vox_regex_t;

/* 正则表达式编译选项 */
typedef enum {
    VOX_REGEX_NONE = 0,           /* 无选项 */
    VOX_REGEX_IGNORE_CASE = 1,    /* 忽略大小写 */
    VOX_REGEX_MULTILINE = 2,      /* 多行模式（^和$匹配行首行尾） */
    VOX_REGEX_DOTALL = 4          /* .匹配换行符 */
} vox_regex_flag_t;

/**
 * 编译正则表达式模式
 * @param mpool 内存池指针，必须非NULL
 * @param pattern 正则表达式模式字符串
 * @param flags 编译选项标志（多个选项用|组合）
 * @return 成功返回正则表达式对象指针，失败返回NULL
 * 
 * 支持的正则表达式语法：
 * - 基础字符：普通字符匹配自身，支持 \ 引导的转义
 * - . : 匹配任意字符（默认不匹配换行符，除非使用VOX_REGEX_DOTALL）
 * - ^ : 匹配字符串/行开始
 * - $ : 匹配字符串/行结束
 * - \b : 匹配单词边界
 * - [abc] : 字符类，匹配a、b或c
 * - [^abc] : 否定字符类，匹配除a、b、c外的字符
 * - [a-z] : 字符范围，匹配a到z之间的字符
 * - \d, \D : 匹配数字/非数字字符
 * - \w, \W : 匹配单词/非单词字符
 * - \s, \S : 匹配空白/非空白字符
 * - *, +, ? : 贪婪量词（0+次, 1+次, 0/1次）
 * - *?, +?, ?? : 非贪婪量词
 * - {n}, {n,}, {n,m} : 范围量词（及其非贪婪形式 {n,m}?）
 * - | : 选择，匹配左侧或右侧
 * - (pattern) : 捕获组
 * - (?:pattern) : 非捕获组
 * - (?=pattern) : 正向先行断言 (Positive Lookahead)
 * - (?!pattern) : 负向先行断言 (Negative Lookahead)
 * - (?<=pattern) : 正向后行断言 (Positive Lookbehind)
 * - (?<!pattern) : 负向后行断言 (Negative Lookbehind)
 */
vox_regex_t* vox_regex_compile(vox_mpool_t* mpool, const char* pattern, int flags);

/**
 * 销毁正则表达式对象（释放内存池中的资源）
 * @param regex 正则表达式对象指针
 */
void vox_regex_destroy(vox_regex_t* regex);

/* ===== 匹配结果 ===== */

/* 匹配结果结构 */
typedef struct {
    size_t start;   /* 匹配开始位置（字节偏移） */
    size_t end;     /* 匹配结束位置（字节偏移，不包含） */
} vox_regex_match_t;

/* 匹配结果集合（包含完整匹配和所有捕获组） */
typedef struct {
    vox_regex_match_t* matches;  /* 匹配结果数组 */
    size_t count;                 /* 匹配结果数量（包括完整匹配和捕获组） */
    size_t capacity;              /* 数组容量（内部使用） */
} vox_regex_matches_t;

/**
 * 匹配字符串
 * @param regex 正则表达式对象指针
 * @param text 要匹配的文本
 * @param text_len 文本长度（字节数）
 * @param matches 输出匹配结果集合（可为NULL，如果不关心捕获组）
 * @return 匹配成功返回true，失败返回false
 * 
 * 注意：matches结构体由调用者分配，但matches->matches数组会在函数内部分配（使用内存池）
 */
bool vox_regex_match(vox_regex_t* regex, const char* text, size_t text_len, 
                     vox_regex_matches_t* matches);

/**
 * 查找第一个匹配
 * @param regex 正则表达式对象指针
 * @param text 要搜索的文本
 * @param text_len 文本长度（字节数）
 * @param start_pos 搜索起始位置（字节偏移）
 * @param match 输出匹配结果（可为NULL）
 * @return 找到匹配返回true，否则返回false
 */
bool vox_regex_search(vox_regex_t* regex, const char* text, size_t text_len, 
                      size_t start_pos, vox_regex_match_t* match);

/**
 * 查找所有匹配
 * @param regex 正则表达式对象指针
 * @param text 要搜索的文本
 * @param text_len 文本长度（字节数）
 * @param matches 输出匹配结果数组（由函数分配，使用内存池）
 * @param match_count 输出匹配数量
 * @return 成功返回0，失败返回-1
 * 
 * 注意：matches数组由函数内部分配（使用内存池），调用者不需要释放
 */
int vox_regex_findall(vox_regex_t* regex, const char* text, size_t text_len,
                      vox_regex_match_t** matches, size_t* match_count);

/**
 * 替换匹配的字符串
 * @param regex 正则表达式对象指针
 * @param text 原始文本
 * @param text_len 文本长度（字节数）
 * @param replacement 替换字符串（支持$1, $2等引用捕获组）
 * @param output 输出缓冲区（由调用者分配）
 * @param output_size 输出缓冲区大小
 * @param output_len 输出实际长度
 * @return 成功返回0，失败返回-1
 */
int vox_regex_replace(vox_regex_t* regex, const char* text, size_t text_len,
                      const char* replacement, char* output, size_t output_size,
                      size_t* output_len);

/**
 * 释放匹配结果数组（如果使用vox_regex_findall）
 * @param regex 正则表达式对象指针（用于获取内存池）
 * @param matches 匹配结果数组
 * @param match_count 匹配数量
 * 
 * 注意：实际上由于使用内存池，此函数可能为空操作，但为了API一致性保留
 */
void vox_regex_free_matches(vox_regex_t* regex, vox_regex_match_t* matches, size_t match_count);

#ifdef __cplusplus
}
#endif

#endif /* VOX_REGEX_H */
