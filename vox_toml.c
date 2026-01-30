/*
 * vox_toml.c - 高性能 TOML 解析器实现
 * 使用 vox_scanner 实现零拷贝解析，使用 vox_mpool 进行内存管理
 * 支持 TOML v1.0.0 规范
 */

#include "vox_toml.h"
#include "vox_os.h"
#include "vox_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#ifdef VOX_OS_WINDOWS
    #include <malloc.h>
#else
    #include <alloca.h>
#endif

/* ===== 内部辅助函数 ===== */

/* 更新错误信息 */
static void set_error(vox_toml_err_info_t* err_info, vox_scanner_t* scanner,
                      const char* message) {
    if (!err_info) return;
    
    err_info->message = message;
    err_info->offset = vox_scanner_offset(scanner);
    
    /* 计算行号和列号 */
    int line = 1;
    int column = 1;
    const char* ptr = scanner->begin;
    const char* cur = scanner->curptr;
    
    while (ptr < cur) {
        if (*ptr == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
        ptr++;
    }
    
    err_info->line = line;
    err_info->column = column;
}

/* 跳过空白字符和注释 */
static void skip_whitespace_and_comments(vox_scanner_t* scanner,
                                          vox_toml_err_info_t* err_info) {
    (void)err_info;  /* 未使用的参数 */
    while (!vox_scanner_eof(scanner)) {
        vox_scanner_skip_ws(scanner);
        
        int ch = vox_scanner_peek_char(scanner);
        if (ch < 0) break;
        
        if (ch == '#') {
            /* 跳过注释到行尾 */
            while (!vox_scanner_eof(scanner)) {
                ch = vox_scanner_get_char(scanner);
                if (ch == '\n' || ch == '\r') {
                    break;
                }
            }
        } else if (ch == '\n' || ch == '\r') {
            vox_scanner_get_char(scanner);
            if (ch == '\r' && vox_scanner_peek_char(scanner) == '\n') {
                vox_scanner_get_char(scanner);
            }
        } else {
            break;
        }
    }
}

/* 解析 bare key */
static int parse_bare_key(vox_scanner_t* scanner, vox_strview_t* key,
                           vox_toml_err_info_t* err_info) {
    const char* start = vox_scanner_curptr(scanner);
    
    int ch = vox_scanner_peek_char(scanner);
    if (ch < 0) {
        set_error(err_info, scanner, "Unexpected end of input while parsing key");
        return -1;
    }
    
    /* Bare key: A-Za-z0-9_- */
    if (!isalnum(ch) && ch != '_' && ch != '-') {
        set_error(err_info, scanner, "Invalid bare key start character");
        return -1;
    }
    
    vox_scanner_get_char(scanner);
    
    while (!vox_scanner_eof(scanner)) {
        ch = vox_scanner_peek_char(scanner);
        if (ch < 0) break;
        
        if (isalnum(ch) || ch == '_' || ch == '-') {
            vox_scanner_get_char(scanner);
        } else {
            break;
        }
    }
    
    const char* end = vox_scanner_curptr(scanner);
    key->ptr = start;
    key->len = end - start;
    
    return 0;
}

/* 解析基本字符串（支持转义） */
static int parse_basic_string(vox_scanner_t* scanner, vox_strview_t* str,
                               vox_toml_err_info_t* err_info) {
    if (vox_scanner_peek_char(scanner) != '"') {
        set_error(err_info, scanner, "Expected '\"' to start basic string");
        return -1;
    }
    vox_scanner_get_char(scanner);  /* 跳过开始引号 */
    
    const char* start = vox_scanner_curptr(scanner);
    const char* ptr = start;
    const char* end = scanner->end;
    
    while (ptr < end) {
        if (*ptr == '"') {
            break;
        }
        if (*ptr == '\\') {
            ptr++;  /* 跳过反斜杠 */
            if (ptr >= end) {
                set_error(err_info, scanner, "Unterminated escape sequence");
                return -1;
            }
            /* 处理转义字符 */
            if (*ptr == 'u' || *ptr == 'U') {
                /* Unicode 转义：跳过4或8个十六进制字符 */
                int hex_count = (*ptr == 'u') ? 4 : 8;
                ptr++;
                for (int i = 0; i < hex_count; i++) {
                    if (ptr >= end || !isxdigit(*ptr)) {
                        set_error(err_info, scanner, "Invalid Unicode escape sequence");
                        return -1;
                    }
                    ptr++;
                }
            } else {
                /* 其他转义字符 */
                ptr++;
            }
        } else {
            ptr++;
        }
    }
    
    if (ptr >= end) {
        set_error(err_info, scanner, "Unterminated basic string");
        return -1;
    }
    
    size_t len = ptr - start;
    vox_scanner_skip(scanner, len);
    
    if (vox_scanner_peek_char(scanner) != '"') {
        set_error(err_info, scanner, "Expected '\"' to end basic string");
        return -1;
    }
    vox_scanner_get_char(scanner);  /* 跳过结束引号 */
    
    str->ptr = start;
    str->len = len;
    
    return 0;
}

/* 解析字面字符串（不支持转义） */
static int parse_literal_string(vox_scanner_t* scanner, vox_strview_t* str,
                                vox_toml_err_info_t* err_info) {
    if (vox_scanner_peek_char(scanner) != '\'') {
        set_error(err_info, scanner, "Expected ''' to start literal string");
        return -1;
    }
    vox_scanner_get_char(scanner);  /* 跳过开始引号 */
    
    const char* start = vox_scanner_curptr(scanner);
    const char* ptr = start;
    const char* end = scanner->end;
    
    while (ptr < end && *ptr != '\'') {
        ptr++;
    }
    
    if (ptr >= end) {
        set_error(err_info, scanner, "Unterminated literal string");
        return -1;
    }
    
    size_t len = ptr - start;
    vox_scanner_skip(scanner, len);
    
    if (vox_scanner_peek_char(scanner) != '\'') {
        set_error(err_info, scanner, "Expected ''' to end literal string");
        return -1;
    }
    vox_scanner_get_char(scanner);  /* 跳过结束引号 */
    
    str->ptr = start;
    str->len = len;
    
    return 0;
}

/* 解析键名（bare key 或 quoted key） */
static int parse_key(vox_scanner_t* scanner, vox_strview_t* key,
                     vox_toml_err_info_t* err_info) {
    int ch = vox_scanner_peek_char(scanner);
    if (ch < 0) {
        set_error(err_info, scanner, "Unexpected end of input while parsing key");
        return -1;
    }
    
    if (ch == '"') {
        return parse_basic_string(scanner, key, err_info);
    } else if (ch == '\'') {
        return parse_literal_string(scanner, key, err_info);
    } else {
        return parse_bare_key(scanner, key, err_info);
    }
}

/* 解析整数 */
static int parse_integer(vox_mpool_t* mpool, vox_scanner_t* scanner,
                         vox_toml_elem_t** elem, vox_toml_err_info_t* err_info) {
    const char* start = vox_scanner_curptr(scanner);
    const char* ptr = start;
    bool negative = false;
    
    /* 检查负号 */
    if (*ptr == '-') {
        /* 检查是否是日期时间格式（YYYY-MM-DD 或 YYYY-MM-DDTHH:MM:SS） */
        /* 如果下一个字符是数字，且后面跟着 '-'，可能是日期格式 */
        if (ptr + 1 < scanner->end && isdigit(ptr[1]) &&
            ptr + 5 < scanner->end && ptr[5] == '-') {
            /* 可能是日期格式，不应该解析为负数 */
            set_error(err_info, scanner, "Invalid integer format (possibly a date)");
            return -1;
        }
        negative = true;
        ptr++;
    } else if (*ptr == '+') {
        ptr++;
    }
    
    /* 检查十六进制 */
    if (ptr < scanner->end && *ptr == '0' && 
        ptr + 1 < scanner->end && (ptr[1] == 'x' || ptr[1] == 'X')) {
        ptr += 2;
        while (ptr < scanner->end && isxdigit(*ptr)) {
            ptr++;
        }
    } else if (ptr < scanner->end && *ptr == '0' &&
               ptr + 1 < scanner->end && (ptr[1] == 'o' || ptr[1] == 'O')) {
        /* 八进制 */
        ptr += 2;
        while (ptr < scanner->end && (*ptr >= '0' && *ptr <= '7')) {
            ptr++;
        }
    } else if (ptr < scanner->end && *ptr == '0' &&
               ptr + 1 < scanner->end && (ptr[1] == 'b' || ptr[1] == 'B')) {
        /* 二进制 */
        ptr += 2;
        while (ptr < scanner->end && (*ptr == '0' || *ptr == '1')) {
            ptr++;
        }
    } else {
        /* 十进制 */
        if (ptr >= scanner->end || !isdigit(*ptr)) {
            set_error(err_info, scanner, "Invalid integer format");
            return -1;
        }
        while (ptr < scanner->end && isdigit(*ptr)) {
            ptr++;
        }
    }
    
    size_t len = ptr - start;
    char* num_str = (char*)vox_mpool_alloc(mpool, len + 1);
    if (!num_str) {
        set_error(err_info, scanner, "Memory allocation failed");
        return -1;
    }
    memcpy(num_str, start, len);
    num_str[len] = '\0';
    
    int64_t value;
    /* 确定数字部分的起始位置（跳过负号和前缀） */
    const char* num_start = num_str;
    if (negative) {
        num_start++;  /* 跳过负号 */
    }
    
    size_t num_start_offset = (size_t)(num_start - num_str);
    if (num_start[0] == '0' && len > num_start_offset + 1 && 
        (num_start[1] == 'x' || num_start[1] == 'X')) {
        /* 十六进制 */
        value = (int64_t)strtoll(num_start + 2, NULL, 16);
        if (negative) {
            value = -value;
        }
    } else if (num_start[0] == '0' && len > num_start_offset + 1 &&
               (num_start[1] == 'o' || num_start[1] == 'O')) {
        /* 八进制 */
        value = (int64_t)strtoll(num_start + 2, NULL, 8);
        if (negative) {
            value = -value;
        }
    } else if (num_start[0] == '0' && len > num_start_offset + 1 &&
               (num_start[1] == 'b' || num_start[1] == 'B')) {
        /* 二进制 */
        value = (int64_t)strtoll(num_start + 2, NULL, 2);
        if (negative) {
            value = -value;
        }
    } else {
        /* 十进制：如果 num_str 包含负号，strtoll 会自动处理 */
        /* 如果 negative 为 true，num_str 一定包含 '-' */
        value = (int64_t)strtoll(num_str, NULL, 10);
    }
    
    vox_mpool_free(mpool, num_str);
    vox_scanner_skip(scanner, len);
    
    *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
    if (!*elem) {
        set_error(err_info, scanner, "Memory allocation failed");
        return -1;
    }
    
    memset(*elem, 0, sizeof(vox_toml_elem_t));
    (*elem)->type = VOX_TOML_INTEGER;
    (*elem)->u.integer = value;
    (*elem)->parent = NULL;
    vox_list_node_init(&(*elem)->node);
    
    return 0;
}

/* 解析浮点数 */
static int parse_float( vox_mpool_t* mpool, vox_scanner_t* scanner,
                        vox_toml_elem_t** elem, vox_toml_err_info_t* err_info) {
    const char* start = vox_scanner_curptr(scanner);
    const char* ptr = start;
    bool negative = false;
    
    /* 检查负号 */
    if (*ptr == '-') {
        negative = true;
        ptr++;
    } else if (*ptr == '+') {
        ptr++;
    }
    
    /* 检查特殊值 */
    if (ptr < scanner->end && (*ptr == 'i' || *ptr == 'I')) {
        if (vox_scanner_remaining(scanner) >= 3 &&
            (ptr[1] == 'n' || ptr[1] == 'N') &&
            (ptr[2] == 'f' || ptr[2] == 'F')) {
            ptr += 3;
            size_t len = ptr - start;
            vox_scanner_skip(scanner, len);
            
            *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
            if (!*elem) {
                set_error(err_info, scanner, "Memory allocation failed");
                return -1;
            }
            memset(*elem, 0, sizeof(vox_toml_elem_t));
            (*elem)->type = VOX_TOML_FLOAT;
            (*elem)->u.float_val = negative ? -INFINITY : INFINITY;
            (*elem)->parent = NULL;
            vox_list_node_init(&(*elem)->node);
            return 0;
        }
    } else if (ptr < scanner->end && (*ptr == 'n' || *ptr == 'N')) {
        if (vox_scanner_remaining(scanner) >= 3 &&
            (ptr[1] == 'a' || ptr[1] == 'A') &&
            (ptr[2] == 'n' || ptr[2] == 'N')) {
            ptr += 3;
            size_t len = ptr - start;
            vox_scanner_skip(scanner, len);
            
            *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
            if (!*elem) {
                set_error(err_info, scanner, "Memory allocation failed");
                return -1;
            }
            memset(*elem, 0, sizeof(vox_toml_elem_t));
            (*elem)->type = VOX_TOML_FLOAT;
            (*elem)->u.float_val = NAN;
            (*elem)->parent = NULL;
            vox_list_node_init(&(*elem)->node);
            return 0;
        }
    }
    
    /* 解析数字部分 */
    if (ptr >= scanner->end || !isdigit(*ptr)) {
        set_error(err_info, scanner, "Invalid float format");
        return -1;
    }
    
    while (ptr < scanner->end && isdigit(*ptr)) {
        ptr++;
    }
    
    /* 检查小数点 */
    bool has_dot = false;
    if (ptr < scanner->end && *ptr == '.') {
        has_dot = true;
        ptr++;
        while (ptr < scanner->end && isdigit(*ptr)) {
            ptr++;
        }
    }
    
    /* 检查指数 */
    bool has_exp = false;
    if (ptr < scanner->end && (*ptr == 'e' || *ptr == 'E')) {
        has_exp = true;
        ptr++;
        if (ptr < scanner->end && (*ptr == '+' || *ptr == '-')) {
            ptr++;
        }
        if (ptr >= scanner->end || !isdigit(*ptr)) {
            set_error(err_info, scanner, "Invalid exponent in float");
            return -1;
        }
        while (ptr < scanner->end && isdigit(*ptr)) {
            ptr++;
        }
    }
    
    if (!has_dot && !has_exp) {
        set_error(err_info, scanner, "Invalid float format");
        return -1;
    }
    
    size_t len = ptr - start;
    char* num_str = (char*)vox_mpool_alloc(mpool, len + 1);
    if (!num_str) {
        set_error(err_info, scanner, "Memory allocation failed");
        return -1;
    }
    memcpy(num_str, start, len);
    num_str[len] = '\0';
    
    double value = strtod(num_str, NULL);
    vox_mpool_free(mpool, num_str);
    vox_scanner_skip(scanner, len);
    
    *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
    if (!*elem) {
        set_error(err_info, scanner, "Memory allocation failed");
        return -1;
    }
    
    memset(*elem, 0, sizeof(vox_toml_elem_t));
    (*elem)->type = VOX_TOML_FLOAT;
    (*elem)->u.float_val = value;
    (*elem)->parent = NULL;
    vox_list_node_init(&(*elem)->node);
    
    return 0;
}

/* 解析布尔值 */
static int parse_boolean(vox_mpool_t* mpool, vox_scanner_t* scanner,
                         vox_toml_elem_t** elem, vox_toml_err_info_t* err_info) {
    const char* start = vox_scanner_curptr(scanner);
    size_t remaining = vox_scanner_remaining(scanner);
    
    *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
    if (!*elem) {
        set_error(err_info, scanner, "Memory allocation failed");
        return -1;
    }
    
    memset(*elem, 0, sizeof(vox_toml_elem_t));
    (*elem)->type = VOX_TOML_BOOLEAN;
    (*elem)->parent = NULL;
    vox_list_node_init(&(*elem)->node);
    
    /* 检查 "true" */
    if (remaining >= 4 && memcmp(start, "true", 4) == 0) {
        (*elem)->u.boolean = true;
        vox_scanner_skip(scanner, 4);
        return 0;
    }
    
    /* 检查 "false" */
    if (remaining >= 5 && memcmp(start, "false", 5) == 0) {
        (*elem)->u.boolean = false;
        vox_scanner_skip(scanner, 5);
        return 0;
    }
    
    set_error(err_info, scanner, "Invalid boolean value");
    vox_mpool_free(mpool, *elem);
    *elem = NULL;
    return -1;
}

/* 解析日期时间（简化处理，只解析格式） */
static int parse_datetime(vox_mpool_t* mpool, vox_scanner_t* scanner,
                          vox_toml_elem_t** elem, vox_toml_type_t type,
                          vox_toml_err_info_t* err_info) {
    const char* start = vox_scanner_curptr(scanner);
    const char* ptr = start;
    
    /* 简化的日期时间解析：查找 ISO 8601 格式 */
    /* 格式：1979-05-27T07:32:00Z 或 1979-05-27T07:32:00-07:00 */
    
    if (type == VOX_TOML_TIME) {
        /* 纯时间格式：HH:MM:SS（没有日期部分） */
        /* 时间部分：HH:MM:SS */
        for (int i = 0; i < 2; i++) {
            if (ptr >= scanner->end || !isdigit(*ptr)) {
                set_error(err_info, scanner, "Invalid time format");
                return -1;
            }
            ptr++;
        }
        if (ptr >= scanner->end || *ptr != ':') {
            set_error(err_info, scanner, "Invalid time format");
            return -1;
        }
        ptr++;
        for (int i = 0; i < 2; i++) {
            if (ptr >= scanner->end || !isdigit(*ptr)) {
                set_error(err_info, scanner, "Invalid time format");
                return -1;
            }
            ptr++;
        }
        if (ptr >= scanner->end || *ptr != ':') {
            set_error(err_info, scanner, "Invalid time format");
            return -1;
        }
        ptr++;
        for (int i = 0; i < 2; i++) {
            if (ptr >= scanner->end || !isdigit(*ptr)) {
                set_error(err_info, scanner, "Invalid time format");
                return -1;
            }
            ptr++;
        }
        
        /* 可选的小数秒 */
        if (ptr < scanner->end && *ptr == '.') {
            ptr++;
            while (ptr < scanner->end && isdigit(*ptr)) {
                ptr++;
            }
        }
        
        size_t len = ptr - start;
        vox_scanner_skip(scanner, len);
        
        *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
        if (!*elem) {
            set_error(err_info, scanner, "Memory allocation failed");
            return -1;
        }
        memset(*elem, 0, sizeof(vox_toml_elem_t));
        (*elem)->type = VOX_TOML_TIME;
        (*elem)->u.time.ptr = start;
        (*elem)->u.time.len = len;
        (*elem)->parent = NULL;
        vox_list_node_init(&(*elem)->node);
        return 0;
    }
    
    /* 日期部分：YYYY-MM-DD */
    for (int i = 0; i < 4; i++) {
        if (ptr >= scanner->end || !isdigit(*ptr)) {
            set_error(err_info, scanner, "Invalid date format");
            return -1;
        }
        ptr++;
    }
    if (ptr >= scanner->end || *ptr != '-') {
        set_error(err_info, scanner, "Invalid date format");
        return -1;
    }
    ptr++;
    for (int i = 0; i < 2; i++) {
        if (ptr >= scanner->end || !isdigit(*ptr)) {
            set_error(err_info, scanner, "Invalid date format");
            return -1;
        }
        ptr++;
    }
    if (ptr >= scanner->end || *ptr != '-') {
        set_error(err_info, scanner, "Invalid date format");
        return -1;
    }
    ptr++;
    for (int i = 0; i < 2; i++) {
        if (ptr >= scanner->end || !isdigit(*ptr)) {
            set_error(err_info, scanner, "Invalid date format");
            return -1;
        }
        ptr++;
    }
    
    if (type == VOX_TOML_DATE) {
        size_t len = ptr - start;
        vox_scanner_skip(scanner, len);
        
        *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
        if (!*elem) {
            set_error(err_info, scanner, "Memory allocation failed");
            return -1;
        }
        memset(*elem, 0, sizeof(vox_toml_elem_t));
        (*elem)->type = VOX_TOML_DATE;
        (*elem)->u.date.ptr = start;
        (*elem)->u.date.len = len;
        (*elem)->parent = NULL;
        vox_list_node_init(&(*elem)->node);
        return 0;
    }
    
    /* 日期时间：需要 T 分隔符 */
    if (ptr >= scanner->end || *ptr != 'T') {
        set_error(err_info, scanner, "Invalid datetime format");
        return -1;
    }
    ptr++;
    
    if (type == VOX_TOML_DATETIME) {
        /* 时间部分：HH:MM:SS */
        for (int i = 0; i < 2; i++) {
            if (ptr >= scanner->end || !isdigit(*ptr)) {
                set_error(err_info, scanner, "Invalid datetime format");
                return -1;
            }
            ptr++;
        }
        if (ptr >= scanner->end || *ptr != ':') {
            set_error(err_info, scanner, "Invalid datetime format");
            return -1;
        }
        ptr++;
        for (int i = 0; i < 2; i++) {
            if (ptr >= scanner->end || !isdigit(*ptr)) {
                set_error(err_info, scanner, "Invalid datetime format");
                return -1;
            }
            ptr++;
        }
        if (ptr >= scanner->end || *ptr != ':') {
            set_error(err_info, scanner, "Invalid datetime format");
            return -1;
        }
        ptr++;
        for (int i = 0; i < 2; i++) {
            if (ptr >= scanner->end || !isdigit(*ptr)) {
                set_error(err_info, scanner, "Invalid datetime format");
                return -1;
            }
            ptr++;
        }
        
        /* 可选的小数秒 */
        if (ptr < scanner->end && *ptr == '.') {
            ptr++;
            while (ptr < scanner->end && isdigit(*ptr)) {
                ptr++;
            }
        }
        
        /* 时区偏移（Z 或 +/-HH:MM） */
        if (ptr < scanner->end) {
            if (*ptr == 'Z') {
                ptr++;
            } else if (*ptr == '+' || *ptr == '-') {
                ptr++;
                for (int i = 0; i < 2; i++) {
                    if (ptr >= scanner->end || !isdigit(*ptr)) {
                        set_error(err_info, scanner, "Invalid datetime format");
                        return -1;
                    }
                    ptr++;
                }
                if (ptr >= scanner->end || *ptr != ':') {
                    set_error(err_info, scanner, "Invalid datetime format");
                    return -1;
                }
                ptr++;
                for (int i = 0; i < 2; i++) {
                    if (ptr >= scanner->end || !isdigit(*ptr)) {
                        set_error(err_info, scanner, "Invalid datetime format");
                        return -1;
                    }
                    ptr++;
                }
            }
        }
        
        size_t len = ptr - start;
        vox_scanner_skip(scanner, len);
        
        *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
        if (!*elem) {
            set_error(err_info, scanner, "Memory allocation failed");
            return -1;
        }
        memset(*elem, 0, sizeof(vox_toml_elem_t));
        (*elem)->type = VOX_TOML_DATETIME;
        (*elem)->u.datetime.ptr = start;
        (*elem)->u.datetime.len = len;
        (*elem)->parent = NULL;
        vox_list_node_init(&(*elem)->node);
        return 0;
    }
    
    return -1;
}

/* 前向声明 */
static int parse_value(vox_mpool_t* mpool, vox_scanner_t* scanner,
                       vox_toml_elem_t** elem, vox_toml_err_info_t* err_info);

/* 解析数组 */
static int parse_array(vox_mpool_t* mpool, vox_scanner_t* scanner,
                       vox_toml_elem_t** elem, vox_toml_err_info_t* err_info) {
    if (vox_scanner_peek_char(scanner) != '[') {
        set_error(err_info, scanner, "Expected '[' to start array");
        return -1;
    }
    vox_scanner_get_char(scanner);  /* 跳过 '[' */
    
    *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
    if (!*elem) {
        set_error(err_info, scanner, "Memory allocation failed");
        return -1;
    }
    
    memset(*elem, 0, sizeof(vox_toml_elem_t));
    (*elem)->type = VOX_TOML_ARRAY;
    vox_list_init(&(*elem)->u.array.list);
    (*elem)->parent = NULL;
    vox_list_node_init(&(*elem)->node);
    
    skip_whitespace_and_comments(scanner, err_info);
    
    /* 检查空数组 */
    if (vox_scanner_peek_char(scanner) == ']') {
        vox_scanner_get_char(scanner);
        return 0;
    }
    
    /* 解析数组元素 */
    while (true) {
        skip_whitespace_and_comments(scanner, err_info);
        
        vox_toml_elem_t* item = NULL;
        if (parse_value(mpool, scanner, &item, err_info) != 0) {
            vox_mpool_free(mpool, *elem);
            *elem = NULL;
            return -1;
        }
        
        item->parent = *elem;
        vox_list_push_back(&(*elem)->u.array.list, &item->node);
        
        skip_whitespace_and_comments(scanner, err_info);
        
        int ch = vox_scanner_peek_char(scanner);
        if (ch == ']') {
            vox_scanner_get_char(scanner);
            break;
        } else if (ch == ',') {
            vox_scanner_get_char(scanner);
        } else {
            set_error(err_info, scanner, "Expected ',' or ']' in array");
            vox_mpool_free(mpool, *elem);
            *elem = NULL;
            return -1;
        }
    }
    
    return 0;
}

/* 解析内联表 */
static int parse_inline_table(vox_mpool_t* mpool, vox_scanner_t* scanner,
                               vox_toml_elem_t** elem, vox_toml_err_info_t* err_info) {
    if (vox_scanner_peek_char(scanner) != '{') {
        set_error(err_info, scanner, "Expected '{' to start inline table");
        return -1;
    }
    vox_scanner_get_char(scanner);  /* 跳过 '{' */
    
    *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
    if (!*elem) {
        set_error(err_info, scanner, "Memory allocation failed");
        return -1;
    }
    
    memset(*elem, 0, sizeof(vox_toml_elem_t));
    (*elem)->type = VOX_TOML_INLINE_TABLE;
    vox_list_init(&(*elem)->u.inline_table.keyvalues);
    (*elem)->parent = NULL;
    vox_list_node_init(&(*elem)->node);
    
    vox_scanner_skip_ws(scanner);
    
    /* 检查空内联表 */
    if (vox_scanner_peek_char(scanner) == '}') {
        vox_scanner_get_char(scanner);
        return 0;
    }
    
    /* 解析键值对 */
    while (true) {
        vox_scanner_skip_ws(scanner);
        
        vox_strview_t key;
        if (parse_key(scanner, &key, err_info) != 0) {
            vox_mpool_free(mpool, *elem);
            *elem = NULL;
            return -1;
        }
        
        vox_scanner_skip_ws(scanner);
        
        if (vox_scanner_peek_char(scanner) != '=') {
            set_error(err_info, scanner, "Expected '=' after key");
            vox_mpool_free(mpool, *elem);
            *elem = NULL;
            return -1;
        }
        vox_scanner_get_char(scanner);
        
        vox_scanner_skip_ws(scanner);
        
        vox_toml_elem_t* value = NULL;
        if (parse_value(mpool, scanner, &value, err_info) != 0) {
            vox_mpool_free(mpool, *elem);
            *elem = NULL;
            return -1;
        }
        
        vox_toml_keyvalue_t* kv = (vox_toml_keyvalue_t*)vox_mpool_alloc(mpool,
                                                                     sizeof(vox_toml_keyvalue_t));
        if (!kv) {
            set_error(err_info, scanner, "Memory allocation failed");
            vox_mpool_free(mpool, *elem);
            *elem = NULL;
            return -1;
        }
        
        vox_list_node_init(&kv->node);
        kv->key = key;
        kv->value = value;
        kv->table = NULL;  /* 内联表不是表，所以设为 NULL */
        
        vox_list_push_back(&(*elem)->u.inline_table.keyvalues, &kv->node);
        
        vox_scanner_skip_ws(scanner);
        
        int ch = vox_scanner_peek_char(scanner);
        if (ch == '}') {
            vox_scanner_get_char(scanner);
            break;
        } else if (ch == ',') {
            vox_scanner_get_char(scanner);
        } else {
            set_error(err_info, scanner, "Expected ',' or '}' in inline table");
            vox_mpool_free(mpool, *elem);
            *elem = NULL;
            return -1;
        }
    }
    
    return 0;
}

/* 解析值 */
static int parse_value(vox_mpool_t* mpool, vox_scanner_t* scanner,
                       vox_toml_elem_t** elem, vox_toml_err_info_t* err_info) {
    skip_whitespace_and_comments(scanner, err_info);
    
    if (vox_scanner_eof(scanner)) {
        set_error(err_info, scanner, "Unexpected end of input");
        return -1;
    }
    
    int ch = vox_scanner_peek_char(scanner);
    
    if (ch == '"') {
        /* 基本字符串 */
        vox_strview_t str;
        if (parse_basic_string(scanner, &str, err_info) != 0) {
            return -1;
        }
        
        *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
        if (!*elem) {
            set_error(err_info, scanner, "Memory allocation failed");
            return -1;
        }
        memset(*elem, 0, sizeof(vox_toml_elem_t));
        (*elem)->type = VOX_TOML_STRING;
        (*elem)->u.string = str;
        (*elem)->parent = NULL;
        vox_list_node_init(&(*elem)->node);
        return 0;
    } else if (ch == '\'') {
        /* 字面字符串 */
        vox_strview_t str;
        if (parse_literal_string(scanner, &str, err_info) != 0) {
            return -1;
        }
        
        *elem = (vox_toml_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_elem_t));
        if (!*elem) {
            set_error(err_info, scanner, "Memory allocation failed");
            return -1;
        }
        memset(*elem, 0, sizeof(vox_toml_elem_t));
        (*elem)->type = VOX_TOML_STRING;
        (*elem)->u.string = str;
        (*elem)->parent = NULL;
        vox_list_node_init(&(*elem)->node);
        return 0;
    } else if (ch == '[') {
        /* 数组 */
        return parse_array(mpool, scanner, elem, err_info);
    } else if (ch == '{') {
        /* 内联表 */
        return parse_inline_table(mpool, scanner, elem, err_info);
    } else if (ch == 't' || ch == 'f') {
        /* 布尔值 */
        return parse_boolean(mpool, scanner, elem, err_info);
    } else if (isdigit(ch)) {
        /* 先尝试解析日期时间（优先级高于数字） */
        vox_scanner_state_t save;
        vox_scanner_save_state(scanner, &save);
        
        const char* start = vox_scanner_curptr(scanner);
        if (vox_scanner_remaining(scanner) >= 10) {
            /* 检查是否是日期格式 YYYY-MM-DD */
            bool is_date = true;
            for (int i = 0; i < 4; i++) {
                if (!isdigit(start[i])) {
                    is_date = false;
                    break;
                }
            }
            if (is_date && start[4] == '-' &&
                isdigit(start[5]) && isdigit(start[6]) &&
                start[7] == '-' &&
                isdigit(start[8]) && isdigit(start[9])) {
                /* 检查是否有 T 分隔符（日期时间） */
                if (vox_scanner_remaining(scanner) > 10 && start[10] == 'T') {
                    if (parse_datetime(mpool, scanner, elem, VOX_TOML_DATETIME, err_info) == 0) {
                        return 0;
                    }
                } else {
                    if (parse_datetime(mpool, scanner, elem, VOX_TOML_DATE, err_info) == 0) {
                        return 0;
                    }
                }
            }
        }
        
        /* 检查时间格式 HH:MM:SS */
        if (vox_scanner_remaining(scanner) >= 8 &&
            isdigit(start[0]) && isdigit(start[1]) &&
            start[2] == ':' &&
            isdigit(start[3]) && isdigit(start[4]) &&
            start[5] == ':' &&
            isdigit(start[6]) && isdigit(start[7])) {
            if (parse_datetime(mpool, scanner, elem, VOX_TOML_TIME, err_info) == 0) {
                return 0;
            }
        }
        
        /* 恢复状态，尝试解析数字 */
        vox_scanner_restore_state(scanner, &save);
        
        /* 数字（整数或浮点数）或特殊值 */
        if (parse_integer(mpool, scanner, elem, err_info) == 0) {
            /* 检查下一个字符，如果是 '.' 或 'e'/'E'，则应该是浮点数 */
            int next_ch = vox_scanner_peek_char(scanner);
            if (next_ch == '.' || next_ch == 'e' || next_ch == 'E') {
                vox_mpool_free(mpool, *elem);
                vox_scanner_restore_state(scanner, &save);
                return parse_float(mpool, scanner, elem, err_info);
            }
            return 0;
        }
        
        vox_scanner_restore_state(scanner, &save);
        return parse_float(mpool, scanner, elem, err_info);
    } else if (ch == '+' || ch == '-' || 
               (ch == 'i' || ch == 'I') || (ch == 'n' || ch == 'N')) {
        /* 数字（整数或浮点数）或特殊值（带符号或特殊值） */
        vox_scanner_state_t save;
        vox_scanner_save_state(scanner, &save);
        
        if (parse_integer(mpool, scanner, elem, err_info) == 0) {
            /* 检查下一个字符，如果是 '.' 或 'e'/'E'，则应该是浮点数 */
            int next_ch = vox_scanner_peek_char(scanner);
            if (next_ch == '.' || next_ch == 'e' || next_ch == 'E') {
                vox_mpool_free(mpool, *elem);
                vox_scanner_restore_state(scanner, &save);
                return parse_float(mpool, scanner, elem, err_info);
            }
            return 0;
        }
        
        vox_scanner_restore_state(scanner, &save);
        return parse_float(mpool, scanner, elem, err_info);
    } else {
        set_error(err_info, scanner, "Unexpected character in value");
        return -1;
    }
}

