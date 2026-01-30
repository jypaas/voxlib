/*
 * vox_scanner.h - 零拷贝字符串扫描器
 * 通过指针移动实现零拷贝解析
 */

#ifndef VOX_SCANNER_H
#define VOX_SCANNER_H

#include "vox_os.h"
#include "vox_string.h"  /* 使用 vox_strview_t */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 字符集规范 ===== */

/**
 * 字符集规范结构（用于匹配字符类）
 * 使用位图实现，每个字符对应一个位
 */
typedef struct vox_charset {
    uint8_t bitmap[32];  /* 256个字符，每个字符1位 (256/8=32) */
} vox_charset_t;

/**
 * 初始化字符集规范
 * @param cs 字符集指针（由外部分配）
 */
void vox_charset_init(vox_charset_t* cs);

/**
 * 添加单个字符到字符集
 * @param cs 字符集指针
 * @param ch 字符
 * @return 成功返回0，失败返回-1
 */
int vox_charset_add_char(vox_charset_t* cs, char ch);

/**
 * 添加字符范围到字符集
 * @param cs 字符集指针
 * @param start 起始字符
 * @param end 结束字符（包含）
 * @return 成功返回0，失败返回-1
 */
int vox_charset_add_range(vox_charset_t* cs, char start, char end);

/**
 * 添加所有字母字符（a-z, A-Z）
 * @param cs 字符集指针
 * @return 成功返回0，失败返回-1
 */
int vox_charset_add_alpha(vox_charset_t* cs);

/**
 * 添加所有数字字符（0-9）
 * @param cs 字符集指针
 * @return 成功返回0，失败返回-1
 */
int vox_charset_add_digit(vox_charset_t* cs);

/**
 * 添加所有字母数字字符（a-z, A-Z, 0-9）
 * @param cs 字符集指针
 * @return 成功返回0，失败返回-1
 */
int vox_charset_add_alnum(vox_charset_t* cs);

/**
 * 添加空白字符（空格、制表符、换行符等）
 * @param cs 字符集指针
 * @return 成功返回0，失败返回-1
 */
int vox_charset_add_space(vox_charset_t* cs);

/**
 * 检查字符是否在字符集中
 * @param cs 字符集指针
 * @param ch 字符
 * @return 在字符集中返回true，否则返回false
 */
bool vox_charset_contains(const vox_charset_t* cs, char ch);

/* ===== 扫描器选项 ===== */

/**
 * 扫描器选项标志
 */
typedef enum {
    VOX_SCANNER_NONE = 0,                    /* 无选项 */
    VOX_SCANNER_AUTOSKIP_WS = 1 << 0,        /* 自动跳过空白字符 */
    VOX_SCANNER_AUTOSKIP_NEWLINE = 1 << 1,   /* 自动跳过换行符 */
    VOX_SCANNER_CASE_SENSITIVE = 1 << 2      /* 大小写敏感（默认敏感）,暂时无用 */
} vox_scanner_flag_t;

/* 常用组合标志 */
#define VOX_SCANNER_AUTOSKIP_WS_NL  (VOX_SCANNER_AUTOSKIP_WS | VOX_SCANNER_AUTOSKIP_NEWLINE)  /* 自动跳过空白字符和换行符 */

/* ===== 扫描器 ===== */

/**
 * 扫描器结构体
 * 用户可以直接在栈上声明此结构体
 */
typedef struct vox_scanner {
    char* begin;        /* 缓冲区起始位置（内部使用） */
    char* end;          /* 缓冲区结束位置（内部使用） */
    char* curptr;       /* 当前扫描位置（内部使用） */
    int flags;          /* 扫描器选项标志（内部使用） */
} vox_scanner_t;

/**
 * 初始化扫描器
 * @param scanner 扫描器指针（必须已分配内存）
 * @param buf 输入缓冲区指针（必须可写，且缓冲区末尾必须有'\0'）
 * @param len 缓冲区长度（不包括末尾的'\0'）
 * @param flags 扫描器选项标志
 * @return 成功返回0，失败返回-1
 * 
 * 注意：缓冲区必须保证在扫描器使用期间有效，且缓冲区末尾必须有'\0'
 */
