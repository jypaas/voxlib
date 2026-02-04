/*
 * vox_json.h - 高性能 JSON 解析器
 * 使用 vox_scanner 实现零拷贝解析，使用 vox_mpool 进行内存管理
 */

#ifndef VOX_JSON_H
#define VOX_JSON_H

#include "vox_os.h"
#include "vox_scanner.h"
#include "vox_mpool.h"
#include "vox_list.h"
#include "vox_string.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== JSON 值类型 ===== */

/**
 * JSON 值类型枚举
 */
typedef enum {
    VOX_JSON_NULL = 0,      /* null */
    VOX_JSON_BOOLEAN,       /* true/false */
    VOX_JSON_NUMBER,        /* 数字 */
    VOX_JSON_STRING,        /* 字符串 */
    VOX_JSON_ARRAY,         /* 数组 */
    VOX_JSON_OBJECT         /* 对象 */
} vox_json_type_t;

/* ===== JSON 值结构 ===== */

/**
 * JSON 数组元素
 */
typedef struct vox_json_elem vox_json_elem_t;

/**
 * JSON 对象成员
 */
typedef struct vox_json_member {
    vox_list_node_t node;        /* 链表节点（用于对象成员链表） */
    vox_strview_t name;          /* 键名（字符串视图，零拷贝） */
    vox_json_elem_t* value;      /* 值 */
} vox_json_member_t;

/**
 * JSON 数组
 */
typedef struct vox_json_array {
    vox_list_t list;             /* 链表（管理数组元素） */
} vox_json_array_t;

/**
 * JSON 对象
 */
typedef struct vox_json_object {
    vox_list_t list;              /* 链表（管理对象成员） */
} vox_json_object_t;

/**
 * JSON 元素（值）
 */
struct vox_json_elem {
    vox_list_node_t node;        /* 链表节点（用于数组元素链表） */
    vox_json_type_t type;         /* 值类型 */
    union {
        bool boolean;             /* 布尔值 */
        double number;            /* 数字 */
        vox_strview_t string;     /* 字符串（字符串视图，零拷贝） */
        vox_json_array_t array;   /* 数组 */
        vox_json_object_t object; /* 对象 */
    } u;
    vox_json_elem_t* parent;      /* 父元素（用于遍历） */
};

/* ===== 错误信息 ===== */

/**
 * JSON 解析错误信息
 */
typedef struct {
    int line;                     /* 错误行号（从1开始） */
    int column;                   /* 错误列号（从1开始） */
    size_t offset;                /* 错误位置偏移量 */
    const char* message;           /* 错误消息 */
} vox_json_err_info_t;

/* ===== 解析接口 ===== */

/**
 * 解析 JSON 字符串
 * @param mpool 内存池指针（必须非NULL）
 * @param buffer JSON 字符串缓冲区（必须可写，且末尾必须有'\0'）
 * @param size 缓冲区大小（不包括末尾的'\0'），解析后返回实际使用的字节数
 * @param err_info 错误信息输出（可为NULL）
 * @return 成功返回 JSON 元素指针，失败返回 NULL
 * 
 * 注意：
 * - buffer 必须保证在解析期间有效
 * - 解析后的 JSON 元素使用字符串视图指向原始 buffer，不复制数据
 * - 解析失败时，如果 err_info 非 NULL，会填充错误信息
 */
vox_json_elem_t* vox_json_parse(vox_mpool_t* mpool, char* buffer, size_t* size, 
                                vox_json_err_info_t* err_info);

/**
 * 解析 JSON 字符串（从 C 字符串）
 * @param mpool 内存池指针（必须非NULL）
 * @param json_str JSON 字符串（必须以'\0'结尾）
 * @param err_info 错误信息输出（可为NULL）
 * @return 成功返回 JSON 元素指针，失败返回 NULL
 */
vox_json_elem_t* vox_json_parse_str(vox_mpool_t* mpool, const char* json_str,
                                    vox_json_err_info_t* err_info);

/**
 * 从文件解析 JSON
 * @param mpool 内存池指针（必须非NULL）
 * @param filepath 文件路径
 * @param err_info 错误信息输出（可为NULL）
 * @return 成功返回 JSON 元素指针，失败返回 NULL
 */
vox_json_elem_t* vox_json_parse_file(vox_mpool_t* mpool, const char* filepath,
                                     vox_json_err_info_t* err_info);

/* ===== 类型检查接口 ===== */

/**
 * 获取 JSON 元素类型
 * @param elem JSON 元素指针
 * @return 返回元素类型，如果 elem 为 NULL 返回 VOX_JSON_NULL
 */