/* 解析表名（点分隔的键名） */
static int parse_table_name(vox_scanner_t* scanner, vox_strview_t* name,
                            vox_toml_err_info_t* err_info) {
    const char* start = vox_scanner_curptr(scanner);
    const char* ptr = start;
    const char* end = scanner->end;
    
    /* 表名可以是点分隔的键名序列 */
    while (ptr < end) {
        int ch = *ptr;
        
        if (ch == ']') {
            break;
        }
        
        if (ch == '"' || ch == '\'') {
            /* 引号键 */
            vox_scanner_skip(scanner, ptr - vox_scanner_curptr(scanner));
            vox_strview_t quoted_key;
            if (ch == '"') {
                if (parse_basic_string(scanner, &quoted_key, err_info) != 0) {
                    return -1;
                }
            } else {
                if (parse_literal_string(scanner, &quoted_key, err_info) != 0) {
                    return -1;
                }
            }
            ptr = vox_scanner_curptr(scanner);
        } else if (isalnum(ch) || ch == '_' || ch == '-') {
            /* Bare key */
            ptr++;
            while (ptr < end && (isalnum(*ptr) || *ptr == '_' || *ptr == '-')) {
                ptr++;
            }
        } else if (ch == '.') {
            ptr++;
        } else {
            set_error(err_info, scanner, "Invalid character in table name");
            return -1;
        }
        
        if (ptr < end && *ptr == '.') {
            ptr++;
        }
    }
    
    size_t len = ptr - start;
    vox_scanner_skip(scanner, len);
    
    name->ptr = start;
    name->len = len;
    
    return 0;
}

