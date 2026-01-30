/*
 * vox_redis_parser.c - RESP 解析器实现
 */

#include "vox_redis_parser.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>

/* ===== 内部常量 ===== */

#define VOX_REDIS_DEFAULT_MAX_BULK_STRING_SIZE (512 * 1024 * 1024)  /* 512MB */
#define VOX_REDIS_DEFAULT_MAX_ARRAY_SIZE 1000000
#define VOX_REDIS_DEFAULT_MAX_NESTING_DEPTH 64

/* ===== 解析器状态 ===== */

typedef enum {
    VOX_REDIS_STATE_START = 0,           /* 等待类型标识符 */
    VOX_REDIS_STATE_SIMPLE_STRING,        /* 解析 Simple String */
    VOX_REDIS_STATE_ERROR,                /* 解析 Error */
    VOX_REDIS_STATE_INTEGER,              /* 解析 Integer */
    VOX_REDIS_STATE_BULK_STRING_LEN,     /* 解析 Bulk String 长度 */
    VOX_REDIS_STATE_BULK_STRING_DATA,   /* 解析 Bulk String 数据 */
    VOX_REDIS_STATE_ARRAY_COUNT,         /* 解析 Array 元素数量 */
    VOX_REDIS_STATE_ARRAY_ELEMENT,       /* 解析 Array 元素 */
    VOX_REDIS_STATE_CR,                  /* 等待 \r */
    VOX_REDIS_STATE_LF,                  /* 等待 \n */
    VOX_REDIS_STATE_COMPLETE,            /* 解析完成 */
    VOX_REDIS_STATE_ERROR_STATE          /* 错误状态 */
} vox_redis_parser_state_t;

/* ===== 解析器结构 ===== */

struct vox_redis_parser {
    vox_mpool_t* mpool;
    vox_redis_parser_callbacks_t callbacks;
    vox_redis_parser_config_t config;
    
    /* 状态 */
    vox_redis_parser_state_t state;
    vox_redis_type_t current_type;
    
    /* 嵌套数组栈 */
    struct {
        int64_t count;      /* 数组元素总数 */
        size_t current;     /* 当前元素索引 */
        int64_t expected_len; /* 当前元素的期望长度（用于bulk string） */
    } array_stack[VOX_REDIS_DEFAULT_MAX_NESTING_DEPTH];
    size_t array_depth;     /* 当前嵌套深度 */
    
    /* 当前解析的值 */
    int64_t integer_value;
    int64_t bulk_string_len;  /* -1表示NULL */
    int64_t array_count;      /* -1表示NULL */
    
    /* 字符串缓冲区（用于累积数据） */
    vox_string_t* string_buf;
    
    /* 错误处理 */
    bool has_error;
    char* error_message;
    
    /* 统计 */
    size_t bytes_parsed;
};

/* ===== 辅助函数 ===== */

static void set_error(vox_redis_parser_t* parser, const char* message) {
    if (!parser) return;
    parser->has_error = true;
    if (parser->error_message) {
        vox_mpool_free(parser->mpool, parser->error_message);
    }
    size_t len = strlen(message);
    parser->error_message = (char*)vox_mpool_alloc(parser->mpool, len + 1);
    if (parser->error_message) {
        memcpy(parser->error_message, message, len + 1);
    }
    if (parser->callbacks.on_error_parse) {
        parser->callbacks.on_error_parse((void*)parser, message);
    }
}

static int parse_integer(const char* data, size_t len, int64_t* out) {
    if (!data || len == 0 || !out) return -1;
    
    bool negative = false;
    size_t start = 0;
    
    if (data[0] == '-') {
        negative = true;
        start = 1;
    } else if (data[0] == '+') {
        start = 1;
    }
    
    if (start >= len) return -1;
    
    int64_t value = 0;
    for (size_t i = start; i < len; i++) {
        if (!isdigit((unsigned char)data[i])) {
            return -1;
        }
        int64_t digit = (int64_t)(data[i] - '0');
        if (value > (INT64_MAX - digit) / 10) {
            return -1;  /* 溢出 */
        }
        value = value * 10 + digit;
    }
    
    *out = negative ? -value : value;
    return 0;
}

static int find_crlf(const char* data, size_t len, size_t* out_pos) {
    if (!data || !out_pos) return -1;
    
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\r') {
            if (i + 1 < len && data[i + 1] == '\n') {
                *out_pos = i;
                return 0;
            }
        }
    }
    return -1;  /* 未找到 */
}