int vox_scanner_init(vox_scanner_t* scanner, char* buf, size_t len, int flags);

/**
 * 销毁扫描器（不释放缓冲区）
 * @param scanner 扫描器指针
 */
void vox_scanner_destroy(vox_scanner_t* scanner);

/**
 * 获取当前扫描位置
 * @param scanner 扫描器指针
 * @return 返回当前位置指针
 */
const char* vox_scanner_curptr(const vox_scanner_t* scanner);

/**
 * 获取当前位置相对于起始位置的偏移量
 * @param scanner 扫描器指针
 * @return 返回偏移量（字节数）
 */
size_t vox_scanner_offset(const vox_scanner_t* scanner);

/**
 * 获取剩余长度
 * @param scanner 扫描器指针
 * @return 返回剩余可扫描的字节数
 */
size_t vox_scanner_remaining(const vox_scanner_t* scanner);

/**
 * 检查是否已到达末尾
 * @param scanner 扫描器指针
 * @return 已到达末尾返回true，否则返回false
 */
bool vox_scanner_eof(const vox_scanner_t* scanner);

/**
 * 查看当前字符（不移动指针）
 * @param scanner 扫描器指针
 * @return 成功返回字符，失败或EOF返回-1
 */
int vox_scanner_peek_char(const vox_scanner_t* scanner);

/**
 * 查看指定位置的字符（不移动指针）
 * @param scanner 扫描器指针
 * @param offset 偏移量（从当前位置开始）
 * @return 成功返回字符，失败或EOF返回-1
 */
int vox_scanner_peek_char_at(const vox_scanner_t* scanner, size_t offset);

/**
 * 查看指定长度的字符串（不移动指针）
 * @param scanner 扫描器指针
 * @param len 要查看的长度
 * @param out 输出的字符串视图
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_peek(const vox_scanner_t* scanner, size_t len, vox_strview_t* out);

/**
 * 查看直到匹配字符集的字符串（不移动指针）
 * @param scanner 扫描器指针
 * @param charset 字符集指针（NULL表示匹配任何字符）
 * @param include_match 是否包含匹配的字符
 * @param out 输出的字符串视图
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_peek_until_charset(const vox_scanner_t* scanner, 
                                   const vox_charset_t* charset,
                                   bool include_match,
                                   vox_strview_t* out);

/**
 * 查看直到匹配字符的字符串（不移动指针）
 * @param scanner 扫描器指针
 * @param ch 匹配字符
 * @param include_match 是否包含匹配的字符
 * @param out 输出的字符串视图
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_peek_until_char(const vox_scanner_t* scanner, 
                                char ch,
                                bool include_match,
                                vox_strview_t* out);

/**
 * 查看直到匹配字符串的字符串（不移动指针）
 * @param scanner 扫描器指针
 * @param str 匹配字符串
 * @param include_match 是否包含匹配的字符串
 * @param out 输出的字符串视图
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_peek_until_str(const vox_scanner_t* scanner,
                               const char* str,
                               bool include_match,
                               vox_strview_t* out);

/**
 * 获取当前字符并移动指针
 * @param scanner 扫描器指针
 * @return 成功返回字符，失败或EOF返回-1
 */
int vox_scanner_get_char(vox_scanner_t* scanner);