/* 查找或创建表（支持点分隔的表名，如 "server.database"） */
static vox_toml_table_t* find_or_create_table(vox_mpool_t* mpool,
                                                vox_toml_table_t* root,
                                                const vox_strview_t* table_name,
                                                bool is_array_of_tables) {
    /* 检查表名是否包含点（嵌套表） */
    const char* dot = NULL;
    for (size_t i = 0; i < table_name->len; i++) {
        if (table_name->ptr[i] == '.') {
            dot = table_name->ptr + i;
            break;
        }
    }
    
    if (dot) {
        /* 嵌套表：先找到或创建父表，然后在父表中创建子表 */
        size_t parent_len = dot - table_name->ptr;
        vox_strview_t parent_name;
        parent_name.ptr = table_name->ptr;
        parent_name.len = parent_len;
        
        /* 递归查找或创建父表 */
        vox_toml_table_t* parent = find_or_create_table(mpool, root, &parent_name, false);
        if (!parent) {
            return NULL;
        }
        
        /* 在父表中查找或创建子表 */
        size_t child_len = (table_name->ptr + table_name->len) - (dot + 1);
        vox_strview_t child_name;
        child_name.ptr = dot + 1;
        child_name.len = child_len;
        
        /* 在父表的子表中查找 */
        vox_toml_table_t* table = NULL;
        vox_list_node_t* node = vox_list_first(&parent->subtables);
        while (node) {
            vox_toml_table_t* t = vox_container_of(node, vox_toml_table_t, node);
            if (vox_strview_compare(&t->name, &child_name) == 0 &&
                t->is_array_of_tables == is_array_of_tables) {
                if (is_array_of_tables) {
                    /* 表数组：返回最后一个元素 */
                    while (node) {
                        table = t;
                        node = node->next;
                        if (node != &parent->subtables.head) {
                            t = vox_container_of(node, vox_toml_table_t, node);
                            if (vox_strview_compare(&t->name, &child_name) == 0 &&
                                t->is_array_of_tables == is_array_of_tables) {
                                continue;
                            }
                        }
                        break;
                    }
                } else {
                    table = t;
                }
                break;
            }
            node = node->next;
            if (node == &parent->subtables.head) {
                break;
            }
        }
        
        if (!table) {
            /* 创建新子表 */
            table = (vox_toml_table_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_table_t));
            if (!table) {
                return NULL;
            }
            memset(table, 0, sizeof(vox_toml_table_t));
            table->name = child_name;
            table->is_array_of_tables = is_array_of_tables;
            table->parent = parent;
            vox_list_init(&table->keyvalues);
            vox_list_init(&table->subtables);
            vox_list_node_init(&table->node);
            vox_list_push_back(&parent->subtables, &table->node);
        }
        
        return table;
    } else {
        /* 非嵌套表：直接在根表中查找或创建 */
        vox_toml_table_t* table = NULL;
        vox_list_node_t* node = vox_list_first(&root->subtables);
        while (node) {
            vox_toml_table_t* t = vox_container_of(node, vox_toml_table_t, node);
            if (vox_strview_compare(&t->name, table_name) == 0 &&
                t->is_array_of_tables == is_array_of_tables) {
                if (is_array_of_tables) {
                    /* 表数组：返回最后一个元素 */
                    while (node) {
                        table = t;
                        node = node->next;
                        if (node != &root->subtables.head) {
                            t = vox_container_of(node, vox_toml_table_t, node);
                            if (vox_strview_compare(&t->name, table_name) == 0 &&
                                t->is_array_of_tables == is_array_of_tables) {
                                continue;
                            }
                        }
                        break;
                    }
                } else {
                    table = t;
                }
                break;
            }
            node = node->next;
            if (node == &root->subtables.head) {
                break;
            }
        }
        
        if (!table) {
            /* 创建新表 */
            table = (vox_toml_table_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_table_t));
            if (!table) {
                return NULL;
            }
            memset(table, 0, sizeof(vox_toml_table_t));
            table->name = *table_name;
            table->is_array_of_tables = is_array_of_tables;
            table->parent = root;
            vox_list_init(&table->keyvalues);
            vox_list_init(&table->subtables);
            vox_list_node_init(&table->node);
            vox_list_push_back(&root->subtables, &table->node);
        }
        
        return table;
    }
}

