/*
 * vox_toml.h - 高性能 TOML 解析器
 * 使用 vox_scanner 实现零拷贝解析，使用 vox_mpool 进行内存管理
 * 支持 TOML v1.0.0 规范
 */

#ifndef VOX_TOML_H
#define VOX_TOML_H

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

/* ===== TOML 值类型 ===== */

/**
 * TOML 值类型枚举
 */
typedef enum {
    VOX_TOML_STRING = 0,      /* 字符串 */
    VOX_TOML_INTEGER,         /* 整数 */
    VOX_TOML_FLOAT,           /* 浮点数 */
    VOX_TOML_BOOLEAN,         /* 布尔值 */
    VOX_TOML_DATETIME,        /* 日期时间 */
    VOX_TOML_DATE,            /* 日期 */
    VOX_TOML_TIME,            /* 时间 */
    VOX_TOML_ARRAY,           /* 数组 */
    VOX_TOML_INLINE_TABLE,    /* 内联表 */
    VOX_TOML_TABLE,           /* 表 */
    VOX_TOML_ARRAY_OF_TABLES  /* 表数组 */
} vox_toml_type_t;

/* ===== TOML 值结构 ===== */

/**
 * TOML 数组元素
 */
typedef struct vox_toml_elem vox_toml_elem_t;

/**
 * TOML 键值对
 */
typedef struct vox_toml_keyvalue {
    vox_list_node_t node;        /* 链表节点（用于键值对链表） */
    vox_strview_t key;            /* 键名（字符串视图，零拷贝） */
    vox_toml_elem_t* value;      /* 值 */
    struct vox_toml_table* table; /* 所属表（用于遍历） */
} vox_toml_keyvalue_t;

/**
 * TOML 表
 */
typedef struct vox_toml_table {
    vox_list_node_t node;         /* 链表节点（用于表链表） */
    vox_strview_t name;            /* 表名（字符串视图，零拷贝） */
    vox_list_t keyvalues;          /* 键值对链表 */
    vox_list_t subtables;          /* 子表链表 */
    struct vox_toml_table* parent; /* 父表（用于遍历） */
    bool is_array_of_tables;       /* 是否为表数组 */
} vox_toml_table_t;

/**
 * TOML 数组
 */
typedef struct vox_toml_array {
    vox_list_t list;              /* 链表（管理数组元素） */
} vox_toml_array_t;

/**
 * TOML 内联表
 */
typedef struct vox_toml_inline_table {
    vox_list_t keyvalues;         /* 键值对链表 */
} vox_toml_inline_table_t;

/**
 * TOML 元素（值）
 */
struct vox_toml_elem {
    vox_list_node_t node;         /* 链表节点（用于数组元素链表） */
    vox_toml_type_t type;          /* 值类型 */
    union {
        vox_strview_t string;      /* 字符串（字符串视图，零拷贝） */
        int64_t integer;           /* 整数 */
        double float_val;           /* 浮点数 */
        bool boolean;               /* 布尔值 */
        vox_strview_t datetime;     /* 日期时间（字符串视图） */
        vox_strview_t date;         /* 日期（字符串视图） */
        vox_strview_t time;         /* 时间（字符串视图） */
        vox_toml_array_t array;     /* 数组 */
        vox_toml_inline_table_t inline_table; /* 内联表 */
        vox_toml_table_t* table;   /* 表指针 */
    } u;
    vox_toml_elem_t* parent;       /* 父元素（用于遍历） */
};

/* ===== 错误信息 ===== */

/**
 * TOML 解析错误信息
 */
typedef struct {
    int line;                     /* 错误行号（从1开始） */
    int column;                   /* 错误列号（从1开始） */
    size_t offset;                /* 错误位置偏移量 */
    const char* message;           /* 错误消息 */
} vox_toml_err_info_t;

/* ===== 解析接口 ===== */

/**
 * 解析 TOML 字符串
 * @param mpool 内存池指针（必须非NULL）
 * @param buffer TOML 字符串缓冲区（必须可写，且末尾必须有'\0'）
 * @param size 缓冲区大小（不包括末尾的'\0'），解析后返回实际使用的字节数
 * @param err_info 错误信息输出（可为NULL）
 * @return 成功返回根表指针，失败返回 NULL
 * 
 * 注意：
 * - buffer 必须保证在解析期间有效
 * - 解析后的 TOML 元素使用字符串视图指向原始 buffer，不复制数据
 * - 解析失败时，如果 err_info 非 NULL，会填充错误信息
 */
vox_toml_table_t* vox_toml_parse(vox_mpool_t* mpool, char* buffer, size_t* size,
                                  vox_toml_err_info_t* err_info);

/**
 * 解析 TOML 字符串（从 C 字符串）
 * @param mpool 内存池指针（必须非NULL）
 * @param toml_str TOML 字符串（必须以'\0'结尾）
 * @param err_info 错误信息输出（可为NULL）
 * @return 成功返回根表指针，失败返回 NULL
 */
vox_toml_table_t* vox_toml_parse_str(vox_mpool_t* mpool, const char* toml_str,
                                     vox_toml_err_info_t* err_info);

