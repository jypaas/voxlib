/*
 * vox_xml.h - 高性能 XML 解析器
 * 使用 vox_scanner 实现零拷贝解析，使用 vox_mpool 进行内存管理
 */

#ifndef VOX_XML_H
#define VOX_XML_H

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

/* ===== XML 节点和属性结构 ===== */

/**
 * XML 属性结构
 */
typedef struct vox_xml_attr {
    vox_list_node_t node;        /* 链表节点（用于属性链表） */
    vox_strview_t name;           /* 属性名（字符串视图，零拷贝） */
    vox_strview_t value;          /* 属性值（字符串视图，零拷贝） */
    struct vox_xml_node* parent;  /* 父节点（用于遍历） */
} vox_xml_attr_t;

/**
 * XML 节点结构
 */
typedef struct vox_xml_node {
    vox_list_node_t node;         /* 链表节点（用于子节点链表） */
    vox_strview_t name;            /* 节点名（字符串视图，零拷贝） */
    vox_strview_t content;         /* 文本内容（字符串视图，零拷贝） */
    vox_list_t children;           /* 子节点链表 */
    vox_list_t attrs;              /* 属性链表 */
    struct vox_xml_node* parent;  /* 父节点（用于遍历） */
} vox_xml_node_t;

/* ===== 错误信息 ===== */

/**
 * XML 解析错误信息
 */
typedef struct {
    int line;                     /* 错误行号（从1开始） */
    int column;                   /* 错误列号（从1开始） */
    size_t offset;                /* 错误位置偏移量 */
    const char* message;          /* 错误消息 */
} vox_xml_err_info_t;

/* ===== 解析接口 ===== */

/**
 * 解析 XML 字符串
 * @param mpool 内存池指针（必须非NULL）
 * @param buffer XML 字符串缓冲区（必须可写，且末尾必须有'\0'）
 * @param size 缓冲区大小（不包括末尾的'\0'），解析后返回实际使用的字节数
 * @param err_info 错误信息输出（可为NULL）
 * @return 成功返回 XML 根节点指针，失败返回 NULL
 * 
 * 注意：
 * - buffer 必须保证在解析期间有效
 * - 解析后的 XML 节点使用字符串视图指向原始 buffer，不复制数据
 * - 解析失败时，如果 err_info 非 NULL，会填充错误信息
 * - 处理指令（<? ... ?>）和注释（<!-- ... -->）会被解析但不会保留在节点树中
 */
vox_xml_node_t* vox_xml_parse(vox_mpool_t* mpool, char* buffer, size_t* size,
                              vox_xml_err_info_t* err_info);

/**
 * 解析 XML 字符串（从 C 字符串）
 * @param mpool 内存池指针（必须非NULL）
 * @param xml_str XML 字符串（必须以'\0'结尾）
 * @param err_info 错误信息输出（可为NULL）
 * @return 成功返回 XML 根节点指针，失败返回 NULL
 */
vox_xml_node_t* vox_xml_parse_str(vox_mpool_t* mpool, const char* xml_str,
                                  vox_xml_err_info_t* err_info);

/**
 * 从文件解析 XML
 * @param mpool 内存池指针（必须非NULL）
 * @param filepath 文件路径
 * @param err_info 错误信息输出（可为NULL）
 * @return 成功返回 XML 根节点指针，失败返回 NULL
 */
vox_xml_node_t* vox_xml_parse_file(vox_mpool_t* mpool, const char* filepath,
                                   vox_xml_err_info_t* err_info);

/* ===== 节点创建接口 ===== */

/**
 * 创建新的 XML 节点
 * @param mpool 内存池指针（必须非NULL）
 * @param name 节点名称
 * @return 成功返回节点指针，失败返回 NULL
 */
vox_xml_node_t* vox_xml_node_new(vox_mpool_t* mpool, const vox_strview_t* name);

/**
 * 创建新的 XML 属性
 * @param mpool 内存池指针（必须非NULL）
 * @param name 属性名称
 * @param value 属性值
 * @return 成功返回属性指针，失败返回 NULL
 */
vox_xml_attr_t* vox_xml_attr_new(vox_mpool_t* mpool, const vox_strview_t* name,
                                  const vox_strview_t* value);

/**
 * 克隆 XML 节点（深拷贝）
 * @param mpool 内存池指针（必须非NULL）
 * @param src 源节点指针
 * @return 成功返回克隆的节点指针，失败返回 NULL
 */
vox_xml_node_t* vox_xml_clone(vox_mpool_t* mpool, const vox_xml_node_t* src);

/* ===== 节点操作接口 ===== */

/**
 * 添加子节点
 * @param parent 父节点指针
 * @param child 子节点指针
 */