/* 解析表头 */
static int parse_table_header(vox_mpool_t* mpool, vox_scanner_t* scanner,
                              vox_toml_table_t* root, vox_toml_table_t** table,
                              vox_toml_err_info_t* err_info) {
    skip_whitespace_and_comments(scanner, err_info);
    
    int ch = vox_scanner_peek_char(scanner);
    bool is_array_of_tables = false;
    
    if (ch == '[') {
        vox_scanner_get_char(scanner);
        ch = vox_scanner_peek_char(scanner);
        if (ch == '[') {
            /* 表数组 */
            is_array_of_tables = true;
            vox_scanner_get_char(scanner);
        }
    } else {
        set_error(err_info, scanner, "Expected '[' to start table header");
        return -1;
    }
    
    skip_whitespace_and_comments(scanner, err_info);
    
    vox_strview_t table_name;
    if (parse_table_name(scanner, &table_name, err_info) != 0) {
        return -1;
    }
    
    skip_whitespace_and_comments(scanner, err_info);
    
    if (is_array_of_tables) {
        ch = vox_scanner_peek_char(scanner);
        if (ch != ']') {
            set_error(err_info, scanner, "Expected ']' in array of tables header");
            return -1;
        }
        vox_scanner_get_char(scanner);
    }
    
    ch = vox_scanner_peek_char(scanner);
    if (ch != ']') {
        set_error(err_info, scanner, "Expected ']' to end table header");
        return -1;
    }
    vox_scanner_get_char(scanner);
    
    *table = find_or_create_table(mpool, root, &table_name, is_array_of_tables);
    if (!*table) {
        set_error(err_info, scanner, "Memory allocation failed");
        return -1;
    }
    
    return 0;
}