/**
 * 从文件解析 TOML
 * @param mpool 内存池指针（必须非NULL）
 * @param filepath 文件路径
 * @param err_info 错误信息输出（可为NULL）
 * @return 成功返回根表指针，失败返回 NULL
 */
vox_toml_table_t* vox_toml_parse_file(vox_mpool_t* mpool, const char* filepath,
                                      vox_toml_err_info_t* err_info);

/* ===== 类型检查接口 ===== */

/**
 * 获取 TOML 元素类型
 * @param elem TOML 元素指针
 * @return 返回元素类型
 */
vox_toml_type_t vox_toml_get_type(const vox_toml_elem_t* elem);

/**
 * 检查 TOML 元素是否为指定类型
 * @param elem TOML 元素指针
 * @param type 要检查的类型
 * @return 是指定类型返回 true，否则返回 false
 */
bool vox_toml_is_type(const vox_toml_elem_t* elem, vox_toml_type_t type);

/* ===== 值获取接口 ===== */

/**
 * 获取字符串值（字符串视图，零拷贝）
 * @param elem TOML 元素指针（必须是 VOX_TOML_STRING 类型）
 * @return 返回字符串视图，如果类型不匹配返回 VOX_STRVIEW_NULL
 */
vox_strview_t vox_toml_get_string(const vox_toml_elem_t* elem);

/**
 * 获取整数值
 * @param elem TOML 元素指针（必须是 VOX_TOML_INTEGER 类型）
 * @return 返回整数值，如果类型不匹配返回 0
 */
int64_t vox_toml_get_integer(const vox_toml_elem_t* elem);

/**
 * 获取浮点数值
 * @param elem TOML 元素指针（必须是 VOX_TOML_FLOAT 类型）
 * @return 返回浮点数值，如果类型不匹配返回 0.0
 */
double vox_toml_get_float(const vox_toml_elem_t* elem);

/**
 * 获取布尔值
 * @param elem TOML 元素指针（必须是 VOX_TOML_BOOLEAN 类型）
 * @return 返回布尔值，如果类型不匹配返回 false
 */
bool vox_toml_get_boolean(const vox_toml_elem_t* elem);

/**
 * 获取日期时间值（字符串视图）
 * @param elem TOML 元素指针（必须是 VOX_TOML_DATETIME 类型）
 * @return 返回日期时间字符串视图，如果类型不匹配返回 VOX_STRVIEW_NULL
 */
vox_strview_t vox_toml_get_datetime(const vox_toml_elem_t* elem);

/**
 * 获取日期值（字符串视图）
 * @param elem TOML 元素指针（必须是 VOX_TOML_DATE 类型）
 * @return 返回日期字符串视图，如果类型不匹配返回 VOX_STRVIEW_NULL
 */
vox_strview_t vox_toml_get_date(const vox_toml_elem_t* elem);

/**
 * 获取时间值（字符串视图）
 * @param elem TOML 元素指针（必须是 VOX_TOML_TIME 类型）
 * @return 返回时间字符串视图，如果类型不匹配返回 VOX_STRVIEW_NULL
 */
vox_strview_t vox_toml_get_time(const vox_toml_elem_t* elem);

/**
 * 获取数组元素数量
 * @param elem TOML 元素指针（必须是 VOX_TOML_ARRAY 类型）
 * @return 返回数组元素数量，如果类型不匹配返回 0
 */
size_t vox_toml_get_array_count(const vox_toml_elem_t* elem);

/**
 * 获取数组元素（通过索引）
 * @param elem TOML 元素指针（必须是 VOX_TOML_ARRAY 类型）
 * @param index 元素索引（从0开始）
 * @return 成功返回元素指针，失败返回 NULL
 */
vox_toml_elem_t* vox_toml_get_array_elem(const vox_toml_elem_t* elem, size_t index);

/**
 * 获取内联表的键值对数量
 * @param elem TOML 元素指针（必须是 VOX_TOML_INLINE_TABLE 类型）
 * @return 返回键值对数量，如果类型不匹配返回 0
 */
size_t vox_toml_get_inline_table_count(const vox_toml_elem_t* elem);

/**
 * 获取内联表中的值（通过键名）
 * @param elem TOML 元素指针（必须是 VOX_TOML_INLINE_TABLE 类型）
 * @param key 键名
 * @return 成功返回值元素指针，失败返回 NULL
 */
vox_toml_elem_t* vox_toml_get_inline_table_value(const vox_toml_elem_t* elem, const char* key);

/* ===== 表操作接口 ===== */

/**
 * 获取表中的键值对数量
 * @param table 表指针
 * @return 返回键值对数量，如果 table 为 NULL 返回 0
 */
size_t vox_toml_get_keyvalue_count(const vox_toml_table_t* table);

/**
 * 获取表中的子表数量
 * @param table 表指针
 * @return 返回子表数量，如果 table 为 NULL 返回 0
 */
size_t vox_toml_get_subtable_count(const vox_toml_table_t* table);

/**
 * 查找表中的键值对（通过键名）
 * @param table 表指针
 * @param key 键名
 * @return 成功返回键值对指针，失败返回 NULL
 */
