/*
 * vox_json.c - 高性能 JSON 解析器实现
 * 使用 vox_scanner 实现零拷贝解析，使用 vox_mpool 进行内存管理
 */

#include "vox_json.h"
#include "vox_os.h"  /* 使用 VOX_UNUSED 宏 */
#include "vox_file.h"
#include "vox_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>

/* ===== 内部辅助函数 ===== */

/* 更新错误信息 */
static void set_error(vox_json_err_info_t* err_info, vox_scanner_t* scanner,
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

/* 解析 null */
static vox_json_elem_t* parse_null(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                    vox_json_err_info_t* err_info) {
    const char* start = vox_scanner_curptr(scanner);
    
    /* 检查 "null" */
    if (vox_scanner_remaining(scanner) < 4) {
        set_error(err_info, scanner, "Unexpected end of input while parsing null");
        return NULL;
    }
    
    if (memcmp(start, "null", 4) != 0) {
        set_error(err_info, scanner, "Invalid null value");
        return NULL;
    }
    
    vox_scanner_skip(scanner, 4);
    
    vox_json_elem_t* elem = (vox_json_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_json_elem_t));
    if (!elem) {
        set_error(err_info, scanner, "Memory allocation failed");
        return NULL;
    }
    
    memset(elem, 0, sizeof(vox_json_elem_t));
    elem->type = VOX_JSON_NULL;
    elem->parent = NULL;
    vox_list_node_init(&elem->node);
    
    return elem;
}

/* 解析布尔值 */
static vox_json_elem_t* parse_boolean(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                       vox_json_err_info_t* err_info) {
    const char* start = vox_scanner_curptr(scanner);
    size_t remaining = vox_scanner_remaining(scanner);
    
    vox_json_elem_t* elem = (vox_json_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_json_elem_t));
    if (!elem) {
        set_error(err_info, scanner, "Memory allocation failed");
        return NULL;
    }
    
    memset(elem, 0, sizeof(vox_json_elem_t));
    elem->type = VOX_JSON_BOOLEAN;
    elem->parent = NULL;
    vox_list_node_init(&elem->node);
    
    /* 检查 "true" */
    if (remaining >= 4 && memcmp(start, "true", 4) == 0) {
        elem->u.boolean = true;
        vox_scanner_skip(scanner, 4);
        return elem;
    }
    
    /* 检查 "false" */
    if (remaining >= 5 && memcmp(start, "false", 5) == 0) {
        elem->u.boolean = false;
        vox_scanner_skip(scanner, 5);
        return elem;
    }
    
    set_error(err_info, scanner, "Invalid boolean value");
    vox_mpool_free(mpool, elem);
    return NULL;
}


/* 解析数字 */
static vox_json_elem_t* parse_number(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                      vox_json_err_info_t* err_info) {
    const char* start = vox_scanner_curptr(scanner);
    const char* ptr = start;
    bool has_dot = false;  /* 标记是否包含小数点 */
    bool has_exp = false;  /* 标记是否包含指数 */
    
    /* 检查负号 */
    if (*ptr == '-') {
        ptr++;
    }
    
    /* 检查整数部分 */
    if (*ptr == '0') {
        ptr++;
    } else if (*ptr >= '1' && *ptr <= '9') {
        ptr++;
        while (ptr < scanner->end && *ptr >= '0' && *ptr <= '9') {
            ptr++;
        }
    } else {
        set_error(err_info, scanner, "Invalid number format");
        return NULL;
    }
    
    /* JSON 规范：不允许前导零（如 01、-09），仅允许 0、0.xxx、0Exxx */
    if (ptr - start >= 1 && ptr - start <= 2 && start[ptr - start - 1] == '0' &&
        ptr < scanner->end && *ptr >= '0' && *ptr <= '9') {
        set_error(err_info, scanner, "Leading zeros not allowed");
        return NULL;
    }
    
    /* 检查小数部分 */
    if (ptr < scanner->end && *ptr == '.') {
        has_dot = true;
        ptr++;
        if (ptr >= scanner->end || *ptr < '0' || *ptr > '9') {
            set_error(err_info, scanner, "Invalid number format: missing digits after decimal point");
            return NULL;
        }
        while (ptr < scanner->end && *ptr >= '0' && *ptr <= '9') {
            ptr++;
        }
    }
    
    /* 检查指数部分 */
    if (ptr < scanner->end && (*ptr == 'e' || *ptr == 'E')) {
        has_exp = true;
        ptr++;
        if (ptr < scanner->end && (*ptr == '+' || *ptr == '-')) {
            ptr++;
        }
        if (ptr >= scanner->end || *ptr < '0' || *ptr > '9') {
            set_error(err_info, scanner, "Invalid number format: missing digits in exponent");
            return NULL;
        }
        while (ptr < scanner->end && *ptr >= '0' && *ptr <= '9') {
            ptr++;
        }
    }
    
    /* 标记变量为已使用（避免警告） */
    VOX_UNUSED(has_dot);
    VOX_UNUSED(has_exp);
    
    /* 解析数字值 */
    size_t len = ptr - start;
    char* num_str = (char*)vox_mpool_alloc(mpool, len + 1);
    if (!num_str) {
        set_error(err_info, scanner, "Memory allocation failed");
        return NULL;
    }
    memcpy(num_str, start, len);
    num_str[len] = '\0';
    errno = 0;
    double value = strtod(num_str, NULL);
    vox_mpool_free(mpool, num_str);
    
    /* 溢出/下溢或非有限数（Inf/NaN）均视为非法 JSON 数字 */
    if (errno == ERANGE || !isfinite(value)) {
        set_error(err_info, scanner, errno == ERANGE ? "Number overflow or underflow" : "Number must be finite (no Inf/NaN)");
        return NULL;
    }
    
    /* 跳过已解析的数字 */
    vox_scanner_skip(scanner, len);
    
    vox_json_elem_t* elem = (vox_json_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_json_elem_t));
    if (!elem) {
        set_error(err_info, scanner, "Memory allocation failed");
        return NULL;
    }
    
    memset(elem, 0, sizeof(vox_json_elem_t));
    elem->type = VOX_JSON_NUMBER;
    elem->u.number = value;
    elem->parent = NULL;
    vox_list_node_init(&elem->node);
    
    return elem;
}

