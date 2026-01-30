/*
 * vox_string.h - 高性能字符串处理
 * 使用 vox_mpool 内存池管理所有内存分配
 */

#ifndef VOX_STRING_H
#define VOX_STRING_H

#include "vox_mpool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 字符串视图结构（零拷贝字符串表示） ===== */

/**
 * 字符串视图结构
 * 包含指向原始缓冲区的指针和长度，不复制数据
 */
typedef struct {
    const char* ptr;    /* 字符串指针（指向原始缓冲区） */
    size_t len;         /* 字符串长度（字节数） */
} vox_strview_t;

/* 字符串视图常量 */
#define VOX_STRVIEW_NULL { NULL, 0 }
#define VOX_STRVIEW_INIT(p, l) { (p), (l) }

/**
 * 从C字符串创建字符串视图
 * @param cstr C字符串（必须以'\0'结尾）
 * @return 字符串视图
 */
vox_strview_t vox_strview_from_cstr(const char* cstr);

/**
 * 从指针和长度创建字符串视图
 * @param ptr 字符串指针
 * @param len 长度
 * @return 字符串视图
 */
vox_strview_t vox_strview_from_ptr(const char* ptr, size_t len);

/**
 * 检查字符串视图是否为空
 * @param sv 字符串视图
 * @return 为空返回true，否则返回false
 */
bool vox_strview_empty(const vox_strview_t* sv);

/**
 * 比较两个字符串视图
 * @param sv1 字符串视图1
 * @param sv2 字符串视图2
 * @return sv1 < sv2 返回负数，sv1 == sv2 返回0，sv1 > sv2 返回正数
 */
int vox_strview_compare(const vox_strview_t* sv1, const vox_strview_t* sv2);

/**
 * 与C字符串比较
 * @param sv 字符串视图
 * @param cstr C字符串
 * @return sv < cstr 返回负数，sv == cstr 返回0，sv > cstr 返回正数
 */
int vox_strview_compare_cstr(const vox_strview_t* sv, const char* cstr);

/* ===== 字符串对象 ===== */

/* 字符串不透明类型 */
typedef struct vox_string vox_string_t;

/* 字符串配置 */
typedef struct {
    size_t initial_capacity;  /* 初始容量，0表示使用默认值 */
} vox_string_config_t;

/**
 * 使用默认配置创建空字符串
 * @param mpool 内存池指针，必须非NULL
 * @return 成功返回字符串指针，失败返回NULL
 */
vox_string_t* vox_string_create(vox_mpool_t* mpool);

/**
 * 使用自定义配置创建空字符串
 * @param mpool 内存池指针，必须非NULL
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回字符串指针，失败返回NULL
 */
vox_string_t* vox_string_create_with_config(vox_mpool_t* mpool, const vox_string_config_t* config);

/**
 * 从C字符串创建字符串对象
 * @param mpool 内存池指针，必须非NULL
 * @param str C字符串（可以为NULL，创建空字符串）
 * @return 成功返回字符串指针，失败返回NULL
 */
vox_string_t* vox_string_from_cstr(vox_mpool_t* mpool, const char* str);

/**
 * 从指定长度的数据创建字符串对象
 * @param mpool 内存池指针，必须非NULL
 * @param data 数据指针
 * @param len 数据长度
 * @return 成功返回字符串指针，失败返回NULL
 */
vox_string_t* vox_string_from_data(vox_mpool_t* mpool, const void* data, size_t len);

/**
 * 复制字符串对象
 * @param mpool 内存池指针，必须非NULL
 * @param src 源字符串
 * @return 成功返回新字符串指针，失败返回NULL
 */
vox_string_t* vox_string_clone(vox_mpool_t* mpool, const vox_string_t* src);

/**
 * 清空字符串（保留容量）
 * @param str 字符串指针
 */
void vox_string_clear(vox_string_t* str);