vox_toml_keyvalue_t* vox_toml_find_keyvalue(const vox_toml_table_t* table, const char* key);

/**
 * 查找表中的值（通过键名）
 * @param table 表指针
 * @param key 键名
 * @return 成功返回值元素指针，失败返回 NULL
 */
vox_toml_elem_t* vox_toml_get_value(const vox_toml_table_t* table, const char* key);

/**
 * 查找子表（通过表名）
 * @param table 表指针
 * @param name 表名
 * @return 成功返回子表指针，失败返回 NULL
 */
vox_toml_table_t* vox_toml_find_subtable(const vox_toml_table_t* table, const char* name);

/**
 * 查找表（通过点分隔的路径，如 "database.server.host"）
 * @param root 根表指针
 * @param path 点分隔的路径
 * @return 成功返回表指针，失败返回 NULL
 */
vox_toml_table_t* vox_toml_find_table_by_path(const vox_toml_table_t* root, const char* path);

/* ===== 遍历接口 ===== */

/**
 * 获取数组的第一个元素
 * @param elem TOML 元素指针（必须是 VOX_TOML_ARRAY 类型）
 * @return 返回第一个元素指针，如果数组为空返回 NULL
 */
vox_toml_elem_t* vox_toml_array_first(const vox_toml_elem_t* elem);

/**
 * 获取数组的下一个元素
 * @param elem 当前元素指针
 * @return 返回下一个元素指针，如果没有下一个元素返回 NULL
 */
vox_toml_elem_t* vox_toml_array_next(const vox_toml_elem_t* elem);

/**
 * 获取表的第一个键值对
 * @param table 表指针
 * @return 返回第一个键值对指针，如果表为空返回 NULL
 */
vox_toml_keyvalue_t* vox_toml_table_first_keyvalue(const vox_toml_table_t* table);

/**
 * 获取表的下一个键值对
 * @param kv 当前键值对指针
 * @return 返回下一个键值对指针，如果没有下一个键值对返回 NULL
 */
vox_toml_keyvalue_t* vox_toml_table_next_keyvalue(const vox_toml_keyvalue_t* kv);

/**
 * 获取表的第一个子表
 * @param table 表指针
 * @return 返回第一个子表指针，如果表为空返回 NULL
 */
vox_toml_table_t* vox_toml_table_first_subtable(const vox_toml_table_t* table);

/**
 * 获取表的下一个子表
 * @param subtable 当前子表指针
 * @return 返回下一个子表指针，如果没有下一个子表返回 NULL
 */
vox_toml_table_t* vox_toml_table_next_subtable(const vox_toml_table_t* subtable);

/* ===== 调试和输出接口 ===== */

/**
 * 打印 TOML 元素（用于调试）
 * @param elem TOML 元素指针
 * @param indent 缩进级别（用于格式化输出）
 */
void vox_toml_print_elem(const vox_toml_elem_t* elem, int indent);

/**
 * 打印 TOML 表（用于调试）
 * @param table 表指针
 * @param indent 缩进级别（用于格式化输出）
 */
void vox_toml_print_table(const vox_toml_table_t* table, int indent);

/* ===== 序列化接口 ===== */

/**
 * 序列化 TOML 元素到字符串缓冲区
 * @param mpool 内存池指针（必须非NULL）
 * @param elem TOML 元素指针
 * @param indent 缩进级别（用于格式化输出）
 * @param output 输出字符串缓冲区指针（必须非NULL，函数会追加内容）
 * @return 成功返回0，失败返回-1
 */
int vox_toml_serialize_elem(vox_mpool_t* mpool, const vox_toml_elem_t* elem, int indent, char** output, size_t* output_size, size_t* output_capacity);

/**
 * 序列化 TOML 表到字符串缓冲区
 * @param mpool 内存池指针（必须非NULL）
 * @param table 表指针
 * @param indent 缩进级别（用于格式化输出）
 * @param output 输出字符串缓冲区指针（必须非NULL，函数会追加内容）
 * @return 成功返回0，失败返回-1
 */
int vox_toml_serialize_table(vox_mpool_t* mpool, const vox_toml_table_t* table, int indent, char** output, size_t* output_size, size_t* output_capacity);

/**
 * 序列化 TOML 表到字符串
 * @param mpool 内存池指针（必须非NULL）
 * @param root 根表指针
 * @param output_size 输出字符串大小（不包括末尾的'\0'）
 * @return 成功返回字符串指针（需要调用者释放），失败返回 NULL
 * 
 * 注意：返回的字符串使用 mpool 分配，会在 mpool 销毁时自动释放
 */
char* vox_toml_to_string(vox_mpool_t* mpool, const vox_toml_table_t* root, size_t* output_size);

/**
 * 将 TOML 表写入文件
 * @param mpool 内存池指针（必须非NULL）
 * @param root 根表指针
 * @param filepath 文件路径
 * @return 成功返回0，失败返回-1
 */
int vox_toml_write_file(vox_mpool_t* mpool, const vox_toml_table_t* root, const char* filepath);

#ifdef __cplusplus
}
#endif

#endif /* VOX_TOML_H */