/* 解析转义字符（未使用，保留用于未来扩展） */
VOX_UNUSED_FUNC static int parse_escape_char(vox_scanner_t* scanner, char* out) {
    if (vox_scanner_eof(scanner)) {
        return -1;
    }
    
    int ch = vox_scanner_get_char(scanner);
    switch (ch) {
        case '"':  *out = '"';  return 0;
        case '\\': *out = '\\'; return 0;
        case '/':  *out = '/';  return 0;
        case 'b':  *out = '\b'; return 0;
        case 'f':  *out = '\f'; return 0;
        case 'n':  *out = '\n'; return 0;
        case 'r':  *out = '\r'; return 0;
        case 't':  *out = '\t'; return 0;
        case 'u':  /* Unicode 转义，简化处理：返回 '?' */
            /* 跳过4个十六进制字符 */
            for (int i = 0; i < 4; i++) {
                int hex = vox_scanner_get_char(scanner);
                if (hex < 0 || !isxdigit(hex)) {
                    return -1;
                }
            }
            *out = '?';  /* 简化处理 */
            return 0;
        default:
            return -1;
    }
}

/* 解析字符串 */
static vox_json_elem_t* parse_string(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                      vox_json_err_info_t* err_info) {
    /* 跳过开始的引号 */
    if (vox_scanner_peek_char(scanner) != '"') {
        set_error(err_info, scanner, "Expected string to start with '\"'");
        return NULL;
    }
    vox_scanner_get_char(scanner);
    
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
                return NULL;
            }
            /* 处理转义字符 */
            if (*ptr == 'u') {
                /* Unicode 转义：跳过4个十六进制字符 */
                ptr++;
                for (int i = 0; i < 4; i++) {
                    if (ptr >= end || !isxdigit(*ptr)) {
                        set_error(err_info, scanner, "Invalid Unicode escape sequence");
                        return NULL;
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
        set_error(err_info, scanner, "Unterminated string");
        return NULL;
    }
    
    size_t len = ptr - start;
    
    /* 跳过字符串内容 */
    vox_scanner_skip(scanner, len);
    
    /* 跳过结束引号 */
    if (vox_scanner_peek_char(scanner) != '"') {
        set_error(err_info, scanner, "Expected string to end with '\"'");
        return NULL;
    }
    vox_scanner_get_char(scanner);
    
    vox_json_elem_t* elem = (vox_json_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_json_elem_t));
    if (!elem) {
        set_error(err_info, scanner, "Memory allocation failed");
        return NULL;
    }
    
    memset(elem, 0, sizeof(vox_json_elem_t));
    elem->type = VOX_JSON_STRING;
    elem->u.string.ptr = start;
    elem->u.string.len = len;
    elem->parent = NULL;
    vox_list_node_init(&elem->node);
    
    return elem;
}

/* 内部解析函数（使用已初始化的扫描器） */
static vox_json_elem_t* parse_value(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                      vox_json_err_info_t* err_info);