vox_json_type_t vox_json_get_type(const vox_json_elem_t* elem);

/**
 * 检查 JSON 元素是否为指定类型
 * @param elem JSON 元素指针
 * @param type 要检查的类型
 * @return 是指定类型返回 true，否则返回 false
 */
bool vox_json_is_type(const vox_json_elem_t* elem, vox_json_type_t type);

/* ===== 值获取接口 ===== */

/**
 * 获取布尔值
 * @param elem JSON 元素指针（必须是 VOX_JSON_BOOLEAN 类型）
 * @return 返回布尔值，如果类型不匹配返回 false
 */
bool vox_json_get_bool(const vox_json_elem_t* elem);

/**
 * 获取数字值
 * @param elem JSON 元素指针（必须是 VOX_JSON_NUMBER 类型）
 * @return 返回数字值，如果类型不匹配返回 0.0
 */
double vox_json_get_number(const vox_json_elem_t* elem);

/**
 * 判断数字是否为可安全转换为 int64_t 的整数值（有限、在 [INT64_MIN,INT64_MAX] 内且无小数部分）
 * @param elem JSON 元素指针（必须是 VOX_JSON_NUMBER 类型）
 * @return 是则返回 true，否则返回 false
 */
bool vox_json_number_is_integer(const vox_json_elem_t* elem);

/**
 * 获取整数值（从数字类型）
 * 类型非 NUMBER、非有限、或超出 int64_t 范围时返回 0；小数会截断（建议先用 vox_json_number_is_integer 校验）
 * @param elem JSON 元素指针（必须是 VOX_JSON_NUMBER 类型）
 * @return 返回整数值，若类型不匹配或越界返回 0
 */
int64_t vox_json_get_int(const vox_json_elem_t* elem);

/**
 * 获取字符串值（字符串视图，零拷贝）
 * @param elem JSON 元素指针（必须是 VOX_JSON_STRING 类型）
 * @return 返回字符串视图，如果类型不匹配返回 VOX_STRVIEW_NULL
 */
vox_strview_t vox_json_get_string(const vox_json_elem_t* elem);

/**
 * 获取数组元素数量
 * @param elem JSON 元素指针（必须是 VOX_JSON_ARRAY 类型）
 * @return 返回数组元素数量，如果类型不匹配返回 0
 */
size_t vox_json_get_array_count(const vox_json_elem_t* elem);

/**
 * 获取数组元素（通过索引）
 * @param elem JSON 元素指针（必须是 VOX_JSON_ARRAY 类型）
 * @param index 元素索引（从0开始）
 * @return 成功返回元素指针，失败返回 NULL
 */
vox_json_elem_t* vox_json_get_array_elem(const vox_json_elem_t* elem, size_t index);

/**
 * 获取对象成员数量
 * @param elem JSON 元素指针（必须是 VOX_JSON_OBJECT 类型）
 * @return 返回对象成员数量，如果类型不匹配返回 0
 */
size_t vox_json_get_object_count(const vox_json_elem_t* elem);

/**
 * 获取对象成员（通过键名）
 * @param elem JSON 元素指针（必须是 VOX_JSON_OBJECT 类型）
 * @param name 成员键名
 * @return 成功返回成员指针，失败返回 NULL
 */
vox_json_member_t* vox_json_get_object_member(const vox_json_elem_t* elem, 
                                               const char* name);

/**
 * 获取对象成员的值（通过键名）
 * @param elem JSON 元素指针（必须是 VOX_JSON_OBJECT 类型）
 * @param name 成员键名
 * @return 成功返回值元素指针，失败返回 NULL
 */
vox_json_elem_t* vox_json_get_object_value(const vox_json_elem_t* elem, 
                                           const char* name);

/* ===== 遍历接口 ===== */

/**
 * 获取数组的第一个元素
 * @param elem JSON 元素指针（必须是 VOX_JSON_ARRAY 类型）
 * @return 返回第一个元素指针，如果数组为空返回 NULL
 */
vox_json_elem_t* vox_json_array_first(const vox_json_elem_t* elem);

/**
 * 获取数组的下一个元素
 * @param elem 当前元素指针
 * @return 返回下一个元素指针，如果没有下一个元素返回 NULL
 */
vox_json_elem_t* vox_json_array_next(const vox_json_elem_t* elem);

/**
 * 获取对象的第一个成员
 * @param elem JSON 元素指针（必须是 VOX_JSON_OBJECT 类型）
 * @return 返回第一个成员指针，如果对象为空返回 NULL
 */