/**
 * 获取指定长度的字符串并移动指针
 * @param scanner 扫描器指针
 * @param len 要获取的长度
 * @param out 输出的字符串视图
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_get(vox_scanner_t* scanner, size_t len, vox_strview_t* out);

/**
 * 获取直到匹配字符集的字符串并移动指针
 * @param scanner 扫描器指针
 * @param charset 字符集指针（NULL表示匹配任何字符）
 * @param include_match 是否包含匹配的字符
 * @param out 输出的字符串视图
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_get_until_charset(vox_scanner_t* scanner,
                                  const vox_charset_t* charset,
                                  bool include_match,
                                  vox_strview_t* out);

/**
 * 获取直到匹配字符的字符串并移动指针
 * @param scanner 扫描器指针
 * @param ch 匹配字符
 * @param include_match 是否包含匹配的字符
 * @param out 输出的字符串视图
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_get_until_char(vox_scanner_t* scanner,
                               char ch,
                               bool include_match,
                               vox_strview_t* out);

/**
 * 获取直到匹配字符串的字符串并移动指针
 * @param scanner 扫描器指针
 * @param str 匹配字符串
 * @param include_match 是否包含匹配的字符串
 * @param out 输出的字符串视图
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_get_until_str(vox_scanner_t* scanner,
                              const char* str,
                              bool include_match,
                              vox_strview_t* out);

/**
 * 获取字符集中的字符序列（获取所有在字符集中的连续字符）
 * @param scanner 扫描器指针
 * @param charset 字符集指针
 * @param out 输出的字符串视图
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_get_charset(vox_scanner_t* scanner,
                            const vox_charset_t* charset,
                            vox_strview_t* out);

/**
 * 跳过指定数量的字符
 * @param scanner 扫描器指针
 * @param count 要跳过的字符数
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_skip(vox_scanner_t* scanner, size_t count);

/**
 * 跳过匹配字符集的字符
 * @param scanner 扫描器指针
 * @param charset 字符集指针
 * @return 返回跳过的字符数
 */
size_t vox_scanner_skip_charset(vox_scanner_t* scanner, const vox_charset_t* charset);

/**
 * 跳过空白字符
 * @param scanner 扫描器指针
 * @return 返回跳过的字符数
 */
size_t vox_scanner_skip_ws(vox_scanner_t* scanner);

/**
 * 跳过换行符
 * @param scanner 扫描器指针
 * @return 返回跳过的字符数
 */
size_t vox_scanner_skip_newline(vox_scanner_t* scanner);

/**
 * 扫描器状态结构（用于保存和恢复扫描器状态）
 */
typedef struct vox_scanner_state {
    const char* curptr;  /* 保存的当前位置 */
} vox_scanner_state_t;

/**
 * 保存扫描器状态（用于回溯）
 * @param scanner 扫描器指针
 * @param state 状态结构体指针（由外部分配）
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_save_state(const vox_scanner_t* scanner, vox_scanner_state_t* state);

/**
 * 恢复扫描器状态
 * @param scanner 扫描器指针
 * @param state 状态结构体指针（由 vox_scanner_save_state 保存）
 * @return 成功返回0，失败返回-1
 */
int vox_scanner_restore_state(vox_scanner_t* scanner, const vox_scanner_state_t* state);

/* ===== 流式扫描器 ===== */

/**
 * 缓冲区片段（用于零拷贝模式）
 */
typedef struct vox_scanner_chunk {
    char* data;                  /* 数据指针（外部拥有，不复制，内部可能修改） */
    size_t len;                  /* 数据长度 */
    struct vox_scanner_chunk* next;  /* 下一个片段 */
} vox_scanner_chunk_t;

/**
 * 流式扫描器结构体
 * 支持零拷贝模式：直接使用外部缓冲区，不复制数据
 */
typedef struct vox_scanner_stream {
    vox_scanner_t scanner;       /* 基础扫描器（指向当前可扫描的数据） */
    vox_scanner_chunk_t* chunks; /* 缓冲区片段链表（零拷贝模式） */
    vox_scanner_chunk_t* chunks_tail; /* 片段链表尾指针 */
    char* temp_buffer;           /* 临时缓冲区（用于跨边界匹配，仅在需要时分配） */
    size_t temp_buffer_size;     /* 临时缓冲区大小 */
    size_t temp_buffer_capacity; /* 临时缓冲区容量 */
    size_t total_size;           /* 总数据大小 */
    void* mpool;                 /* 内存池（必须提供，不能为NULL） */
    int flags;                   /* 扫描器选项标志 */
} vox_scanner_stream_t;