/* 解析数组 */
static vox_json_elem_t* parse_array(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                     vox_json_err_info_t* err_info) {
    /* 跳过开始的 '[' */
    if (vox_scanner_peek_char(scanner) != '[') {
        set_error(err_info, scanner, "Expected array to start with '['");
        return NULL;
    }
    vox_scanner_get_char(scanner);
    
    vox_json_elem_t* elem = (vox_json_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_json_elem_t));
    if (!elem) {
        set_error(err_info, scanner, "Memory allocation failed");
        return NULL;
    }
    
    memset(elem, 0, sizeof(vox_json_elem_t));
    elem->type = VOX_JSON_ARRAY;
    vox_list_init(&elem->u.array.list);
    elem->parent = NULL;
    vox_list_node_init(&elem->node);
    
    /* 检查空数组 */
    if (vox_scanner_peek_char(scanner) == ']') {
        vox_scanner_get_char(scanner);
        return elem;
    }
    
    /* 解析数组元素 */
    while (true) {
        /* 解析元素 */
        vox_json_elem_t* item = parse_value(mpool, scanner, err_info);
        if (!item) {
            vox_mpool_free(mpool, elem);
            return NULL;
        }
        
        item->parent = elem;
        vox_list_push_back(&elem->u.array.list, &item->node);
        
        /* 检查结束或逗号 */
        int ch = vox_scanner_peek_char(scanner);
        if (ch == ']') {
            vox_scanner_get_char(scanner);
            break;
        } else if (ch == ',') {
            vox_scanner_get_char(scanner);
        } else {
            set_error(err_info, scanner, "Expected ',' or ']' in array");
            vox_mpool_free(mpool, elem);
            return NULL;
        }
    }
    
    return elem;
}

/* 解析对象 */
static vox_json_elem_t* parse_object(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                      vox_json_err_info_t* err_info) {
    /* 跳过开始的 '{' */
    if (vox_scanner_peek_char(scanner) != '{') {
        set_error(err_info, scanner, "Expected object to start with '{'");
        return NULL;
    }
    vox_scanner_get_char(scanner);
    
    vox_json_elem_t* elem = (vox_json_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_json_elem_t));
    if (!elem) {
        set_error(err_info, scanner, "Memory allocation failed");
        return NULL;
    }
    
    memset(elem, 0, sizeof(vox_json_elem_t));
    elem->type = VOX_JSON_OBJECT;
    vox_list_init(&elem->u.object.list);
    elem->parent = NULL;
    vox_list_node_init(&elem->node);
    
    /* 检查空对象 */
    if (vox_scanner_peek_char(scanner) == '}') {
        vox_scanner_get_char(scanner);
        return elem;
    }
    
    /* 解析对象成员 */
    while (true) {
        /* 解析键名（字符串） */
        if (vox_scanner_peek_char(scanner) != '"') {
            set_error(err_info, scanner, "Expected object key to be a string");
            vox_mpool_free(mpool, elem);
            return NULL;
        }
        
        vox_json_elem_t* key_elem = parse_string(mpool, scanner, err_info);
        if (!key_elem) {
            vox_mpool_free(mpool, elem);
            return NULL;
        }
        
        vox_strview_t key_name = key_elem->u.string;
        vox_mpool_free(mpool, key_elem);  /* 释放临时元素，只保留字符串视图 */
        
        /* 跳过 ':' */
        if (vox_scanner_peek_char(scanner) != ':') {
            set_error(err_info, scanner, "Expected ':' after object key");
            vox_mpool_free(mpool, elem);
            return NULL;
        }
        vox_scanner_get_char(scanner);
        
        /* 解析值 */
        vox_json_elem_t* value = parse_value(mpool, scanner, err_info);
        if (!value) {
            vox_mpool_free(mpool, elem);
            return NULL;
        }
        
        value->parent = elem;
        
        /* 创建成员 */
        vox_json_member_t* member = (vox_json_member_t*)vox_mpool_alloc(mpool, 
                                                                         sizeof(vox_json_member_t));
        if (!member) {
            set_error(err_info, scanner, "Memory allocation failed");
            vox_mpool_free(mpool, elem);
            return NULL;
        }
        
        vox_list_node_init(&member->node);
        member->name = key_name;
        member->value = value;
        
        vox_list_push_back(&elem->u.object.list, &member->node);
        
        /* 检查结束或逗号 */
        int ch = vox_scanner_peek_char(scanner);
        if (ch == '}') {
            vox_scanner_get_char(scanner);
            break;
        } else if (ch == ',') {
            vox_scanner_get_char(scanner);
        } else {
            set_error(err_info, scanner, "Expected ',' or '}' in object");
            vox_mpool_free(mpool, elem);
            return NULL;
        }
    }
    
    return elem;
}