/**
 * 销毁字符串并释放所有资源
 * @param str 字符串指针
 */
void vox_string_destroy(vox_string_t* str);

/**
 * 获取字符串长度
 * @param str 字符串指针
 * @return 返回字符串长度（字节数，不包括结尾的'\0'）
 */
size_t vox_string_length(const vox_string_t* str);

/**
 * 获取字符串容量
 * @param str 字符串指针
 * @return 返回字符串容量
 */
size_t vox_string_capacity(const vox_string_t* str);

/**
 * 检查字符串是否为空
 * @param str 字符串指针
 * @return 为空返回true，否则返回false
 */
bool vox_string_empty(const vox_string_t* str);

/**
 * 获取C字符串指针（以'\0'结尾）
 * @param str 字符串指针
 * @return 返回C字符串指针，失败返回NULL
 */
const char* vox_string_cstr(const vox_string_t* str);

/**
 * 获取原始数据指针
 * @param str 字符串指针
 * @return 返回数据指针，失败返回NULL
 */
const void* vox_string_data(const vox_string_t* str);

/**
 * 设置字符串内容
 * @param str 字符串指针
 * @param cstr C字符串
 * @return 成功返回0，失败返回-1
 */
int vox_string_set(vox_string_t* str, const char* cstr);

/**
 * 设置字符串内容（指定长度）
 * @param str 字符串指针
 * @param data 数据指针
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int vox_string_set_data(vox_string_t* str, const void* data, size_t len);

/**
 * 设置字符串视图内容（优化：避免 strlen 调用）
 * @param str 字符串指针
 * @param view 字符串视图
 * @return 成功返回0，失败返回-1
 */
int vox_string_set_view(vox_string_t* str, const vox_strview_t* view);

/**
 * 追加C字符串
 * @param str 字符串指针
 * @param cstr C字符串
 * @return 成功返回0，失败返回-1
 */
int vox_string_append(vox_string_t* str, const char* cstr);

/**
 * 追加数据
 * @param str 字符串指针
 * @param data 数据指针
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int vox_string_append_data(vox_string_t* str, const void* data, size_t len);

/**
 * 追加另一个字符串对象
 * @param str 目标字符串指针
 * @param other 源字符串指针
 * @return 成功返回0，失败返回-1
 */
int vox_string_append_string(vox_string_t* str, const vox_string_t* other);

/**
 * 追加单个字符
 * @param str 字符串指针
 * @param ch 字符
 * @return 成功返回0，失败返回-1
 */
int vox_string_append_char(vox_string_t* str, char ch);

/**
 * 格式化追加（类似 sprintf）
 * @param str 字符串指针
 * @param fmt 格式字符串
 * @param ... 可变参数
 * @return 成功返回追加的字符数，失败返回-1
 */
int vox_string_append_format(vox_string_t* str, const char* fmt, ...);

/**
 * 格式化追加（va_list版本）
 * @param str 字符串指针
 * @param fmt 格式字符串
 * @param args 可变参数列表
 * @return 成功返回追加的字符数，失败返回-1
 */
int vox_string_append_vformat(vox_string_t* str, const char* fmt, va_list args);

/**
 * 在指定位置插入字符串
 * @param str 字符串指针
 * @param pos 插入位置
 * @param cstr C字符串
 * @return 成功返回0，失败返回-1
 */
int vox_string_insert(vox_string_t* str, size_t pos, const char* cstr);

/**
 * 在指定位置插入数据
 * @param str 字符串指针
 * @param pos 插入位置
 * @param data 数据指针
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int vox_string_insert_data(vox_string_t* str, size_t pos, const void* data, size_t len);

/**
 * 在指定位置插入字符串视图（优化：避免 strlen 调用）
 * @param str 字符串指针
 * @param pos 插入位置
 * @param view 字符串视图
 * @return 成功返回0，失败返回-1
 */
int vox_string_insert_view(vox_string_t* str, size_t pos, const vox_strview_t* view);