void vox_xml_add_child(vox_xml_node_t* parent, vox_xml_node_t* child);

/**
 * 添加属性
 * @param node 节点指针
 * @param attr 属性指针
 */
void vox_xml_add_attr(vox_xml_node_t* node, vox_xml_attr_t* attr);

/**
 * 设置节点文本内容
 * @param node 节点指针
 * @param content 文本内容
 */
void vox_xml_set_content(vox_xml_node_t* node, const vox_strview_t* content);

/* ===== 查询接口 ===== */

/**
 * 获取节点名称
 * @param node 节点指针
 * @return 返回节点名称的字符串视图，如果 node 为 NULL 返回 VOX_STRVIEW_NULL
 */
vox_strview_t vox_xml_get_name(const vox_xml_node_t* node);

/**
 * 获取节点文本内容
 * @param node 节点指针
 * @return 返回文本内容的字符串视图，如果 node 为 NULL 返回 VOX_STRVIEW_NULL
 */
vox_strview_t vox_xml_get_content(const vox_xml_node_t* node);

/**
 * 获取子节点数量
 * @param node 节点指针
 * @return 返回子节点数量，如果 node 为 NULL 返回 0
 */
size_t vox_xml_get_child_count(const vox_xml_node_t* node);

/**
 * 获取属性数量
 * @param node 节点指针
 * @return 返回属性数量，如果 node 为 NULL 返回 0
 */
size_t vox_xml_get_attr_count(const vox_xml_node_t* node);

/**
 * 查找子节点（通过名称）
 * @param node 节点指针
 * @param name 子节点名称
 * @return 成功返回子节点指针，失败返回 NULL
 */
vox_xml_node_t* vox_xml_find_child(const vox_xml_node_t* node, const char* name);

/**
 * 查找属性（通过名称）
 * @param node 节点指针
 * @param name 属性名称
 * @return 成功返回属性指针，失败返回 NULL
 */
vox_xml_attr_t* vox_xml_find_attr(const vox_xml_node_t* node, const char* name);

/**
 * 获取属性值（通过名称）
 * @param node 节点指针
 * @param name 属性名称
 * @return 成功返回属性值的字符串视图，失败返回 VOX_STRVIEW_NULL
 */
vox_strview_t vox_xml_get_attr_value(const vox_xml_node_t* node, const char* name);

/* ===== 遍历接口 ===== */

/**
 * 获取第一个子节点
 * @param node 节点指针
 * @return 返回第一个子节点指针，如果没有子节点返回 NULL
 */
vox_xml_node_t* vox_xml_first_child(const vox_xml_node_t* node);

/**
 * 获取下一个子节点
 * @param child 当前子节点指针
 * @return 返回下一个子节点指针，如果没有下一个子节点返回 NULL
 */
vox_xml_node_t* vox_xml_next_child(const vox_xml_node_t* child);

/**
 * 获取第一个属性
 * @param node 节点指针
 * @return 返回第一个属性指针，如果没有属性返回 NULL
 */
vox_xml_attr_t* vox_xml_first_attr(const vox_xml_node_t* node);

/**
 * 获取下一个属性
 * @param attr 当前属性指针
 * @return 返回下一个属性指针，如果没有下一个属性返回 NULL
 */
vox_xml_attr_t* vox_xml_next_attr(const vox_xml_attr_t* attr);

/* ===== 输出接口 ===== */

/**
 * 打印 XML 节点（序列化）
 * @param node 节点指针
 * @param buffer 输出缓冲区（必须可写）
 * @param size 缓冲区大小（不包括末尾的'\0'），输出后返回实际写入的字节数
 * @param prolog 是否包含 XML 声明（<?xml version="1.0"?>）
 * @return 成功返回写入的字节数，失败返回 -1（缓冲区不足）
 * 
 * 注意：
 * - 不会在缓冲区末尾添加 '\0'
 * - 如果缓冲区不足，返回 -1
 */
int vox_xml_print(const vox_xml_node_t* node, char* buffer, size_t* size, bool prolog);

/**
 * 将 XML 节点写入文件
 * @param mpool 内存池指针（必须非NULL）
 * @param node 根节点指针
 * @param filepath 文件路径
 * @param prolog 是否包含 XML 声明
 * @return 成功返回0，失败返回-1
 */
int vox_xml_write_file(vox_mpool_t* mpool, const vox_xml_node_t* node, const char* filepath, bool prolog);

/**
 * 打印 XML 节点（用于调试）
 * @param node 节点指针
 * @param indent 缩进级别（用于格式化输出）
 */
void vox_xml_print_debug(const vox_xml_node_t* node, int indent);

#ifdef __cplusplus
}
#endif

#endif /* VOX_XML_H */