/* 解析键值对 */
static int parse_keyvalue_pair(vox_mpool_t* mpool, vox_scanner_t* scanner,
                               vox_toml_table_t* table, vox_toml_err_info_t* err_info) {
    skip_whitespace_and_comments(scanner, err_info);
    
    /* 解析键名（支持点分隔的键名） */
    vox_strview_t key;
    if (parse_key(scanner, &key, err_info) != 0) {
        return -1;
    }
    
    /* 检查点分隔的键名 */
    while (vox_scanner_peek_char(scanner) == '.') {
        vox_scanner_get_char(scanner);
        vox_strview_t next_key;
        if (parse_key(scanner, &next_key, err_info) != 0) {
            return -1;
        }
        /* 简化处理：将点分隔的键名合并为一个键名 */
        /* 实际应该创建嵌套表，这里简化处理 */
        key.len = (next_key.ptr + next_key.len) - key.ptr;
    }
    
    skip_whitespace_and_comments(scanner, err_info);
    
    if (vox_scanner_peek_char(scanner) != '=') {
        set_error(err_info, scanner, "Expected '=' after key");
        return -1;
    }
    vox_scanner_get_char(scanner);
    
    skip_whitespace_and_comments(scanner, err_info);
    
    vox_toml_elem_t* value = NULL;
    if (parse_value(mpool, scanner, &value, err_info) != 0) {
        return -1;
    }
    
    vox_toml_keyvalue_t* kv = (vox_toml_keyvalue_t*)vox_mpool_alloc(mpool,
                                                                     sizeof(vox_toml_keyvalue_t));
    if (!kv) {
        set_error(err_info, scanner, "Memory allocation failed");
        return -1;
    }
    
    vox_list_node_init(&kv->node);
    kv->key = key;
    kv->value = value;
    kv->table = table;  /* 设置所属表 */
    
    vox_list_push_back(&table->keyvalues, &kv->node);
    
    return 0;
}

/* 解析 TOML 文档 */
static vox_toml_table_t* parse_toml_document(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                              vox_toml_err_info_t* err_info) {
    /* 创建根表 */
    vox_toml_table_t* root = (vox_toml_table_t*)vox_mpool_alloc(mpool, sizeof(vox_toml_table_t));
    if (!root) {
        set_error(err_info, scanner, "Memory allocation failed");
        return NULL;
    }
    
    memset(root, 0, sizeof(vox_toml_table_t));
    root->name.ptr = NULL;
    root->name.len = 0;
    root->parent = NULL;
    root->is_array_of_tables = false;
    vox_list_init(&root->keyvalues);
    vox_list_init(&root->subtables);
    vox_list_node_init(&root->node);
    
    vox_toml_table_t* current_table = root;
    
    while (!vox_scanner_eof(scanner)) {
        skip_whitespace_and_comments(scanner, err_info);
        
        if (vox_scanner_eof(scanner)) {
            break;
        }
        
        int ch = vox_scanner_peek_char(scanner);
        
        if (ch == '[') {
            /* 表头 */
            if (parse_table_header(mpool, scanner, root, &current_table, err_info) != 0) {
                /* 错误处理：简化处理，直接返回 */
                return root;
            }
        } else {
            /* 键值对 */
            if (parse_keyvalue_pair(mpool, scanner, current_table, err_info) != 0) {
                /* 错误处理：简化处理，直接返回 */
                return root;
            }
        }
    }
    
    return root;
}