/**
 * 删除指定范围的字符
 * @param str 字符串指针
 * @param pos 起始位置
 * @param len 删除长度
 * @return 成功返回0，失败返回-1
 */
int vox_string_remove(vox_string_t* str, size_t pos, size_t len);

/**
 * 获取指定位置的字符
 * @param str 字符串指针
 * @param index 位置索引
 * @return 成功返回字符，失败返回0
 */
char vox_string_at(const vox_string_t* str, size_t index);

/**
 * 设置指定位置的字符
 * @param str 字符串指针
 * @param index 位置索引
 * @param ch 字符
 * @return 成功返回0，失败返回-1
 */
int vox_string_set_char(vox_string_t* str, size_t index, char ch);

/**
 * 查找子字符串
 * @param str 字符串指针
 * @param substr 子字符串
 * @param start_pos 起始搜索位置
 * @return 找到返回位置索引，未找到返回SIZE_MAX
 */
size_t vox_string_find(const vox_string_t* str, const char* substr, size_t start_pos);

/**
 * 从后往前查找子字符串
 * @param str 字符串指针
 * @param substr 子字符串
 * @param start_pos 起始搜索位置（从后往前，SIZE_MAX表示从末尾开始）
 * @return 找到返回位置索引，未找到返回SIZE_MAX
 */
size_t vox_string_rfind(const vox_string_t* str, const char* substr, size_t start_pos);

/**
 * 替换所有匹配的子字符串
 * @param str 字符串指针
 * @param old_str 要替换的子字符串
 * @param new_str 新字符串
 * @return 成功返回替换次数，失败返回-1
 */
int vox_string_replace(vox_string_t* str, const char* old_str, const char* new_str);

/**
 * 替换指定位置的子字符串
 * @param str 字符串指针
 * @param pos 起始位置
 * @param len 要替换的长度
 * @param new_str 新字符串
 * @return 成功返回0，失败返回-1
 */
int vox_string_replace_at(vox_string_t* str, size_t pos, size_t len, const char* new_str);

/**
 * 比较两个字符串
 * @param str1 字符串1
 * @param str2 字符串2
 * @return str1 < str2 返回负数，str1 == str2 返回0，str1 > str2 返回正数
 */
int vox_string_compare(const vox_string_t* str1, const vox_string_t* str2);

/**
 * 与C字符串比较
 * @param str 字符串对象
 * @param cstr C字符串
 * @return str < cstr 返回负数，str == cstr 返回0，str > cstr 返回正数
 */
int vox_string_compare_cstr(const vox_string_t* str, const char* cstr);

/**
 * 提取子字符串
 * @param mpool 内存池指针，必须非NULL
 * @param str 源字符串
 * @param pos 起始位置
 * @param len 长度（SIZE_MAX表示到末尾）
 * @return 成功返回新字符串指针，失败返回NULL
 */
vox_string_t* vox_string_substr(vox_mpool_t* mpool, const vox_string_t* str, size_t pos, size_t len);

/**
 * 转换为小写
 * @param str 字符串指针
 */
void vox_string_tolower(vox_string_t* str);

/**
 * 转换为大写
 * @param str 字符串指针
 */
void vox_string_toupper(vox_string_t* str);

/**
 * 去除首尾空白字符
 * @param str 字符串指针
 */
void vox_string_trim(vox_string_t* str);

/**
 * 预留容量
 * @param str 字符串指针
 * @param capacity 目标容量
 * @return 成功返回0，失败返回-1
 */
int vox_string_reserve(vox_string_t* str, size_t capacity);

/**
 * 调整字符串大小
 * @param str 字符串指针
 * @param new_size 新大小
 * @return 成功返回0，失败返回-1
 */
int vox_string_resize(vox_string_t* str, size_t new_size);

#ifdef __cplusplus
}
#endif

#endif /* VOX_STRING_H */