/* ===== 公共接口实现 ===== */

vox_redis_parser_t* vox_redis_parser_create(vox_mpool_t* mpool,
                                            const vox_redis_parser_config_t* config,
                                            const vox_redis_parser_callbacks_t* callbacks) {
    if (!mpool) return NULL;
    
    vox_redis_parser_t* parser = (vox_redis_parser_t*)vox_mpool_alloc(mpool, sizeof(vox_redis_parser_t));
    if (!parser) return NULL;
    
    memset(parser, 0, sizeof(vox_redis_parser_t));
    parser->mpool = mpool;
    
    /* 设置配置 */
    if (config) {
        parser->config = *config;
    } else {
        parser->config.max_bulk_string_size = VOX_REDIS_DEFAULT_MAX_BULK_STRING_SIZE;
        parser->config.max_array_size = VOX_REDIS_DEFAULT_MAX_ARRAY_SIZE;
        parser->config.max_nesting_depth = VOX_REDIS_DEFAULT_MAX_NESTING_DEPTH;
    }
    
    /* 设置回调 */
    if (callbacks) {
        memcpy(&parser->callbacks, callbacks, sizeof(vox_redis_parser_callbacks_t));
    }
    
    /* 初始化状态 */
    parser->state = VOX_REDIS_STATE_START;
    parser->array_depth = 0;
    parser->bulk_string_len = -1;
    parser->array_count = -1;
    
    /* 创建字符串缓冲区 */
    parser->string_buf = vox_string_create(parser->mpool);
    if (!parser->string_buf) {
        vox_mpool_free(mpool, parser);
        return NULL;
    }
    
    return parser;
}

void vox_redis_parser_destroy(vox_redis_parser_t* parser) {
    if (!parser) return;
    
    if (parser->error_message) {
        vox_mpool_free(parser->mpool, parser->error_message);
    }
    if (parser->string_buf) {
        vox_string_destroy(parser->string_buf);
    }
    vox_mpool_free(parser->mpool, parser);
}

void vox_redis_parser_reset(vox_redis_parser_t* parser) {
    if (!parser) return;
    
    parser->state = VOX_REDIS_STATE_START;
    parser->current_type = VOX_REDIS_TYPE_SIMPLE_STRING;
    parser->array_depth = 0;
    parser->bulk_string_len = -1;
    parser->array_count = -1;
    parser->has_error = false;
    parser->bytes_parsed = 0;
    
    if (parser->error_message) {
        vox_mpool_free(parser->mpool, parser->error_message);
        parser->error_message = NULL;
    }
    
    if (parser->string_buf) {
        vox_string_clear(parser->string_buf);
    }
}