/* ===== 公共接口实现 ===== */

vox_toml_table_t* vox_toml_parse(vox_mpool_t* mpool, char* buffer, size_t* size,
                                 vox_toml_err_info_t* err_info) {
    if (!mpool || !buffer) {
        if (err_info) {
            err_info->line = 0;
            err_info->column = 0;
            err_info->offset = 0;
            err_info->message = "Invalid parameters";
        }
        return NULL;
    }
    
    /* 初始化扫描器 */
    vox_scanner_t scanner;
    size_t buf_len = size ? *size : strlen(buffer);
    if (vox_scanner_init(&scanner, buffer, buf_len, VOX_SCANNER_NONE) != 0) {
        if (err_info) {
            err_info->line = 0;
            err_info->column = 0;
            err_info->offset = 0;
            err_info->message = "Failed to initialize scanner";
        }
        return NULL;
    }
    
    vox_toml_table_t* root = parse_toml_document(mpool, &scanner, err_info);
    
    if (root && size) {
        *size = vox_scanner_offset(&scanner);
    }
    
    vox_scanner_destroy(&scanner);
    return root;
}

vox_toml_table_t* vox_toml_parse_str(vox_mpool_t* mpool, const char* toml_str,
                                     vox_toml_err_info_t* err_info) {
    if (!mpool || !toml_str) {
        if (err_info) {
            err_info->line = 0;
            err_info->column = 0;
            err_info->offset = 0;
            err_info->message = "Invalid parameters";
        }
        return NULL;
    }
    
    size_t len = strlen(toml_str);
    char* buffer = (char*)vox_mpool_alloc(mpool, len + 1);
    if (!buffer) {
        if (err_info) {
            err_info->line = 0;
            err_info->column = 0;
            err_info->offset = 0;
            err_info->message = "Memory allocation failed";
        }
        return NULL;
    }
    
    memcpy(buffer, toml_str, len);
    buffer[len] = '\0';
    
    size_t size = len;
    vox_toml_table_t* root = vox_toml_parse(mpool, buffer, &size, err_info);
    
    return root;
}

vox_toml_table_t* vox_toml_parse_file(vox_mpool_t* mpool, const char* filepath,
                                      vox_toml_err_info_t* err_info) {
    if (!mpool || !filepath) {
        if (err_info) {
            err_info->line = 0;
            err_info->column = 0;
            err_info->offset = 0;
            err_info->message = "Invalid parameters";
        }
        return NULL;
    }

    size_t size = 0;
    char* buffer = (char*)vox_file_read_all(mpool, filepath, &size);
    if (!buffer) {
        if (err_info) {
            err_info->line = 0;
            err_info->column = 0;
            err_info->offset = 0;
            err_info->message = "Failed to read file";
        }
        return NULL;
    }

    if (size == 0 || buffer[size] != '\0') {
        char* new_buf = (char*)vox_mpool_realloc(mpool, buffer, size + 1);
        if (!new_buf) {
            if (err_info) {
                err_info->line = 0;
                err_info->column = 0;
                err_info->offset = 0;
                err_info->message = "Memory allocation failed";
            }
            return NULL;
        }
        buffer = new_buf;
        buffer[size] = '\0';
    }

    size_t parse_size = size;
    vox_toml_table_t* root = vox_toml_parse(mpool, buffer, &parse_size, err_info);

    return root;
}

/* ===== 类型检查接口 ===== */

vox_toml_type_t vox_toml_get_type(const vox_toml_elem_t* elem) {
    return elem ? elem->type : VOX_TOML_STRING;
}

bool vox_toml_is_type(const vox_toml_elem_t* elem, vox_toml_type_t type) {
    return elem && elem->type == type;
}

/* ===== 值获取接口 ===== */

vox_strview_t vox_toml_get_string(const vox_toml_elem_t* elem) {
    vox_strview_t null_view = VOX_STRVIEW_NULL;
    if (!elem || elem->type != VOX_TOML_STRING) {
        return null_view;
    }
    return elem->u.string;
}

int64_t vox_toml_get_integer(const vox_toml_elem_t* elem) {
    if (!elem || elem->type != VOX_TOML_INTEGER) {
        return 0;
    }
    return elem->u.integer;
}

double vox_toml_get_float(const vox_toml_elem_t* elem) {
    if (!elem || elem->type != VOX_TOML_FLOAT) {
        return 0.0;
    }
    return elem->u.float_val;
}

bool vox_toml_get_boolean(const vox_toml_elem_t* elem) {
    if (!elem || elem->type != VOX_TOML_BOOLEAN) {
        return false;
    }
    return elem->u.boolean;
}

vox_strview_t vox_toml_get_datetime(const vox_toml_elem_t* elem) {
    vox_strview_t null_view = VOX_STRVIEW_NULL;
    if (!elem || elem->type != VOX_TOML_DATETIME) {
        return null_view;
    }
    return elem->u.datetime;
}

vox_strview_t vox_toml_get_date(const vox_toml_elem_t* elem) {
    vox_strview_t null_view = VOX_STRVIEW_NULL;
    if (!elem || elem->type != VOX_TOML_DATE) {
        return null_view;
    }
    return elem->u.date;
}

vox_strview_t vox_toml_get_time(const vox_toml_elem_t* elem) {
    vox_strview_t null_view = VOX_STRVIEW_NULL;
    if (!elem || elem->type != VOX_TOML_TIME) {
        return null_view;
    }
    return elem->u.time;
}

size_t vox_toml_get_array_count(const vox_toml_elem_t* elem) {
    if (!elem || elem->type != VOX_TOML_ARRAY) {
        return 0;
    }
    return vox_list_size(&elem->u.array.list);
}

vox_toml_elem_t* vox_toml_get_array_elem(const vox_toml_elem_t* elem, size_t index) {
    if (!elem || elem->type != VOX_TOML_ARRAY) {
        return NULL;
    }
    
    vox_list_node_t* node = vox_list_first(&elem->u.array.list);
    for (size_t i = 0; i < index && node; i++) {
        node = node->next;
        if (node == &elem->u.array.list.head) {
            return NULL;
        }
    }
    
    if (!node || node == &elem->u.array.list.head) {
        return NULL;
    }
    
    return vox_container_of(node, vox_toml_elem_t, node);
}

size_t vox_toml_get_inline_table_count(const vox_toml_elem_t* elem) {
    if (!elem || elem->type != VOX_TOML_INLINE_TABLE) {
        return 0;
    }
    return vox_list_size(&elem->u.inline_table.keyvalues);
}

vox_toml_elem_t* vox_toml_get_inline_table_value(const vox_toml_elem_t* elem, const char* key) {
    if (!elem || elem->type != VOX_TOML_INLINE_TABLE || !key) {
        return NULL;
    }
    
    vox_toml_keyvalue_t* kv;
    vox_list_for_each_entry(kv, &elem->u.inline_table.keyvalues, vox_toml_keyvalue_t, node) {
        if (vox_strview_compare_cstr(&kv->key, key) == 0) {
            return kv->value;
        }
    }
    
    return NULL;
}

/* ===== 表操作接口 ===== */

size_t vox_toml_get_keyvalue_count(const vox_toml_table_t* table) {
    if (!table) {
        return 0;
    }
    return vox_list_size(&table->keyvalues);
}

size_t vox_toml_get_subtable_count(const vox_toml_table_t* table) {
    if (!table) {
        return 0;
    }
    return vox_list_size(&table->subtables);
}

vox_toml_keyvalue_t* vox_toml_find_keyvalue(const vox_toml_table_t* table, const char* key) {
    if (!table || !key) {
        return NULL;
    }
    
    vox_toml_keyvalue_t* kv;
    vox_list_for_each_entry(kv, &table->keyvalues, vox_toml_keyvalue_t, node) {
        if (vox_strview_compare_cstr(&kv->key, key) == 0) {
            return kv;
        }
    }
    
    return NULL;
}

vox_toml_elem_t* vox_toml_get_value(const vox_toml_table_t* table, const char* key) {
    vox_toml_keyvalue_t* kv = vox_toml_find_keyvalue(table, key);
    return kv ? kv->value : NULL;
}

vox_toml_table_t* vox_toml_find_subtable(const vox_toml_table_t* table, const char* name) {
    if (!table || !name) {
        return NULL;
    }
    
    vox_toml_table_t* subtable;
    vox_list_for_each_entry(subtable, &table->subtables, vox_toml_table_t, node) {
        if (vox_strview_compare_cstr(&subtable->name, name) == 0) {
            return subtable;
        }
    }
    
    return NULL;
}