/* ===== 公共接口实现 ===== */

/* 内部解析函数（使用已初始化的扫描器） */
static vox_json_elem_t* parse_value(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                      vox_json_err_info_t* err_info) {
    if (vox_scanner_eof(scanner)) {
        set_error(err_info, scanner, "Unexpected end of input");
        return NULL;
    }
    
    vox_json_elem_t* elem = NULL;
    int ch = vox_scanner_peek_char(scanner);
    
    switch (ch) {
        case 'n':
            elem = parse_null(mpool, scanner, err_info);
            break;
        case 't':
        case 'f':
            elem = parse_boolean(mpool, scanner, err_info);
            break;
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            elem = parse_number(mpool, scanner, err_info);
            break;
        case '"':
            elem = parse_string(mpool, scanner, err_info);
            break;
        case '[':
            elem = parse_array(mpool, scanner, err_info);
            break;
        case '{':
            elem = parse_object(mpool, scanner, err_info);
            break;
        default:
            set_error(err_info, scanner, "Unexpected character");
            break;
    }
    
    return elem;
}

/* 解析 JSON */
vox_json_elem_t* vox_json_parse(vox_mpool_t* mpool, char* buffer, size_t* size,
                                vox_json_err_info_t* err_info) {
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
    if (vox_scanner_init(&scanner, buffer, buf_len, VOX_SCANNER_AUTOSKIP_WS_NL) != 0) {
        if (err_info) {
            err_info->line = 0;
            err_info->column = 0;
            err_info->offset = 0;
            err_info->message = "Failed to initialize scanner";
        }
        return NULL;
    }
    
    vox_json_elem_t* elem = parse_value(mpool, &scanner, err_info);
    
    if (elem) {
        if (!vox_scanner_eof(&scanner)) {
            set_error(err_info, &scanner, "Unexpected content after JSON value");
            /* 不释放 elem，让调用者决定如何处理 */
        }
        
        if (size) {
            *size = vox_scanner_offset(&scanner);
        }
    }
    
    vox_scanner_destroy(&scanner);
    return elem;
}

/* 从 C 字符串解析 */
vox_json_elem_t* vox_json_parse_str(vox_mpool_t* mpool, const char* json_str,
                                    vox_json_err_info_t* err_info) {
    if (!mpool || !json_str) {
        if (err_info) {
            err_info->line = 0;
            err_info->column = 0;
            err_info->offset = 0;
            err_info->message = "Invalid parameters";
        }
        return NULL;
    }
    
    size_t len = strlen(json_str);
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
    
    memcpy(buffer, json_str, len);
    buffer[len] = '\0';
    
    size_t size = len;
    vox_json_elem_t* elem = vox_json_parse(mpool, buffer, &size, err_info);
    if (!elem) {
        vox_mpool_free(mpool, buffer);
        return NULL;
    }
    return elem;
}