vox_json_member_t* vox_json_object_first(const vox_json_elem_t* elem);

/**
 * 获取对象的下一个成员
 * @param member 当前成员指针
 * @return 返回下一个成员指针，如果没有下一个成员返回 NULL
 */
vox_json_member_t* vox_json_object_next(const vox_json_member_t* member);

/* ===== 调试和输出接口 ===== */

/**
 * 打印 JSON 元素（用于调试）
 * @param elem JSON 元素指针
 * @param indent 缩进级别（用于格式化输出）
 */
void vox_json_print(const vox_json_elem_t* elem, int indent);

/* ===== 序列化接口 ===== */

/**
 * 将 JSON 元素序列化为 vox_string_t（推荐）
 * 内存由 mpool 管理，随 mpool 释放；可用 vox_string_cstr() 取 C 字符串。
 * @param mpool 内存池，不可为 NULL
 * @param elem JSON 元素指针（可为 NULL，输出 "null"）
 * @param pretty 是否格式化输出（换行与缩进）
 * @return 成功返回字符串对象指针，失败返回 NULL，可用 vox_string_destroy() 释放
 */
vox_string_t* vox_json_to_string(vox_mpool_t* mpool, const vox_json_elem_t* elem, bool pretty);

/**
 * 将 JSON 元素序列化到固定缓冲区
 * @param elem JSON 元素指针（可为 NULL，输出 "null"）
 * @param buffer 输出缓冲区（可为 NULL，仅计算长度）
 * @param size 缓冲区大小（含结尾 '\0'）
 * @param written 输出实际长度（不含 '\0'），不可为 NULL
 * @param pretty 是否格式化输出（换行与缩进）
 * @return 成功返回 0；buffer 非 NULL 且空间不足返回 -1（此时 written 为所需长度）
 */
int vox_json_serialize(const vox_json_elem_t* elem, char* buffer, size_t size,
                       size_t* written, bool pretty);

/* ===== 构建接口 ===== */

/**
 * 创建 JSON null 值
 */
vox_json_elem_t* vox_json_new_null(vox_mpool_t* mpool);

/**
 * 创建 JSON 布尔值
 */
vox_json_elem_t* vox_json_new_bool(vox_mpool_t* mpool, bool value);

/**
 * 创建 JSON 数字
 */
vox_json_elem_t* vox_json_new_number(vox_mpool_t* mpool, double value);

/**
 * 创建 JSON 字符串（复制 str 的 len 字节到 mpool）
 * @param str 字符串指针
 * @param len 长度（字节数）
 */
vox_json_elem_t* vox_json_new_string(vox_mpool_t* mpool, const char* str, size_t len);

/**
 * 创建 JSON 字符串（从 C 字符串复制，以 '\0' 结尾）
 */
vox_json_elem_t* vox_json_new_string_cstr(vox_mpool_t* mpool, const char* cstr);

/**
 * 创建空 JSON 数组
 */
vox_json_elem_t* vox_json_new_array(vox_mpool_t* mpool);

/**
 * 创建空 JSON 对象
 */
vox_json_elem_t* vox_json_new_object(vox_mpool_t* mpool);

/**
 * 向数组末尾追加元素
 * @param array_elem 必须是 VOX_JSON_ARRAY 类型
 * @param value_elem 要追加的元素（其 parent 会被设为 array_elem）
 * @return 成功返回 0，失败返回 -1
 */
int vox_json_array_append(vox_json_elem_t* array_elem, vox_json_elem_t* value_elem);

/**
 * 设置对象成员（若键已存在则替换）
 * @param mpool 用于分配键名副本和 member 结构
 * @param object_elem 必须是 VOX_JSON_OBJECT 类型
 * @param name 成员键名（会被复制到 mpool）
 * @param value_elem 成员值（其 parent 会被设为 object_elem）
 * @return 成功返回 0，失败返回 -1
 */
int vox_json_object_set(vox_mpool_t* mpool, vox_json_elem_t* object_elem,
                        const char* name, vox_json_elem_t* value_elem);

/**
 * 移除对象中指定键的成员（不释放 value_elem，仅解除链接）
 * @param mpool 用于释放 member 结构及键名副本
 * @param object_elem 必须是 VOX_JSON_OBJECT 类型
 * @param name 成员键名
 * @return 找到并移除返回 0，未找到返回 -1
 */
int vox_json_object_remove(vox_mpool_t* mpool, vox_json_elem_t* object_elem,
                           const char* name);

#ifdef __cplusplus
}
#endif

#endif /* VOX_JSON_H */