ssize_t vox_redis_parser_execute(vox_redis_parser_t* parser, const char* data, size_t len) {
    if (!parser || !data || len == 0) {
        if (parser && parser->has_error) return -1;
        return 0;
    }
    
    if (parser->has_error || parser->state == VOX_REDIS_STATE_ERROR_STATE) {
        return -1;
    }
    
    const char* p = data;
    const char* end = data + len;
    const char* start = data;
    
    while (p < end && parser->state != VOX_REDIS_STATE_COMPLETE && 
           parser->state != VOX_REDIS_STATE_ERROR_STATE) {
        
        switch (parser->state) {
            case VOX_REDIS_STATE_START: {
                /* 等待类型标识符 */
                char ch = *p++;
                switch (ch) {
                    case '+':
                        parser->current_type = VOX_REDIS_TYPE_SIMPLE_STRING;
                        parser->state = VOX_REDIS_STATE_SIMPLE_STRING;
                        if (parser->string_buf) vox_string_clear(parser->string_buf);
                        break;
                    case '-':
                        parser->current_type = VOX_REDIS_TYPE_ERROR;
                        parser->state = VOX_REDIS_STATE_ERROR;
                        if (parser->string_buf) vox_string_clear(parser->string_buf);
                        break;
                    case ':':
                        parser->current_type = VOX_REDIS_TYPE_INTEGER;
                        parser->state = VOX_REDIS_STATE_INTEGER;
                        if (parser->string_buf) vox_string_clear(parser->string_buf);
                        break;
                    case '$':
                        parser->current_type = VOX_REDIS_TYPE_BULK_STRING;
                        parser->state = VOX_REDIS_STATE_BULK_STRING_LEN;
                        if (parser->string_buf) vox_string_clear(parser->string_buf);
                        break;
                    case '*':
                        parser->current_type = VOX_REDIS_TYPE_ARRAY;
                        parser->state = VOX_REDIS_STATE_ARRAY_COUNT;
                        if (parser->string_buf) vox_string_clear(parser->string_buf);
                        break;
                    default:
                        set_error(parser, "Invalid RESP type identifier");
                        parser->state = VOX_REDIS_STATE_ERROR_STATE;
                        return -1;
                }
                break;
            }
            
            case VOX_REDIS_STATE_SIMPLE_STRING:
            case VOX_REDIS_STATE_ERROR: {
                /* 解析直到 \r\n */
                size_t crlf_pos;
                if (find_crlf(p - 1, (size_t)(end - p + 1), &crlf_pos) == 0) {
                    size_t str_len = crlf_pos;
                    if (parser->string_buf && str_len > 0) {
                        const char* str_data = (const char*)(p - 1);
                        if (vox_string_append_data(parser->string_buf, str_data, str_len) != 0) {
                            set_error(parser, "Failed to accumulate string");
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                    }
                    
                    const char* str_ptr = parser->string_buf ? 
                        vox_string_cstr(parser->string_buf) : (p - 1);
                    size_t total_len = parser->string_buf ? 
                        vox_string_length(parser->string_buf) : str_len;
                    
                    if (parser->state == VOX_REDIS_STATE_SIMPLE_STRING) {
                        if (parser->callbacks.on_simple_string) {
                            if (parser->callbacks.on_simple_string((void*)parser, str_ptr, total_len) != 0) {
                                parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                return -1;
                            }
                        }
                    } else {
                        if (parser->callbacks.on_error) {
                            if (parser->callbacks.on_error((void*)parser, str_ptr, total_len) != 0) {
                                parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                return -1;
                            }
                        }
                    }
                    
                    p += crlf_pos + 2;  /* 跳过 \r\n */
                    
                    /* 检查是否在数组中 */
                    if (parser->array_depth > 0) {
                        parser->array_stack[parser->array_depth - 1].current++;
                        if ((int64_t)parser->array_stack[parser->array_depth - 1].current < 
                            parser->array_stack[parser->array_depth - 1].count) {
                            /* 还有更多元素 */
                            if (parser->callbacks.on_array_element_complete) {
                                size_t idx = parser->array_stack[parser->array_depth - 1].current - 1;
                                if (parser->callbacks.on_array_element_complete((void*)parser, idx) != 0) {
                                    parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                    return -1;
                                }
                            }
                            parser->state = VOX_REDIS_STATE_START;
                        } else {
                            /* 数组完成 */
                            if (parser->callbacks.on_array_element_complete) {
                                size_t idx = parser->array_stack[parser->array_depth - 1].current - 1;
                                if (parser->callbacks.on_array_element_complete((void*)parser, idx) != 0) {
                                    parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                    return -1;
                                }
                            }
                            parser->array_depth--;
                            parser->state = VOX_REDIS_STATE_COMPLETE;
                        }
                    } else {
                        parser->state = VOX_REDIS_STATE_COMPLETE;
                    }
                } else {
                    /* 未找到 \r\n，累积数据 */
                    size_t remaining = (size_t)(end - p + 1);
                    if (parser->string_buf) {
                        if (vox_string_append_data(parser->string_buf, p - 1, remaining) != 0) {
                            set_error(parser, "Failed to accumulate string");
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                    }
                    p = end;
                }
                break;
            }
            
            case VOX_REDIS_STATE_INTEGER: {
                /* 解析整数直到 \r\n */
                size_t crlf_pos;
                if (find_crlf(p, (size_t)(end - p), &crlf_pos) == 0) {
                    if (parser->string_buf && crlf_pos > 0) {
                        if (vox_string_append_data(parser->string_buf, p, crlf_pos) != 0) {
                            set_error(parser, "Failed to accumulate integer");
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                    }
                    
                    const char* int_str = parser->string_buf ? 
                        vox_string_cstr(parser->string_buf) : p;
                    size_t int_len = parser->string_buf ? 
                        vox_string_length(parser->string_buf) : crlf_pos;
                    
                    int64_t value;
                    if (parse_integer(int_str, int_len, &value) != 0) {
                        set_error(parser, "Invalid integer format");
                        parser->state = VOX_REDIS_STATE_ERROR_STATE;
                        return -1;
                    }
                    
                    if (parser->callbacks.on_integer) {
                        if (parser->callbacks.on_integer((void*)parser, value) != 0) {
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                    }
                    
                    p += crlf_pos + 2;  /* 跳过 \r\n */
                    
                    /* 检查是否在数组中 */
                    if (parser->array_depth > 0) {
                        parser->array_stack[parser->array_depth - 1].current++;
                        if ((int64_t)parser->array_stack[parser->array_depth - 1].current < 
                            parser->array_stack[parser->array_depth - 1].count) {
                            /* 还有更多元素 */
                            if (parser->callbacks.on_array_element_complete) {
                                size_t idx = parser->array_stack[parser->array_depth - 1].current - 1;
                                if (parser->callbacks.on_array_element_complete((void*)parser, idx) != 0) {
                                    parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                    return -1;
                                }
                            }
                            parser->state = VOX_REDIS_STATE_START;
                        } else {
                            /* 数组完成 */
                            if (parser->callbacks.on_array_element_complete) {
                                size_t idx = parser->array_stack[parser->array_depth - 1].current - 1;
                                if (parser->callbacks.on_array_element_complete((void*)parser, idx) != 0) {
                                    parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                    return -1;
                                }
                            }
                            parser->array_depth--;
                            parser->state = VOX_REDIS_STATE_COMPLETE;
                        }
                    } else {
                        parser->state = VOX_REDIS_STATE_COMPLETE;
                    }
                } else {
                    /* 未找到 \r\n，累积数据 */
                    size_t remaining = (size_t)(end - p);
                    if (parser->string_buf) {
                        if (vox_string_append_data(parser->string_buf, p, remaining) != 0) {
                            set_error(parser, "Failed to accumulate integer");
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                    }
                    p = end;
                }
                break;
            }
            
            case VOX_REDIS_STATE_BULK_STRING_LEN: {
                /* 解析长度直到 \r\n */
                size_t crlf_pos;
                if (find_crlf(p, (size_t)(end - p), &crlf_pos) == 0) {
                    if (parser->string_buf && crlf_pos > 0) {
                        if (vox_string_append_data(parser->string_buf, p, crlf_pos) != 0) {
                            set_error(parser, "Failed to accumulate length");
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                    }
                    
                    const char* len_str = parser->string_buf ? 
                        vox_string_cstr(parser->string_buf) : p;
                    size_t len_str_len = parser->string_buf ? 
                        vox_string_length(parser->string_buf) : crlf_pos;
                    
                    if (parse_integer(len_str, len_str_len, &parser->bulk_string_len) != 0) {
                        set_error(parser, "Invalid bulk string length");
                        parser->state = VOX_REDIS_STATE_ERROR_STATE;
                        return -1;
                    }
                    
                    if (parser->bulk_string_len == -1) {
                        /* NULL bulk string */
                        if (parser->callbacks.on_bulk_string_start) {
                            if (parser->callbacks.on_bulk_string_start((void*)parser, -1) != 0) {
                                parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                return -1;
                            }
                        }
                        if (parser->callbacks.on_bulk_string_complete) {
                            if (parser->callbacks.on_bulk_string_complete((void*)parser) != 0) {
                                parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                return -1;
                            }
                        }
                        p += crlf_pos + 2;
                        parser->state = VOX_REDIS_STATE_COMPLETE;
                    } else {
                        /* 检查长度限制 */
                        if (parser->config.max_bulk_string_size > 0 && 
                            (size_t)parser->bulk_string_len > parser->config.max_bulk_string_size) {
                            set_error(parser, "Bulk string too large");
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                        
                        if (parser->callbacks.on_bulk_string_start) {
                            if (parser->callbacks.on_bulk_string_start((void*)parser, parser->bulk_string_len) != 0) {
                                parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                return -1;
                            }
                        }
                        
                        p += crlf_pos + 2;
                        parser->state = VOX_REDIS_STATE_BULK_STRING_DATA;
                        if (parser->string_buf) vox_string_clear(parser->string_buf);
                    }
                } else {
                    /* 未找到 \r\n，累积数据 */
                    size_t remaining = (size_t)(end - p);
                    if (parser->string_buf) {
                        if (vox_string_append_data(parser->string_buf, p, remaining) != 0) {
                            set_error(parser, "Failed to accumulate length");
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                    }
                    p = end;
                }
                break;
            }
            
            case VOX_REDIS_STATE_BULK_STRING_DATA: {
                /* 读取 bulk string 数据 */
                size_t remaining = (size_t)(end - p);
                size_t needed = (size_t)parser->bulk_string_len;
                size_t to_read = remaining < needed ? remaining : needed;
                
                if (to_read > 0 && parser->callbacks.on_bulk_string_data) {
                    if (parser->callbacks.on_bulk_string_data((void*)parser, p, to_read) != 0) {
                        parser->state = VOX_REDIS_STATE_ERROR_STATE;
                        return -1;
                    }
                }
                
                p += to_read;
                parser->bulk_string_len -= (int64_t)to_read;
                
                if (parser->bulk_string_len == 0) {
                    /* 数据读取完成，等待 \r\n */
                    parser->state = VOX_REDIS_STATE_CR;
                }
                break;
            }
            
            case VOX_REDIS_STATE_CR: {
                if (*p == '\r') {
                    p++;
                    parser->state = VOX_REDIS_STATE_LF;
                } else {
                    set_error(parser, "Expected \\r after bulk string data");
                    parser->state = VOX_REDIS_STATE_ERROR_STATE;
                    return -1;
                }
                break;
            }
            
            case VOX_REDIS_STATE_LF: {
                if (*p == '\n') {
                    p++;
                    if (parser->callbacks.on_bulk_string_complete) {
                        if (parser->callbacks.on_bulk_string_complete((void*)parser) != 0) {
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                    }
                    
                    /* 检查是否在数组中 */
                    if (parser->array_depth > 0) {
                        parser->array_stack[parser->array_depth - 1].current++;
                        if ((int64_t)parser->array_stack[parser->array_depth - 1].current < 
                            parser->array_stack[parser->array_depth - 1].count) {
                            /* 还有更多元素 */
                            if (parser->callbacks.on_array_element_complete) {
                                size_t idx = parser->array_stack[parser->array_depth - 1].current - 1;
                                if (parser->callbacks.on_array_element_complete((void*)parser, idx) != 0) {
                                    parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                    return -1;
                                }
                            }
                            parser->state = VOX_REDIS_STATE_START;
                        } else {
                            /* 数组完成 */
                            if (parser->callbacks.on_array_element_complete) {
                                size_t idx = parser->array_stack[parser->array_depth - 1].current - 1;
                                if (parser->callbacks.on_array_element_complete((void*)parser, idx) != 0) {
                                    parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                    return -1;
                                }
                            }
                            parser->array_depth--;
                            parser->state = VOX_REDIS_STATE_COMPLETE;
                        }
                    } else {
                        parser->state = VOX_REDIS_STATE_COMPLETE;
                    }
                } else {
                    set_error(parser, "Expected \\n after \\r");
                    parser->state = VOX_REDIS_STATE_ERROR_STATE;
                    return -1;
                }
                break;
            }
            
            case VOX_REDIS_STATE_ARRAY_COUNT: {
                /* 解析数组元素数量直到 \r\n */
                size_t crlf_pos;
                if (find_crlf(p, (size_t)(end - p), &crlf_pos) == 0) {
                    if (parser->string_buf && crlf_pos > 0) {
                        if (vox_string_append_data(parser->string_buf, p, crlf_pos) != 0) {
                            set_error(parser, "Failed to accumulate array count");
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                    }
                    
                    const char* count_str = parser->string_buf ? 
                        vox_string_cstr(parser->string_buf) : p;
                    size_t count_str_len = parser->string_buf ? 
                        vox_string_length(parser->string_buf) : crlf_pos;
                    
                    if (parse_integer(count_str, count_str_len, &parser->array_count) != 0) {
                        set_error(parser, "Invalid array count");
                        parser->state = VOX_REDIS_STATE_ERROR_STATE;
                        return -1;
                    }
                    
                    if (parser->array_count == -1) {
                        /* NULL array */
                        if (parser->callbacks.on_array_start) {
                            if (parser->callbacks.on_array_start((void*)parser, -1) != 0) {
                                parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                return -1;
                            }
                        }
                        if (parser->callbacks.on_array_complete) {
                            if (parser->callbacks.on_array_complete((void*)parser) != 0) {
                                parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                return -1;
                            }
                        }
                        p += crlf_pos + 2;
                        parser->state = VOX_REDIS_STATE_COMPLETE;
                    } else {
                        /* 检查限制 */
                        if (parser->config.max_array_size > 0 && 
                            (size_t)parser->array_count > parser->config.max_array_size) {
                            set_error(parser, "Array too large");
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                        
                        if (parser->array_depth >= parser->config.max_nesting_depth) {
                            set_error(parser, "Array nesting too deep");
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                        
                        if (parser->callbacks.on_array_start) {
                            if (parser->callbacks.on_array_start((void*)parser, parser->array_count) != 0) {
                                parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                return -1;
                            }
                        }
                        
                        /* 初始化数组栈 */
                        parser->array_stack[parser->array_depth].count = parser->array_count;
                        parser->array_stack[parser->array_depth].current = 0;
                        parser->array_depth++;
                        
                        p += crlf_pos + 2;
                        
                        if (parser->array_count == 0) {
                            /* 空数组 */
                            parser->array_depth--;
                            if (parser->callbacks.on_array_complete) {
                                if (parser->callbacks.on_array_complete((void*)parser) != 0) {
                                    parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                    return -1;
                                }
                            }
                            parser->state = VOX_REDIS_STATE_COMPLETE;
                        } else {
                            /* 开始解析第一个元素 */
                            if (parser->callbacks.on_array_element_start) {
                                if (parser->callbacks.on_array_element_start((void*)parser, 0) != 0) {
                                    parser->state = VOX_REDIS_STATE_ERROR_STATE;
                                    return -1;
                                }
                            }
                            parser->state = VOX_REDIS_STATE_START;
                        }
                        
                        if (parser->string_buf) vox_string_clear(parser->string_buf);
                    }
                } else {
                    /* 未找到 \r\n，累积数据 */
                    size_t remaining = (size_t)(end - p);
                    if (parser->string_buf) {
                        if (vox_string_append_data(parser->string_buf, p, remaining) != 0) {
                            set_error(parser, "Failed to accumulate array count");
                            parser->state = VOX_REDIS_STATE_ERROR_STATE;
                            return -1;
                        }
                    }
                    p = end;
                }
                break;
            }
            
            case VOX_REDIS_STATE_ARRAY_ELEMENT:
            case VOX_REDIS_STATE_COMPLETE:
            case VOX_REDIS_STATE_ERROR_STATE:
                /* 这些状态不应该在这里 */
                break;
        }
    }
    
    /* 处理数组元素完成后的状态转换 */
    if (parser->state == VOX_REDIS_STATE_COMPLETE && parser->array_depth > 0) {
        /* 检查是否所有元素都解析完成 */
        if ((int64_t)parser->array_stack[parser->array_depth - 1].current >= 
            parser->array_stack[parser->array_depth - 1].count) {
            parser->array_depth--;
            if (parser->callbacks.on_array_complete) {
                if (parser->callbacks.on_array_complete((void*)parser) != 0) {
                    parser->state = VOX_REDIS_STATE_ERROR_STATE;
                    return -1;
                }
            }
        }
    }
    
    if (parser->state == VOX_REDIS_STATE_COMPLETE) {
        if (parser->callbacks.on_complete) {
            if (parser->callbacks.on_complete((void*)parser) != 0) {
                parser->state = VOX_REDIS_STATE_ERROR_STATE;
                return -1;
            }
        }
    }
    
    parser->bytes_parsed += (size_t)(p - start);
    
    if (parser->state == VOX_REDIS_STATE_ERROR_STATE) {
        return -1;
    }
    
    return (ssize_t)(p - start);
}

bool vox_redis_parser_is_complete(const vox_redis_parser_t* parser) {
    if (!parser) return false;
    return parser->state == VOX_REDIS_STATE_COMPLETE;
}

bool vox_redis_parser_has_error(const vox_redis_parser_t* parser) {
    if (!parser) return false;
    return parser->has_error || parser->state == VOX_REDIS_STATE_ERROR_STATE;
}

const char* vox_redis_parser_get_error(const vox_redis_parser_t* parser) {
    if (!parser || !parser->has_error) return NULL;
    return parser->error_message;
}

void* vox_redis_parser_get_user_data(const vox_redis_parser_t* parser) {
    if (!parser) return NULL;
    return parser->callbacks.user_data;
}

void vox_redis_parser_set_user_data(vox_redis_parser_t* parser, void* user_data) {
    if (!parser) return;
    parser->callbacks.user_data = user_data;
}