/**
 * 初始化流式扫描器（零拷贝模式）
 * @param stream 流式扫描器指针（必须已分配内存）
 * @param mpool 内存池指针（必须提供，不能为NULL）
 * @param flags 扫描器选项标志
 * @return 成功返回0，失败返回-1
 * 
 * 注意：此流式扫描器使用零拷贝设计，直接使用外部提供的缓冲区，不复制数据。
 * 调用者必须保证在扫描器使用期间，所有通过feed添加的缓冲区保持有效。
 */
int vox_scanner_stream_init(vox_scanner_stream_t* stream, 
                            void* mpool,
                            int flags);

/**
 * 销毁流式扫描器
 * @param stream 流式扫描器指针
 */
void vox_scanner_stream_destroy(vox_scanner_stream_t* stream);

/**
 * 追加新数据到流式扫描器（零拷贝，不复制数据）
 * @param stream 流式扫描器指针
 * @param data 要追加的数据指针（必须保持有效直到被consume）
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 * 
 * 注意：此函数不复制数据，直接使用外部缓冲区。
 * 调用者必须保证data指针在扫描器使用期间保持有效。
 */
int vox_scanner_stream_feed(vox_scanner_stream_t* stream, 
                            const void* data, 
                            size_t len);

/**
 * 消费已处理的数据，释放已处理的片段
 * @param stream 流式扫描器指针
 * @param bytes 要消费的字节数（必须小于等于已扫描的字节数）
 * @return 成功返回0，失败返回-1
 * 
 * 注意：消费后，相应的缓冲区片段会被移除，调用者可以安全释放这些缓冲区。
 */
int vox_scanner_stream_consume(vox_scanner_stream_t* stream, size_t bytes);

/**
 * 获取基础扫描器指针（用于调用标准扫描器API）
 * @param stream 流式扫描器指针
 * @return 返回基础扫描器指针
 */
vox_scanner_t* vox_scanner_stream_get_scanner(vox_scanner_stream_t* stream);

/**
 * 获取当前可扫描的数据大小
 * @param stream 流式扫描器指针
 * @return 返回数据大小（字节）
 */
size_t vox_scanner_stream_get_size(const vox_scanner_stream_t* stream);

/**
 * 更新扫描器视图（在feed后调用，用于更新扫描器指向的数据范围）
 * @param stream 流式扫描器指针
 * @return 成功返回0，失败返回-1
 * 
 * 注意：在feed新数据后，需要调用此函数来更新扫描器的数据视图。
 * 如果数据跨越多个片段，此函数会创建临时缓冲区来合并数据（仅在需要时）。
 */
int vox_scanner_stream_update_view(vox_scanner_stream_t* stream);

/**
 * 清空流式扫描器（重置所有状态，但保留缓冲区）
 * @param stream 流式扫描器指针
 */
void vox_scanner_stream_reset(vox_scanner_stream_t* stream);

/**
 * 检查流式扫描器是否可能匹配到指定字符串（用于处理跨缓冲区边界的情况）
 * @param stream 流式扫描器指针
 * @param str 要匹配的字符串
 * @param partial_match_len 输出参数，返回部分匹配的长度（如果返回true）
 * @return 如果可能匹配（包括部分匹配）返回true，否则返回false
 * 
 * 此函数用于检查当前数据末尾是否与目标字符串的开头部分匹配。
 * 如果返回true且partial_match_len > 0，说明需要继续feed数据才能完成匹配。
 * 
 * 注意：此函数会检查所有片段，支持跨片段匹配。
 */
bool vox_scanner_stream_check_partial_match(vox_scanner_stream_t* stream,
                                            const char* str,
                                            size_t* partial_match_len);

#ifdef __cplusplus
}
#endif

#endif /* VOX_SCANNER_H */