vox_toml_table_t* vox_toml_find_table_by_path(const vox_toml_table_t* root, const char* path) {
    if (!root || !path) {
        return NULL;
    }
    
    const char* ptr = path;
    vox_toml_table_t* current = (vox_toml_table_t*)root;
    
    while (*ptr) {
        const char* dot = strchr(ptr, '.');
        size_t len = dot ? (size_t)(dot - ptr) : strlen(ptr);
        
        char* key = (char*)alloca(len + 1);
        memcpy(key, ptr, len);
        key[len] = '\0';
        
        current = vox_toml_find_subtable(current, key);
        if (!current) {
            return NULL;
        }
        
        if (dot) {
            ptr = dot + 1;
        } else {
            break;
        }
    }
    
    return current;
}

/* ===== 遍历接口 ===== */

vox_toml_elem_t* vox_toml_array_first(const vox_toml_elem_t* elem) {
    if (!elem || elem->type != VOX_TOML_ARRAY) {
        return NULL;
    }
    vox_list_node_t* node = vox_list_first(&elem->u.array.list);
    return node ? vox_container_of(node, vox_toml_elem_t, node) : NULL;
}

vox_toml_elem_t* vox_toml_array_next(const vox_toml_elem_t* elem) {
    if (!elem || !elem->parent || elem->parent->type != VOX_TOML_ARRAY) {
        return NULL;
    }
    vox_list_node_t* node = elem->node.next;
    if (node == &elem->parent->u.array.list.head) {
        return NULL;
    }
    return vox_container_of(node, vox_toml_elem_t, node);
}

vox_toml_keyvalue_t* vox_toml_table_first_keyvalue(const vox_toml_table_t* table) {
    if (!table) {
        return NULL;
    }
    vox_list_node_t* node = vox_list_first(&table->keyvalues);
    return node ? vox_container_of(node, vox_toml_keyvalue_t, node) : NULL;
}

vox_toml_keyvalue_t* vox_toml_table_next_keyvalue(const vox_toml_keyvalue_t* kv) {
    if (!kv || !kv->table) {
        return NULL;
    }
    vox_list_node_t* node = kv->node.next;
    /* 检查是否到达链表末尾（哨兵节点） */
    /* 哨兵节点的特征是：node 等于 &list->head */
    if (node == &kv->table->keyvalues.head) {
        return NULL;
    }
    return vox_container_of(node, vox_toml_keyvalue_t, node);
}

vox_toml_table_t* vox_toml_table_first_subtable(const vox_toml_table_t* table) {
    if (!table) {
        return NULL;
    }
    vox_list_node_t* node = vox_list_first(&table->subtables);
    return node ? vox_container_of(node, vox_toml_table_t, node) : NULL;
}

vox_toml_table_t* vox_toml_table_next_subtable(const vox_toml_table_t* subtable) {
    if (!subtable) {
        return NULL;
    }
    vox_list_node_t* node = subtable->node.next;
    if (node == &subtable->parent->subtables.head) {
        return NULL;
    }
    return vox_container_of(node, vox_toml_table_t, node);
}

/* ===== 调试和输出接口 ===== */

void vox_toml_print_elem(const vox_toml_elem_t* elem, int indent) {
    if (!elem) {
        printf("(null)");
        return;
    }
    
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    
    switch (elem->type) {
        case VOX_TOML_STRING:
            printf("\"%.*s\"", (int)elem->u.string.len, elem->u.string.ptr);
            break;
        case VOX_TOML_INTEGER:
            printf("%lld", (long long)elem->u.integer);
            break;
        case VOX_TOML_FLOAT:
            printf("%.15g", elem->u.float_val);
            break;
        case VOX_TOML_BOOLEAN:
            printf("%s", elem->u.boolean ? "true" : "false");
            break;
        case VOX_TOML_DATETIME:
            printf("\"%.*s\"", (int)elem->u.datetime.len, elem->u.datetime.ptr);
            break;
        case VOX_TOML_DATE:
            printf("\"%.*s\"", (int)elem->u.date.len, elem->u.date.ptr);
            break;
        case VOX_TOML_TIME:
            printf("\"%.*s\"", (int)elem->u.time.len, elem->u.time.ptr);
            break;
        case VOX_TOML_ARRAY:
            printf("[\n");
            {
                vox_toml_elem_t* item = vox_toml_array_first(elem);
                bool first = true;
                while (item) {
                    if (!first) {
                        printf(",\n");
                    }
                    vox_toml_print_elem(item, indent + 1);
                    item = vox_toml_array_next(item);
                    first = false;
                }
                if (!first) {
                    printf("\n");
                }
            }
            for (int i = 0; i < indent; i++) {
                printf("  ");
            }
            printf("]");
            break;
        case VOX_TOML_INLINE_TABLE:
            printf("{");
            {
                vox_list_node_t* node = vox_list_first(&elem->u.inline_table.keyvalues);
                bool first = true;
                while (node && node != &elem->u.inline_table.keyvalues.head) {
                    vox_toml_keyvalue_t* kv = vox_container_of(node, vox_toml_keyvalue_t, node);
                    if (!first) {
                        printf(", ");
                    }
                    printf("\"%.*s\" = ", (int)kv->key.len, kv->key.ptr);
                    vox_toml_print_elem(kv->value, 0);
                    node = node->next;
                    first = false;
                }
            }
            printf("}");
            break;
        default:
            printf("(unknown)");
            break;
    }
}

void vox_toml_print_table(const vox_toml_table_t* table, int indent) {
    if (!table) {
        printf("(null table)");
        return;
    }
    
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    
    if (table->name.len > 0) {
        printf("[%.*s]\n", (int)table->name.len, table->name.ptr);
    }
    
    /* 打印键值对 */
    vox_toml_keyvalue_t* kv = vox_toml_table_first_keyvalue(table);
    while (kv) {
        for (int i = 0; i < indent + 1; i++) {
            printf("  ");
        }
        printf("\"%.*s\" = ", (int)kv->key.len, kv->key.ptr);
        vox_toml_print_elem(kv->value, 0);
        printf("\n");
        kv = vox_toml_table_next_keyvalue(kv);
    }
    
    /* 打印子表 */
    vox_toml_table_t* subtable = vox_toml_table_first_subtable(table);
    while (subtable) {
        vox_toml_print_table(subtable, indent + 1);
        subtable = vox_toml_table_next_subtable(subtable);
    }
}

/* ===== 序列化辅助函数 ===== */

/* 扩展输出缓冲区 */
static int expand_output_buffer(vox_mpool_t* mpool, char** output, size_t* output_size, 
                                 size_t* output_capacity, size_t needed) {
    if (!output || !output_size || !output_capacity) return -1;
    
    if (*output_size + needed + 1 <= *output_capacity) {
        return 0;  /* 容量足够 */
    }
    
    /* 扩展缓冲区 */
    size_t new_capacity = *output_capacity * 2;
    if (new_capacity < *output_size + needed + 1) {
        new_capacity = *output_size + needed + 1;
    }
    
    char* new_output = (char*)vox_mpool_alloc(mpool, new_capacity);
    if (!new_output) {
        return -1;
    }
    
    if (*output && *output_size > 0) {
        memcpy(new_output, *output, *output_size);
    }
    
    if (*output) {
        vox_mpool_free(mpool, *output);
    }
    
    *output = new_output;
    *output_capacity = new_capacity;
    
    return 0;
}

/* 追加字符串到输出缓冲区 */
static int append_string(vox_mpool_t* mpool, char** output, size_t* output_size,
                          size_t* output_capacity, const char* str, size_t len) {
    if (expand_output_buffer(mpool, output, output_size, output_capacity, len) != 0) {
        return -1;
    }
    
    memcpy(*output + *output_size, str, len);
    *output_size += len;
    (*output)[*output_size] = '\0';
    
    return 0;
}

/* 追加格式化字符串到输出缓冲区 */
static int append_format(vox_mpool_t* mpool, char** output, size_t* output_size,
                          size_t* output_capacity, const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    /* 计算需要的空间 */
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);
    
    if (needed < 0) {
        va_end(args);
        return -1;
    }
    
    if (expand_output_buffer(mpool, output, output_size, output_capacity, needed) != 0) {
        va_end(args);
        return -1;
    }
    
    int written = vsnprintf(*output + *output_size, *output_capacity - *output_size, format, args);
    va_end(args);
    
    if (written < 0 || (size_t)written >= *output_capacity - *output_size) {
        return -1;
    }
    
    *output_size += written;
    (*output)[*output_size] = '\0';
    
    return 0;
}

