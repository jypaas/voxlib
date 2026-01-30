/*
 * vox_ini.h - 高性能 INI 解析器和生成器
 * 使用 vox_scanner 实现解析，使用 vox_mpool 进行内存管理
 */

#ifndef VOX_INI_H
#define VOX_INI_H

#include "vox_os.h"
#include "vox_scanner.h"
#include "vox_mpool.h"
#include "vox_list.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== INI 数据结构 ===== */

/**
 * INI 键值对
 */
typedef struct {
    vox_list_node_t node;        /* 链表节点 */
    char* key;                   /* 键名（使用 mpool 分配的副本，支持修改） */
    char* value;                 /* 值（使用 mpool 分配的副本，支持修改） */
    char* comment;               /* 行内注释（可选） */
} vox_ini_keyvalue_t;

/**
 * INI 节 (Section)
 */
typedef struct {
    vox_list_node_t node;        /* 链表节点 */
    char* name;                  /* 节名（NULL 表示全局/默认节） */
    vox_list_t keyvalues;        /* 键值对链表 */
    char* comment;               /* 节之前的注释或行内注释（可选） */
} vox_ini_section_t;

/**
 * INI 对象
 */
typedef struct {
    vox_mpool_t* mpool;          /* 内存池 */
    vox_list_t sections;         /* 节链表 */
} vox_ini_t;

/* ===== 错误信息 ===== */

typedef struct {
    int line;
    int column;
    const char* message;
} vox_ini_err_info_t;

/* ===== 解析接口 ===== */

/**
 * 创建一个新的 INI 对象
 * @param mpool 内存池
 * @return 成功返回对象指针，失败返回 NULL
 */
vox_ini_t* vox_ini_create(vox_mpool_t* mpool);

/**
 * 解析 INI 字符串
 * @param mpool 内存池
 * @param ini_str INI 内容字符串
 * @param err_info 错误信息输出（可选）
 * @return 成功返回 INI 对象，失败返回 NULL
 */
vox_ini_t* vox_ini_parse(vox_mpool_t* mpool, const char* ini_str, vox_ini_err_info_t* err_info);

/**
 * 解析 INI 文件
 * @param mpool 内存池
 * @param filepath 文件路径
 * @param err_info 错误信息输出（可选）
 * @return 成功返回 INI 对象，失败返回 NULL
 */
vox_ini_t* vox_ini_parse_file(vox_mpool_t* mpool, const char* filepath, vox_ini_err_info_t* err_info);

/**
 * 销毁 INI 对象
 * @param ini 对象指针
 */
void vox_ini_destroy(vox_ini_t* ini);

/* ===== 数据操作接口 ===== */

/**
 * 获取指定节中的值
 * @param ini 对象指针
 * @param section_name 节名（NULL 表示全局节）
 * @param key 键名
 * @return 返回值字符串，未找到返回 NULL
 */
const char* vox_ini_get_value(const vox_ini_t* ini, const char* section_name, const char* key);

/**
 * 设置指定节中的值（如果不存在则创建）
 * @param ini 对象指针
 * @param section_name 节名（NULL 表示全局节）
 * @param key 键名
 * @param value 值
 * @return 成功返回 0，失败返回 -1
 */
int vox_ini_set_value(vox_ini_t* ini, const char* section_name, const char* key, const char* value);

/**
 * 删除键值对
 * @param ini 对象指针
 * @param section_name 节名
 * @param key 键名
 * @return 成功返回 0，失败返回 -1
 */
int vox_ini_remove_key(vox_ini_t* ini, const char* section_name, const char* key);

/**
 * 删除整个节
 * @param ini 对象指针
 * @param section_name 节名
 * @return 成功返回 0，失败返回 -1
 */
int vox_ini_remove_section(vox_ini_t* ini, const char* section_name);

/* ===== 序列化接口 ===== */

/**
 * 将 INI 对象转换为字符串
 * @param ini 对象指针
 * @param out_size 输出字符串长度
 * @return 返回字符串指针（由 mpool 管理），失败返回 NULL
 */
char* vox_ini_to_string(const vox_ini_t* ini, size_t* out_size);

/**
 * 将 INI 对象写入文件
 * @param ini 对象指针
 * @param filepath 文件路径
 * @return 成功返回 0，失败返回 -1
 */
int vox_ini_write_file(const vox_ini_t* ini, const char* filepath);

#ifdef __cplusplus
}
#endif

#endif /* VOX_INI_H */