vox_json_elem_t* vox_json_parse_file(vox_mpool_t* mpool, const char* filepath,
                                     vox_json_err_info_t* err_info) {
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

    /* 确保缓冲区以'\0'结尾，scanner 要求可写缓冲区且有终止符 */
    if (size == 0 || buffer[size] != '\0') {
        /* vox_file_read_all 约定 size 不包含结尾 '\0'，此处手动补一个 */
        char* new_buf = (char*)vox_mpool_realloc(mpool, buffer, size + 1);
        if (!new_buf) {
            vox_mpool_free(mpool, buffer);
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
    vox_json_elem_t* elem = vox_json_parse(mpool, buffer, &parse_size, err_info);
    if (!elem) {
        vox_mpool_free(mpool, buffer);
        return NULL;
    }
    /* vox_json_parse 使用字符串视图引用 buffer，成功时 buffer 由 mpool 管理，会在 mpool_destroy 时释放。*/
    return elem;
}

/* ===== 类型检查接口 ===== */

vox_json_type_t vox_json_get_type(const vox_json_elem_t* elem) {
    return elem ? elem->type : VOX_JSON_NULL;
}

bool vox_json_is_type(const vox_json_elem_t* elem, vox_json_type_t type) {
    return elem && elem->type == type;
}

/* ===== 值获取接口 ===== */

bool vox_json_get_bool(const vox_json_elem_t* elem) {
    if (!elem || elem->type != VOX_JSON_BOOLEAN) {
        return false;
    }
    return elem->u.boolean;
}

double vox_json_get_number(const vox_json_elem_t* elem) {
    if (!elem || elem->type != VOX_JSON_NUMBER) {
        return 0.0;
    }
    return elem->u.number;
}

/* int64_t 在 double 中的安全范围：[-2^63, 2^63)，避免 (int64_t)n 未定义行为 */
#define VOX_JSON_INT64_MIN_AS_DOUBLE  (-9223372036854775808.0)
#define VOX_JSON_INT64_MAX_EXCL_AS_DOUBLE  (9223372036854775808.0)  /* 2^63 */

bool vox_json_number_is_integer(const vox_json_elem_t* elem) {
    if (!elem || elem->type != VOX_JSON_NUMBER) return false;
    double n = elem->u.number;
    if (!isfinite(n) || n < VOX_JSON_INT64_MIN_AS_DOUBLE || n >= VOX_JSON_INT64_MAX_EXCL_AS_DOUBLE)
        return false;
    return n == (double)(int64_t)n;
}

int64_t vox_json_get_int(const vox_json_elem_t* elem) {
    if (!elem || elem->type != VOX_JSON_NUMBER) {
        return 0;
    }
    double n = elem->u.number;
    /* 类型与范围严格检查：非有限或超出 int64_t 范围时返回 0，避免未定义行为 */
    if (!isfinite(n) || n < VOX_JSON_INT64_MIN_AS_DOUBLE || n >= VOX_JSON_INT64_MAX_EXCL_AS_DOUBLE) {
        return 0;
    }
    return (int64_t)n;
}

vox_strview_t vox_json_get_string(const vox_json_elem_t* elem) {
    vox_strview_t null_view = VOX_STRVIEW_NULL;
    if (!elem || elem->type != VOX_JSON_STRING) {
        return null_view;
    }
    return elem->u.string;
}

size_t vox_json_get_array_count(const vox_json_elem_t* elem) {
    if (!elem || elem->type != VOX_JSON_ARRAY) {
        return 0;
    }
    return vox_list_size(&elem->u.array.list);
}

vox_json_elem_t* vox_json_get_array_elem(const vox_json_elem_t* elem, size_t index) {
    if (!elem || elem->type != VOX_JSON_ARRAY) {
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
    
    return vox_container_of(node, vox_json_elem_t, node);
}

size_t vox_json_get_object_count(const vox_json_elem_t* elem) {
    if (!elem || elem->type != VOX_JSON_OBJECT) {
        return 0;
    }
    return vox_list_size(&elem->u.object.list);
}

vox_json_member_t* vox_json_get_object_member(const vox_json_elem_t* elem, 
                                               const char* name) {
    if (!elem || elem->type != VOX_JSON_OBJECT || !name) {
        return NULL;
    }
    
    vox_json_member_t* member;
    vox_list_for_each_entry(member, &elem->u.object.list, vox_json_member_t, node) {
        if (vox_strview_compare_cstr(&member->name, name) == 0) {
            return member;
        }
    }
    
    return NULL;
}

vox_json_elem_t* vox_json_get_object_value(const vox_json_elem_t* elem, 
                                           const char* name) {
    vox_json_member_t* member = vox_json_get_object_member(elem, name);
    return member ? member->value : NULL;
}

/* ===== 遍历接口 ===== */

vox_json_elem_t* vox_json_array_first(const vox_json_elem_t* elem) {
    if (!elem || elem->type != VOX_JSON_ARRAY) {
        return NULL;
    }
    vox_list_node_t* node = vox_list_first(&elem->u.array.list);
    return node ? vox_container_of(node, vox_json_elem_t, node) : NULL;
}

vox_json_elem_t* vox_json_array_next(const vox_json_elem_t* elem) {
    if (!elem || !elem->parent || elem->parent->type != VOX_JSON_ARRAY) {
        return NULL;
    }
    vox_list_node_t* node = elem->node.next;
    /* 检查是否到达链表末尾（哨兵节点）
     * 哨兵节点的特征是：node->next == node->prev（都指向 head）
     * 或者更简单：检查 node 是否等于 &list->head
     */
    if (node == &elem->parent->u.array.list.head) {
        return NULL;
    }
    return vox_container_of(node, vox_json_elem_t, node);
}

vox_json_member_t* vox_json_object_first(const vox_json_elem_t* elem) {
    if (!elem || elem->type != VOX_JSON_OBJECT) {
        return NULL;
    }
    vox_list_node_t* node = vox_list_first(&elem->u.object.list);
    return node ? vox_container_of(node, vox_json_member_t, node) : NULL;
}

vox_json_member_t* vox_json_object_next(const vox_json_member_t* member) {
    if (!member || !member->value || !member->value->parent || 
        member->value->parent->type != VOX_JSON_OBJECT) {
        return NULL;
    }
    vox_list_node_t* node = member->node.next;
    /* 检查是否到达链表末尾（哨兵节点）
     * 哨兵节点的特征是：node 等于 &list->head
     */
    if (node == &member->value->parent->u.object.list.head) {
        return NULL;
    }
    return vox_container_of(node, vox_json_member_t, node);
}

/* ===== 调试和输出接口 ===== */

void vox_json_print(const vox_json_elem_t* elem, int indent) {
    if (!elem) {
        printf("null");
        return;
    }
    
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    
    switch (elem->type) {
        case VOX_JSON_NULL:
            printf("null");
            break;
        case VOX_JSON_BOOLEAN:
            printf("%s", elem->u.boolean ? "true" : "false");
            break;
        case VOX_JSON_NUMBER:
            printf("%.15g", elem->u.number);
            break;
        case VOX_JSON_STRING:
            printf("\"%.*s\"", (int)elem->u.string.len, elem->u.string.ptr);
            break;
        case VOX_JSON_ARRAY:
            printf("[\n");
            {
                vox_json_elem_t* item;
                vox_list_node_t* node;
                bool first = true;
                vox_list_for_each(node, &elem->u.array.list) {
                    item = vox_container_of(node, vox_json_elem_t, node);
                    if (!first) {
                        printf(",");
                    }
                    printf("\n");
                    vox_json_print(item, indent + 1);
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
        case VOX_JSON_OBJECT:
            printf("{\n");
            {
                vox_json_member_t* member;
                bool first = true;
                vox_list_for_each_entry(member, &elem->u.object.list, vox_json_member_t, node) {
                    if (!first) {
                        printf(",");
                    }
                    printf("\n");
                    for (int i = 0; i < indent + 1; i++) {
                        printf("  ");
                    }
                    printf("\"%.*s\": ", (int)member->name.len, member->name.ptr);
                    vox_json_print(member->value, indent + 1);
                    first = false;
                }
                if (!first) {
                    printf("\n");
                }
            }
            for (int i = 0; i < indent; i++) {
                printf("  ");
            }
            printf("}");
            break;
    }
}

/* ===== 序列化内部：写入上下文与 append ===== */

typedef struct {
    char* buf;
    size_t size;
    size_t used;
    vox_string_t* str;  /* 非 NULL 时写入 vox_string，否则写入 buf */
    int indent;
    bool pretty;
} vox_json_serialize_ctx_t;

static void serialize_append(vox_json_serialize_ctx_t* ctx, const char* str, size_t len) {
    if (ctx->str) {
        vox_string_append_data(ctx->str, str, len);
    } else if (ctx->buf && ctx->used + len < ctx->size) {
        memcpy(ctx->buf + ctx->used, str, len);
    }
    ctx->used += len;
}

static void serialize_append_cstr(vox_json_serialize_ctx_t* ctx, const char* str) {
    size_t len = strlen(str);
    serialize_append(ctx, str, len);
}

static void serialize_append_indent(vox_json_serialize_ctx_t* ctx) {
    if (!ctx->pretty) return;
    serialize_append_cstr(ctx, "\n");
    for (int i = 0; i < ctx->indent; i++) {
        serialize_append_cstr(ctx, "  ");
    }
}

/* 计算并写入转义后的字符串（含前后引号） */
static void serialize_string_escaped(vox_json_serialize_ctx_t* ctx,
                                      const char* ptr, size_t len) {
    static const char hex[] = "0123456789abcdef";
    serialize_append(ctx, "\"", 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)ptr[i];
        if (c == '"') {
            serialize_append_cstr(ctx, "\\\"");
        } else if (c == '\\') {
            serialize_append_cstr(ctx, "\\\\");
        } else if (c == '\b') {
            serialize_append_cstr(ctx, "\\b");
        } else if (c == '\f') {
            serialize_append_cstr(ctx, "\\f");
        } else if (c == '\n') {
            serialize_append_cstr(ctx, "\\n");
        } else if (c == '\r') {
            serialize_append_cstr(ctx, "\\r");
        } else if (c == '\t') {
            serialize_append_cstr(ctx, "\\t");
        } else if (c < 0x20) {
            char u[] = "\\u0000";
            u[4] = hex[c >> 4];
            u[5] = hex[c & 0x0f];
            serialize_append(ctx, u, 6);
        } else {
            serialize_append(ctx, ptr + i, 1);
        }
    }
    serialize_append(ctx, "\"", 1);
}

static void serialize_value(vox_json_serialize_ctx_t* ctx, const vox_json_elem_t* elem);

static void serialize_value(vox_json_serialize_ctx_t* ctx, const vox_json_elem_t* elem) {
    if (!elem) {
        serialize_append_cstr(ctx, "null");
        return;
    }
    switch (elem->type) {
        case VOX_JSON_NULL:
            serialize_append_cstr(ctx, "null");
            break;
        case VOX_JSON_BOOLEAN:
            serialize_append_cstr(ctx, elem->u.boolean ? "true" : "false");
            break;
        case VOX_JSON_NUMBER: {
            double n = elem->u.number;
            /* 先检查有限性与范围，再转 int64_t，避免 NaN/Inf/超大数导致未定义行为 */
            if (isfinite(n) && n >= -9007199254740991.0 && n <= 9007199254740991.0 &&
                n == (double)(int64_t)n) {
                char num[32];
                int sz = snprintf(num, sizeof(num), "%" PRId64, (int64_t)n);
                if (sz > 0 && (size_t)sz < sizeof(num)) {
                    serialize_append(ctx, num, (size_t)sz);
                } else {
                    serialize_append_cstr(ctx, "0");
                }
            } else {
                /* 使用 %.15g：兼顾可读性与精度，常见小数如 3.14、2.718 输出简洁 */
                char num[64];
                int sz = snprintf(num, sizeof(num), "%.15g", n);
                if (sz > 0 && (size_t)sz < sizeof(num)) {
                    serialize_append(ctx, num, (size_t)sz);
                } else {
                    serialize_append_cstr(ctx, "0");
                }
            }
            break;
        }
        case VOX_JSON_STRING:
            serialize_string_escaped(ctx, elem->u.string.ptr, elem->u.string.len);
            break;
        case VOX_JSON_ARRAY: {
            serialize_append(ctx, "[", 1);
            ctx->indent += (ctx->pretty ? 1 : 0);
            vox_json_elem_t* item;
            vox_list_node_t* node;
            bool first = true;
            vox_list_for_each(node, &elem->u.array.list) {
                item = vox_container_of(node, vox_json_elem_t, node);
                if (!first) serialize_append(ctx, ",", 1);
                if (ctx->pretty) serialize_append_indent(ctx);
                serialize_value(ctx, item);
                first = false;
            }
            ctx->indent -= (ctx->pretty ? 1 : 0);
            if (ctx->pretty && !first) serialize_append_indent(ctx);
            serialize_append(ctx, "]", 1);
            break;
        }
        case VOX_JSON_OBJECT: {
            serialize_append(ctx, "{", 1);
            ctx->indent += (ctx->pretty ? 1 : 0);
            vox_json_member_t* member;
            bool first = true;
            vox_list_for_each_entry(member, &elem->u.object.list, vox_json_member_t, node) {
                if (!first) serialize_append(ctx, ",", 1);
                if (ctx->pretty) serialize_append_indent(ctx);
                serialize_string_escaped(ctx, member->name.ptr, member->name.len);
                serialize_append_cstr(ctx, ctx->pretty ? ": " : ":");
                serialize_value(ctx, member->value);
                first = false;
            }
            ctx->indent -= (ctx->pretty ? 1 : 0);
            if (ctx->pretty && !first) serialize_append_indent(ctx);
            serialize_append(ctx, "}", 1);
            break;
        }
    }
}

vox_string_t* vox_json_to_string(vox_mpool_t* mpool, const vox_json_elem_t* elem, bool pretty) {
    if (!mpool) return NULL;
    vox_string_t* str = vox_string_create(mpool);
    if (!str) return NULL;
    vox_json_serialize_ctx_t ctx = {
        .buf = NULL,
        .size = 0,
        .used = 0,
        .str = str,
        .indent = 0,
        .pretty = pretty
    };
    serialize_value(&ctx, elem);
    return str;
}

int vox_json_serialize(const vox_json_elem_t* elem, char* buffer, size_t size,
                       size_t* written, bool pretty) {
    if (!written) return -1;
    vox_json_serialize_ctx_t ctx = {
        .buf = buffer,
        .size = size,
        .used = 0,
        .str = NULL,
        .indent = 0,
        .pretty = pretty
    };
    serialize_value(&ctx, elem);
    *written = ctx.used;
    if (buffer && size > ctx.used) {
        buffer[ctx.used] = '\0';
        return 0;
    }
    if (buffer && size > 0) {
        if (size > 0) buffer[0] = '\0';
        return -1; /* 空间不足 */
    }
    return 0; /* 仅计算长度 */
}

/* ===== 构建接口实现 ===== */

static vox_json_elem_t* new_elem(vox_mpool_t* mpool, vox_json_type_t type) {
    vox_json_elem_t* elem = (vox_json_elem_t*)vox_mpool_alloc(mpool, sizeof(vox_json_elem_t));
    if (!elem) return NULL;
    memset(elem, 0, sizeof(vox_json_elem_t));
    elem->type = type;
    elem->parent = NULL;
    vox_list_node_init(&elem->node);
    return elem;
}

vox_json_elem_t* vox_json_new_null(vox_mpool_t* mpool) {
    return new_elem(mpool, VOX_JSON_NULL);
}

vox_json_elem_t* vox_json_new_bool(vox_mpool_t* mpool, bool value) {
    vox_json_elem_t* elem = new_elem(mpool, VOX_JSON_BOOLEAN);
    if (elem) elem->u.boolean = value;
    return elem;
}

vox_json_elem_t* vox_json_new_number(vox_mpool_t* mpool, double value) {
    vox_json_elem_t* elem = new_elem(mpool, VOX_JSON_NUMBER);
    if (elem) elem->u.number = value;
    return elem;
}

vox_json_elem_t* vox_json_new_string(vox_mpool_t* mpool, const char* str, size_t len) {
    if (!str && len > 0) return NULL;
    char* copy = (char*)vox_mpool_alloc(mpool, len + 1);
    if (!copy) return NULL;
    if (str) memcpy(copy, str, len);
    copy[len] = '\0';
    vox_json_elem_t* elem = new_elem(mpool, VOX_JSON_STRING);
    if (!elem) {
        vox_mpool_free(mpool, copy);
        return NULL;
    }
    elem->u.string.ptr = copy;
    elem->u.string.len = len;
    return elem;
}

vox_json_elem_t* vox_json_new_string_cstr(vox_mpool_t* mpool, const char* cstr) {
    if (!cstr) return vox_json_new_string(mpool, NULL, 0);
    return vox_json_new_string(mpool, cstr, strlen(cstr));
}

vox_json_elem_t* vox_json_new_array(vox_mpool_t* mpool) {
    vox_json_elem_t* elem = new_elem(mpool, VOX_JSON_ARRAY);
    if (elem) vox_list_init(&elem->u.array.list);
    return elem;
}

vox_json_elem_t* vox_json_new_object(vox_mpool_t* mpool) {
    vox_json_elem_t* elem = new_elem(mpool, VOX_JSON_OBJECT);
    if (elem) vox_list_init(&elem->u.object.list);
    return elem;
}

int vox_json_array_append(vox_json_elem_t* array_elem, vox_json_elem_t* value_elem) {
    if (!array_elem || array_elem->type != VOX_JSON_ARRAY || !value_elem) return -1;
    value_elem->parent = array_elem;
    vox_list_push_back(&array_elem->u.array.list, &value_elem->node);
    return 0;
}

int vox_json_object_set(vox_mpool_t* mpool, vox_json_elem_t* object_elem,
                        const char* name, vox_json_elem_t* value_elem) {
    if (!mpool || !object_elem || object_elem->type != VOX_JSON_OBJECT || !name || !value_elem)
        return -1;
    size_t name_len = strlen(name);
    vox_json_member_t* old = vox_json_get_object_member(object_elem, name);
    if (old) {
        vox_list_remove(&object_elem->u.object.list, &old->node);
        vox_mpool_free(mpool, old);
    }
    char* key_copy = (char*)vox_mpool_alloc(mpool, name_len + 1);
    if (!key_copy) return -1;
    memcpy(key_copy, name, name_len + 1);
    vox_json_member_t* member = (vox_json_member_t*)vox_mpool_alloc(mpool, sizeof(vox_json_member_t));
    if (!member) {
        vox_mpool_free(mpool, key_copy);
        return -1;
    }
    vox_list_node_init(&member->node);
    member->name.ptr = key_copy;
    member->name.len = name_len;
    member->value = value_elem;
    value_elem->parent = object_elem;
    vox_list_push_back(&object_elem->u.object.list, &member->node);
    return 0;
}

int vox_json_object_remove(vox_mpool_t* mpool, vox_json_elem_t* object_elem,
                           const char* name) {
    if (!mpool || !object_elem || object_elem->type != VOX_JSON_OBJECT || !name)
        return -1;
    vox_json_member_t* member = vox_json_get_object_member(object_elem, name);
    if (!member) return -1;
    vox_list_remove(&object_elem->u.object.list, &member->node);
    /* 不释放 member->name.ptr：解析得到的对象其键指向原 buffer；构建得到的由 mpool 统一释放 */
    vox_mpool_free(mpool, member);
    return 0;
}