/* 转义字符串（用于 TOML 基本字符串） */
static int escape_string(vox_mpool_t* mpool, const vox_strview_t* str, char** output,
                          size_t* output_size, size_t* output_capacity) {
    if (append_string(mpool, output, output_size, output_capacity, "\"", 1) != 0) {
        return -1;
    }
    
    for (size_t i = 0; i < str->len; i++) {
        unsigned char ch = (unsigned char)str->ptr[i];
        switch (ch) {
            case '"':
                if (append_string(mpool, output, output_size, output_capacity, "\\\"", 2) != 0) return -1;
                break;
            case '\\':
                if (append_string(mpool, output, output_size, output_capacity, "\\\\", 2) != 0) return -1;
                break;
            case '\b':
                if (append_string(mpool, output, output_size, output_capacity, "\\b", 2) != 0) return -1;
                break;
            case '\t':
                if (append_string(mpool, output, output_size, output_capacity, "\\t", 2) != 0) return -1;
                break;
            case '\n':
                if (append_string(mpool, output, output_size, output_capacity, "\\n", 2) != 0) return -1;
                break;
            case '\r':
                if (append_string(mpool, output, output_size, output_capacity, "\\r", 2) != 0) return -1;
                break;
            case '\f':
                if (append_string(mpool, output, output_size, output_capacity, "\\f", 2) != 0) return -1;
                break;
            default:
                if (ch < 0x20 || ch == 0x7F) {
                    /* 控制字符，使用 Unicode 转义 */
                    if (append_format(mpool, output, output_size, output_capacity, "\\u%04x", ch) != 0) {
                        return -1;
                    }
                } else {
                    /* 普通字符和 UTF-8 多字节字符，直接输出 */
                    if (append_string(mpool, output, output_size, output_capacity, (const char*)&str->ptr[i], 1) != 0) return -1;
                }
                break;
        }
    }
    
    if (append_string(mpool, output, output_size, output_capacity, "\"", 1) != 0) {
        return -1;
    }
    
    return 0;
}

/* ===== 序列化接口实现 ===== */

int vox_toml_serialize_elem(vox_mpool_t* mpool, const vox_toml_elem_t* elem, int indent,
                            char** output, size_t* output_size, size_t* output_capacity) {
    (void)indent;  /* 未使用的参数（保留用于未来格式化） */
    if (!mpool || !elem || !output || !output_size || !output_capacity) {
        return -1;
    }
    
    /* 初始化输出缓冲区 */
    if (*output == NULL) {
        *output_capacity = 1024;
        *output = (char*)vox_mpool_alloc(mpool, *output_capacity);
        if (!*output) {
            return -1;
        }
        *output_size = 0;
        (*output)[0] = '\0';
    }
    
    switch (elem->type) {
        case VOX_TOML_STRING:
            return escape_string(mpool, &elem->u.string, output, output_size, output_capacity);
            
        case VOX_TOML_INTEGER:
            return append_format(mpool, output, output_size, output_capacity, "%lld", (long long)elem->u.integer);
            
        case VOX_TOML_FLOAT:
            return append_format(mpool, output, output_size, output_capacity, "%.15g", elem->u.float_val);
            
        case VOX_TOML_BOOLEAN:
            return append_string(mpool, output, output_size, output_capacity,
                                  elem->u.boolean ? "true" : "false",
                                  elem->u.boolean ? 4 : 5);
            
        case VOX_TOML_DATETIME:
            return append_format(mpool, output, output_size, output_capacity, "%.*s",
                                  (int)elem->u.datetime.len, elem->u.datetime.ptr);
            
        case VOX_TOML_DATE:
            return append_format(mpool, output, output_size, output_capacity, "%.*s",
                                  (int)elem->u.date.len, elem->u.date.ptr);
            
        case VOX_TOML_TIME:
            return append_format(mpool, output, output_size, output_capacity, "%.*s",
                                  (int)elem->u.time.len, elem->u.time.ptr);
            
        case VOX_TOML_ARRAY: {
            if (append_string(mpool, output, output_size, output_capacity, "[", 1) != 0) return -1;
            
            vox_toml_elem_t* item = vox_toml_array_first(elem);
            bool first = true;
            while (item) {
                if (!first) {
                    if (append_string(mpool, output, output_size, output_capacity, ", ", 2) != 0) return -1;
                }
                if (vox_toml_serialize_elem(mpool, item, 0, output, output_size, output_capacity) != 0) {
                    return -1;
                }
                item = vox_toml_array_next(item);
                first = false;
            }
            
            if (append_string(mpool, output, output_size, output_capacity, "]", 1) != 0) return -1;
            return 0;
        }
        
        case VOX_TOML_INLINE_TABLE: {
            if (append_string(mpool, output, output_size, output_capacity, "{ ", 2) != 0) return -1;
            
            vox_list_node_t* node = vox_list_first(&elem->u.inline_table.keyvalues);
            bool first = true;
            while (node && node != &elem->u.inline_table.keyvalues.head) {
                vox_toml_keyvalue_t* kv = vox_container_of(node, vox_toml_keyvalue_t, node);
                if (!first) {
                    if (append_string(mpool, output, output_size, output_capacity, ", ", 2) != 0) return -1;
                }
                
                /* 键名 */
                if (append_format(mpool, output, output_size, output_capacity, "%.*s = ",
                                   (int)kv->key.len, kv->key.ptr) != 0) {
                    return -1;
                }
                
                /* 值 */
                if (vox_toml_serialize_elem(mpool, kv->value, 0, output, output_size, output_capacity) != 0) {
                    return -1;
                }
                
                node = node->next;
                first = false;
            }
            
            if (append_string(mpool, output, output_size, output_capacity, " }", 2) != 0) return -1;
            return 0;
        }
        
        default:
            return -1;
    }
}

int vox_toml_serialize_table(vox_mpool_t* mpool, const vox_toml_table_t* table, int indent,
                              char** output, size_t* output_size, size_t* output_capacity) {
    if (!mpool || !table || !output || !output_size || !output_capacity) {
        return -1;
    }
    
    /* 初始化输出缓冲区 */
    if (*output == NULL) {
        *output_capacity = 1024;
        *output = (char*)vox_mpool_alloc(mpool, *output_capacity);
        if (!*output) {
            return -1;
        }
        *output_size = 0;
        (*output)[0] = '\0';
    }
    
    /* 打印表头（如果不是根表） */
    if (table->name.len > 0) {
        for (int i = 0; i < indent; i++) {
            if (append_string(mpool, output, output_size, output_capacity, "  ", 2) != 0) return -1;
        }
        
        if (table->is_array_of_tables) {
            if (append_format(mpool, output, output_size, output_capacity, "[[%.*s]]\n",
                               (int)table->name.len, table->name.ptr) != 0) {
                return -1;
            }
        } else {
            if (append_format(mpool, output, output_size, output_capacity, "[%.*s]\n",
                               (int)table->name.len, table->name.ptr) != 0) {
                return -1;
            }
        }
    }
    
    /* 序列化键值对 */
    vox_toml_keyvalue_t* kv = vox_toml_table_first_keyvalue(table);
    while (kv) {
        for (int i = 0; i < indent; i++) {
            if (append_string(mpool, output, output_size, output_capacity, "  ", 2) != 0) return -1;
        }
        
        /* 键名 */
        if (append_format(mpool, output, output_size, output_capacity, "%.*s = ",
                           (int)kv->key.len, kv->key.ptr) != 0) {
            return -1;
        }
        
        /* 值 */
        if (vox_toml_serialize_elem(mpool, kv->value, 0, output, output_size, output_capacity) != 0) {
            return -1;
        }
        
        if (append_string(mpool, output, output_size, output_capacity, "\n", 1) != 0) return -1;
        
        kv = vox_toml_table_next_keyvalue(kv);
    }
    
    /* 序列化子表 */
    vox_toml_table_t* subtable = vox_toml_table_first_subtable(table);
    while (subtable) {
        if (append_string(mpool, output, output_size, output_capacity, "\n", 1) != 0) return -1;
        if (vox_toml_serialize_table(mpool, subtable, indent, output, output_size, output_capacity) != 0) {
            return -1;
        }
        subtable = vox_toml_table_next_subtable(subtable);
    }
    
    return 0;
}

char* vox_toml_to_string(vox_mpool_t* mpool, const vox_toml_table_t* root, size_t* output_size) {
    if (!mpool || !root) {
        return NULL;
    }
    
    char* output = NULL;
    size_t output_size_local = 0;
    size_t output_capacity = 0;
    
    if (vox_toml_serialize_table(mpool, root, 0, &output, &output_size_local, &output_capacity) != 0) {
        if (output) {
            vox_mpool_free(mpool, output);
        }
        return NULL;
    }
    
    if (output_size) {
        *output_size = output_size_local;
    }
    
    return output;
}

int vox_toml_write_file(vox_mpool_t* mpool, const vox_toml_table_t* root, const char* filepath) {
    if (!mpool || !root || !filepath) {
        return -1;
    }
    
    size_t output_size = 0;
    char* content = vox_toml_to_string(mpool, root, &output_size);
    if (!content) {
        return -1;
    }
    
    return vox_file_write_all(mpool, filepath, content, output_size);
}
