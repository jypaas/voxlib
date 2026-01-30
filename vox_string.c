/*
 * vox_string.c - 高性能字符串处理实现
 * 使用 vox_mpool 内存池管理所有内存分配
 */

#include "vox_string.h"
#include "vox_mpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* 默认初始容量 */
#define VOX_STRING_DEFAULT_INITIAL_CAPACITY 32

/* 快速字符串长度计算的阈值 */
#define VOX_STRING_FAST_LEN_THRESHOLD 16

/* ===== 字符串视图实现 ===== */

vox_strview_t vox_strview_from_cstr(const char* cstr) {
    vox_strview_t sv = VOX_STRVIEW_NULL;
    if (cstr) {
        sv.ptr = cstr;
        sv.len = strlen(cstr);
    }
    return sv;
}

vox_strview_t vox_strview_from_ptr(const char* ptr, size_t len) {
    vox_strview_t sv = VOX_STRVIEW_NULL;
    if (ptr) {
        sv.ptr = ptr;
        sv.len = len;
    }
    return sv;
}

bool vox_strview_empty(const vox_strview_t* sv) {
    return !sv || !sv->ptr || sv->len == 0;
}

int vox_strview_compare(const vox_strview_t* sv1, const vox_strview_t* sv2) {
    if (!sv1 && !sv2) return 0;
    if (!sv1) return -1;
    if (!sv2) return 1;
    
    size_t min_len = sv1->len < sv2->len ? sv1->len : sv2->len;
    int result = memcmp(sv1->ptr, sv2->ptr, min_len);
    
    if (result != 0) return result;
    if (sv1->len < sv2->len) return -1;
    if (sv1->len > sv2->len) return 1;
    return 0;
}

int vox_strview_compare_cstr(const vox_strview_t* sv, const char* cstr) {
    if (!sv && !cstr) return 0;
    if (!sv) return -1;
    if (!cstr) return 1;
    
    size_t cstr_len = strlen(cstr);
    size_t min_len = sv->len < cstr_len ? sv->len : cstr_len;
    int result = memcmp(sv->ptr, cstr, min_len);
    
    if (result != 0) return result;
    if (sv->len < cstr_len) return -1;
    if (sv->len > cstr_len) return 1;
    return 0;
}

/* ===== 字符串对象实现 ===== */

/* 字符串结构 */
struct vox_string {
    vox_mpool_t* mpool;      /* 内存池 */
    char* data;               /* 字符串数据 */
    size_t length;            /* 当前长度（不包括'\0'） */
    size_t capacity;          /* 容量（包括'\0'） */
};

/* 快速计算字符串长度（优化：对于短字符串避免 strlen 的开销） */
static inline size_t fast_strlen(const char* str) {
    if (!str) return 0;
    
    /* 快速路径：检查前16个字符 */
    const char* p = str;
    
    /* 展开循环，减少分支 */
    if (p[0] == '\0') return 0;
    if (p[1] == '\0') return 1;
    if (p[2] == '\0') return 2;
    if (p[3] == '\0') return 3;
    if (p[4] == '\0') return 4;
    if (p[5] == '\0') return 5;
    if (p[6] == '\0') return 6;
    if (p[7] == '\0') return 7;
    if (p[8] == '\0') return 8;
    if (p[9] == '\0') return 9;
    if (p[10] == '\0') return 10;
    if (p[11] == '\0') return 11;
    if (p[12] == '\0') return 12;
    if (p[13] == '\0') return 13;
    if (p[14] == '\0') return 14;
    if (p[15] == '\0') return 15;
    
    /* 超过16个字符，使用标准 strlen */
    return strlen(str);
}

/* 确保容量足够 */
static inline int ensure_capacity(vox_string_t* str, size_t needed) {
    if (!str) return -1;
    
    size_t required = str->length + needed + 1;  /* +1 for '\0' */
    
    if (required <= str->capacity) {
        return 0;  /* 容量足够 */
    }
    
    /* 计算新容量（至少翻倍，优化：使用位操作加速） */
    size_t new_capacity = str->capacity;
    if (new_capacity == 0) {
        new_capacity = VOX_STRING_DEFAULT_INITIAL_CAPACITY;
    }
    
    /* 快速路径：如果只需要稍微扩容，直接翻倍 */
    if (new_capacity * 2 >= required) {
        new_capacity *= 2;
    } else {
        /* 需要多次翻倍，使用位操作优化 */
        /* 计算大于等于 required 的最小2的幂 */
        size_t target = required - 1;
        target |= target >> 1;
        target |= target >> 2;
        target |= target >> 4;
        target |= target >> 8;
        target |= target >> 16;
        #if SIZE_MAX > 0xFFFFFFFFUL
        target |= target >> 32;
        #endif
        new_capacity = target + 1;
        
        /* 检查溢出 */
        if (new_capacity < required) {
            return -1;  /* 溢出 */
        }
    }
    
    /* 分配新内存 */
    char* new_data = (char*)vox_mpool_alloc(str->mpool, new_capacity);
    if (!new_data) {
        return -1;
    }
    
    /* 复制旧数据 */
    if (str->data) {
        /* 优化：对于小字符串，使用内联复制 */
        size_t copy_len = str->length + 1;  /* 包括'\0' */
        if (copy_len <= 16) {
            const char* src = str->data;
            char* dst = new_data;
            /* 小数据块快速路径 */
            switch (copy_len) {
                case 16: dst[15] = src[15]; /* fallthrough */
                case 15: dst[14] = src[14]; /* fallthrough */
                case 14: dst[13] = src[13]; /* fallthrough */
                case 13: dst[12] = src[12]; /* fallthrough */
                case 12: dst[11] = src[11]; /* fallthrough */
                case 11: dst[10] = src[10]; /* fallthrough */
                case 10: dst[9] = src[9]; /* fallthrough */
                case 9: dst[8] = src[8]; /* fallthrough */
                case 8: dst[7] = src[7]; /* fallthrough */
                case 7: dst[6] = src[6]; /* fallthrough */
                case 6: dst[5] = src[5]; /* fallthrough */
                case 5: dst[4] = src[4]; /* fallthrough */
                case 4: dst[3] = src[3]; /* fallthrough */
                case 3: dst[2] = src[2]; /* fallthrough */
                case 2: dst[1] = src[1]; /* fallthrough */
                case 1: dst[0] = src[0]; break;
                default: break;
            }
        } else {
            memcpy(new_data, str->data, copy_len);  /* 包括'\0' */
        }
        vox_mpool_free(str->mpool, str->data);
    } else {
        new_data[0] = '\0';
    }
    
    str->data = new_data;
    str->capacity = new_capacity;
    
    return 0;
}

/* 创建空字符串 */
vox_string_t* vox_string_create(vox_mpool_t* mpool) {
    return vox_string_create_with_config(mpool, NULL);
}

/* 使用配置创建空字符串 */
vox_string_t* vox_string_create_with_config(vox_mpool_t* mpool, const vox_string_config_t* config) {
    if (!mpool) return NULL;
    
    /* 使用内存池分配字符串结构 */
    vox_string_t* str = (vox_string_t*)vox_mpool_alloc(mpool, sizeof(vox_string_t));
    if (!str) {
        return NULL;
    }
    
    /* 初始化结构 */
    memset(str, 0, sizeof(vox_string_t));
    str->mpool = mpool;
    
    /* 设置默认值 */
    size_t initial_capacity = VOX_STRING_DEFAULT_INITIAL_CAPACITY;
    
    /* 应用配置 */
    if (config && config->initial_capacity > 0) {
        initial_capacity = config->initial_capacity;
    }
    
    str->capacity = initial_capacity;
    str->length = 0;
    
    /* 分配字符串数据 */
    str->data = (char*)vox_mpool_alloc(str->mpool, str->capacity);
    if (!str->data) {
        vox_mpool_free(str->mpool, str);
        return NULL;
    }
    
    str->data[0] = '\0';
    
    return str;
}

/* 从C字符串创建 */
vox_string_t* vox_string_from_cstr(vox_mpool_t* mpool, const char* cstr) {
    if (!mpool) return NULL;
    
    if (!cstr) {
        return vox_string_create(mpool);
    }
    
    size_t len = strlen(cstr);
    return vox_string_from_data(mpool, cstr, len);
}

/* 从数据创建 */
vox_string_t* vox_string_from_data(vox_mpool_t* mpool, const void* data, size_t len) {
    if (!mpool) return NULL;
    
    vox_string_t* str = vox_string_create(mpool);
    if (!str) return NULL;
    
    if (data && len > 0) {
        if (vox_string_set_data(str, data, len) != 0) {
            vox_string_destroy(str);
            return NULL;
        }
    }
    
    return str;
}

/* 复制字符串 */
vox_string_t* vox_string_clone(vox_mpool_t* mpool, const vox_string_t* src) {
    if (!mpool || !src) return NULL;
    
    vox_string_t* dst = vox_string_create(mpool);
    if (!dst) return NULL;
    
    if (src->length > 0) {
        if (vox_string_set_data(dst, src->data, src->length) != 0) {
            vox_string_destroy(dst);
            return NULL;
        }
    }
    
    return dst;
}

/* 清空字符串 */
void vox_string_clear(vox_string_t* str) {
    if (!str) return;
    
    if (str->data) {
        str->data[0] = '\0';
    }
    str->length = 0;
}

/* 销毁字符串 */
void vox_string_destroy(vox_string_t* str) {
    if (!str) return;
    
    /* 保存内存池指针 */
    vox_mpool_t* mpool = str->mpool;
    
    /* 释放字符串数据 */
    if (str->data) {
        vox_mpool_free(mpool, str->data);
    }
    
    /* 释放字符串结构 */
    vox_mpool_free(mpool, str);
}

/* 获取长度 */
size_t vox_string_length(const vox_string_t* str) {
    return str ? str->length : 0;
}

/* 获取容量 */
size_t vox_string_capacity(const vox_string_t* str) {
    return str ? str->capacity : 0;
}

/* 检查是否为空 */
bool vox_string_empty(const vox_string_t* str) {
    return str ? (str->length == 0) : true;
}

/* 获取C字符串 */
const char* vox_string_cstr(const vox_string_t* str) {
    return str && str->data ? str->data : "";
}

/* 获取数据指针 */
const void* vox_string_data(const vox_string_t* str) {
    return str ? str->data : NULL;
}

/* 设置字符串内容 */
int vox_string_set(vox_string_t* str, const char* cstr) {
    if (!str) return -1;
    
    if (!cstr) {
        vox_string_clear(str);
        return 0;
    }
    
    /* 优化：使用快速字符串长度计算 */
    return vox_string_set_data(str, cstr, fast_strlen(cstr));
}

/* 设置字符串视图内容（优化：避免 strlen 调用） */
int vox_string_set_view(vox_string_t* str, const vox_strview_t* view) {
    if (!str) return -1;
    
    if (!view || vox_strview_empty(view)) {
        vox_string_clear(str);
        return 0;
    }
    
    return vox_string_set_data(str, view->ptr, view->len);
}

/* 设置字符串内容（指定长度） */
int vox_string_set_data(vox_string_t* str, const void* data, size_t len) {
    if (!str || !data || len == 0) {
        if (str && !data && len == 0) {
            vox_string_clear(str);
            return 0;
        }
        return -1;
    }
    
    /* 确保容量足够 */
    if (ensure_capacity(str, len) != 0) {
        return -1;
    }
    
    /* 复制数据 */
    memcpy(str->data, data, len);
    str->data[len] = '\0';
    str->length = len;
    
    return 0;
}

/* 追加C字符串 */
int vox_string_append(vox_string_t* str, const char* cstr) {
    if (!str || !cstr) return -1;
    
    /* 优化：使用快速字符串长度计算 */
    return vox_string_append_data(str, cstr, fast_strlen(cstr));
}

/* 追加字符串视图（优化：避免 strlen 调用） */
int vox_string_append_view(vox_string_t* str, const vox_strview_t* view) {
    if (!str || !view || vox_strview_empty(view)) return -1;
    
    return vox_string_append_data(str, view->ptr, view->len);
}

/* 追加数据 */
int vox_string_append_data(vox_string_t* str, const void* data, size_t len) {
    if (!str || !data || len == 0) return -1;
    
    /* 确保容量足够 */
    if (ensure_capacity(str, len) != 0) {
        return -1;
    }
    
    /* 优化：对于小数据块，使用内联复制 */
    char* dst = str->data + str->length;
    const char* src = (const char*)data;
    
    /* 小数据块快速路径（<= 16字节） */
    if (len <= 16) {
        switch (len) {
            case 16: dst[15] = src[15]; /* fallthrough */
            case 15: dst[14] = src[14]; /* fallthrough */
            case 14: dst[13] = src[13]; /* fallthrough */
            case 13: dst[12] = src[12]; /* fallthrough */
            case 12: dst[11] = src[11]; /* fallthrough */
            case 11: dst[10] = src[10]; /* fallthrough */
            case 10: dst[9] = src[9]; /* fallthrough */
            case 9: dst[8] = src[8]; /* fallthrough */
            case 8: dst[7] = src[7]; /* fallthrough */
            case 7: dst[6] = src[6]; /* fallthrough */
            case 6: dst[5] = src[5]; /* fallthrough */
            case 5: dst[4] = src[4]; /* fallthrough */
            case 4: dst[3] = src[3]; /* fallthrough */
            case 3: dst[2] = src[2]; /* fallthrough */
            case 2: dst[1] = src[1]; /* fallthrough */
            case 1: dst[0] = src[0]; break;
            default: break;
        }
    } else {
        /* 大数据块使用 memcpy */
        memcpy(dst, src, len);
    }
    
    str->length += len;
    str->data[str->length] = '\0';
    
    return 0;
}

/* 追加字符串对象 */
int vox_string_append_string(vox_string_t* str, const vox_string_t* other) {
    if (!str || !other) return -1;
    
    if (other->length == 0) return 0;
    
    /* 优化：使用字符串视图接口 */
    vox_strview_t view = vox_strview_from_ptr(other->data, other->length);
    return vox_string_append_view(str, &view);
}

/* 追加字符 */
int vox_string_append_char(vox_string_t* str, char ch) {
    if (!str) return -1;
    
    /* 确保容量足够 */
    if (ensure_capacity(str, 1) != 0) {
        return -1;
    }
    
    str->data[str->length] = ch;
    str->length++;
    str->data[str->length] = '\0';
    
    return 0;
}

/* 格式化追加 */
int vox_string_append_format(vox_string_t* str, const char* fmt, ...) {
    if (!str || !fmt) return -1;
    
    va_list args;
    va_start(args, fmt);
    int result = vox_string_append_vformat(str, fmt, args);
    va_end(args);
    
    return result;
}

/* 格式化追加（va_list版本） */
int vox_string_append_vformat(vox_string_t* str, const char* fmt, va_list args) {
    if (!str || !fmt) return -1;
    
    /* 先尝试在栈上格式化 */
    char buf[256];
    va_list args_copy;
    va_copy(args_copy, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, args_copy);
    va_end(args_copy);
    
    if (n < 0) return -1;
    
    if (n < (int)sizeof(buf)) {
        /* 栈缓冲区足够 */
        return vox_string_append_data(str, buf, n);
    }
    
    /* 需要更大的缓冲区 */
    size_t needed = (size_t)n + 1;
    if (ensure_capacity(str, needed) != 0) {
        return -1;
    }
    
    /* 直接格式化到字符串末尾 */
    /* 注意：vsnprintf 会在末尾添加 '\0'，所以可用空间需要减1 */
    size_t available = str->capacity - str->length;
    if (available == 0) {
        return -1;  /* 没有可用空间 */
    }
    n = vsnprintf(str->data + str->length, available, fmt, args);
    if (n < 0 || (size_t)n >= available) {
        return -1;  /* 格式化失败或缓冲区不足 */
    }
    
    str->length += n;
    str->data[str->length] = '\0';
    
    return n;
}

/* 在指定位置插入字符串 */
int vox_string_insert(vox_string_t* str, size_t pos, const char* cstr) {
    if (!str || !cstr) return -1;
    
    /* 优化：使用快速字符串长度计算 */
    return vox_string_insert_data(str, pos, cstr, fast_strlen(cstr));
}

/* 在指定位置插入字符串视图（优化：避免 strlen 调用） */
int vox_string_insert_view(vox_string_t* str, size_t pos, const vox_strview_t* view) {
    if (!str || !view || vox_strview_empty(view)) return -1;
    
    return vox_string_insert_data(str, pos, view->ptr, view->len);
}

/* 在指定位置插入数据 */
int vox_string_insert_data(vox_string_t* str, size_t pos, const void* data, size_t len) {
    if (!str || !data || len == 0) return -1;
    
    if (pos > str->length) return -1;
    
    /* 确保容量足够 */
    if (ensure_capacity(str, len) != 0) {
        return -1;
    }
    
    /* 将插入位置之后的字符向后移动 */
    if (pos < str->length) {
        memmove(str->data + pos + len, str->data + pos, str->length - pos);
    }
    
    /* 插入新数据 */
    memcpy(str->data + pos, data, len);
    str->length += len;
    str->data[str->length] = '\0';
    
    return 0;
}

/* 删除指定范围的字符 */
int vox_string_remove(vox_string_t* str, size_t pos, size_t len) {
    if (!str) return -1;
    
    if (pos >= str->length) return -1;
    
    if (pos + len > str->length) {
        len = str->length - pos;
    }
    
    if (len == 0) return 0;
    
    /* 将后面的字符向前移动 */
    if (pos + len < str->length) {
        memmove(str->data + pos, str->data + pos + len, str->length - pos - len);
    }
    
    str->length -= len;
    str->data[str->length] = '\0';
    
    return 0;
}

/* 获取指定位置的字符 */
char vox_string_at(const vox_string_t* str, size_t index) {
    if (!str || index >= str->length) return '\0';
    return str->data[index];
}

/* 设置指定位置的字符 */
int vox_string_set_char(vox_string_t* str, size_t index, char ch) {
    if (!str || index >= str->length) return -1;
    
    str->data[index] = ch;
    return 0;
}

/* 查找子字符串 */
size_t vox_string_find(const vox_string_t* str, const char* substr, size_t start_pos) {
    if (!str || !substr || start_pos >= str->length) {
        return SIZE_MAX;
    }
    
    const char* pos = strstr(str->data + start_pos, substr);
    if (!pos) {
        return SIZE_MAX;
    }
    
    return (size_t)(pos - str->data);
}

/* 从后往前查找子字符串 */
size_t vox_string_rfind(const vox_string_t* str, const char* substr, size_t start_pos) {
    if (!str || !substr) {
        return SIZE_MAX;
    }
    
    /* 优化：使用快速字符串长度计算 */
    size_t substr_len = fast_strlen(substr);
    if (substr_len == 0 || substr_len > str->length) {
        return SIZE_MAX;
    }
    
    size_t search_len = (start_pos == SIZE_MAX) ? str->length : start_pos + 1;
    if (search_len < substr_len) {
        return SIZE_MAX;
    }
    
    const char* data = str->data;
    for (size_t i = search_len - substr_len; i != SIZE_MAX; i--) {
        if (memcmp(data + i, substr, substr_len) == 0) {
            return i;
        }
    }
    
    return SIZE_MAX;
}

/* 替换所有匹配的子字符串 */
int vox_string_replace(vox_string_t* str, const char* old_str, const char* new_str) {
    if (!str || !old_str || !new_str) return -1;
    
    /* 优化：使用快速字符串长度计算，避免重复 strlen */
    size_t old_len = fast_strlen(old_str);
    size_t new_len = fast_strlen(new_str);
    
    if (old_len == 0) return 0;
    
    int count = 0;
    size_t pos = 0;
    
    while (true) {
        pos = vox_string_find(str, old_str, pos);
        if (pos == SIZE_MAX) {
            break;
        }
        
        if (vox_string_replace_at(str, pos, old_len, new_str) != 0) {
            return -1;
        }
        
        pos += new_len;
        count++;
    }
    
    return count;
}

/* 替换指定位置的子字符串 */
int vox_string_replace_at(vox_string_t* str, size_t pos, size_t len, const char* new_str) {
    if (!str || !new_str || pos >= str->length) return -1;
    
    if (pos + len > str->length) {
        len = str->length - pos;
    }
    
    /* 优化：使用快速字符串长度计算 */
    size_t new_len = fast_strlen(new_str);
    
    if (new_len == len) {
        /* 长度相同，直接替换 */
        memcpy(str->data + pos, new_str, new_len);
        return 0;
    }
    
    /* 长度不同，需要移动数据 */
    if (new_len > len) {
        /* 需要扩容 */
        size_t diff = new_len - len;
        if (ensure_capacity(str, diff) != 0) {
            return -1;
        }
    }
    
    /* 移动后面的数据 */
    if (new_len != len) {
        memmove(str->data + pos + new_len, 
                str->data + pos + len, 
                str->length - pos - len);
    }
    
    /* 复制新字符串 */
    memcpy(str->data + pos, new_str, new_len);
    
    str->length += (new_len - len);
    str->data[str->length] = '\0';
    
    return 0;
}

/* 比较两个字符串 */
int vox_string_compare(const vox_string_t* str1, const vox_string_t* str2) {
    if (!str1 && !str2) return 0;
    if (!str1) return -1;
    if (!str2) return 1;
    
    /* 优化：先比较长度，可以快速排除不相等的情况 */
    if (str1->length != str2->length) {
        return (str1->length < str2->length) ? -1 : 1;
    }
    
    /* 长度相同，比较内容 */
    if (str1->length == 0) return 0;
    
    return memcmp(str1->data, str2->data, str1->length);
}

/* 与C字符串比较 */
int vox_string_compare_cstr(const vox_string_t* str, const char* cstr) {
    if (!str && !cstr) return 0;
    if (!str) return -1;
    if (!cstr) return 1;
    
    /* 优化：先比较长度，可以快速排除不相等的情况 */
    size_t cstr_len = strlen(cstr);
    if (str->length != cstr_len) {
        return (str->length < cstr_len) ? -1 : 1;
    }
    
    /* 长度相同，比较内容 */
    if (str->length == 0) return 0;
    
    return memcmp(str->data, cstr, str->length);
}

/* 提取子字符串 */
vox_string_t* vox_string_substr(vox_mpool_t* mpool, const vox_string_t* str, size_t pos, size_t len) {
    if (!mpool || !str || pos >= str->length) return NULL;
    
    if (len == SIZE_MAX || pos + len > str->length) {
        len = str->length - pos;
    }
    
    return vox_string_from_data(mpool, str->data + pos, len);
}

/* 转换为小写 */
void vox_string_tolower(vox_string_t* str) {
    if (!str || !str->data) return;
    
    for (size_t i = 0; i < str->length; i++) {
        str->data[i] = (char)tolower((unsigned char)str->data[i]);
    }
}

/* 转换为大写 */
void vox_string_toupper(vox_string_t* str) {
    if (!str || !str->data) return;
    
    for (size_t i = 0; i < str->length; i++) {
        str->data[i] = (char)toupper((unsigned char)str->data[i]);
    }
}

/* 去除首尾空白字符 */
void vox_string_trim(vox_string_t* str) {
    if (!str || !str->data || str->length == 0) return;
    
    /* 找到第一个非空白字符 */
    size_t start = 0;
    while (start < str->length && isspace((unsigned char)str->data[start])) {
        start++;
    }
    
    /* 找到最后一个非空白字符 */
    size_t end = str->length;
    while (end > start && isspace((unsigned char)str->data[end - 1])) {
        end--;
    }
    
    if (start > 0 || end < str->length) {
        /* 需要移动数据 */
        if (end > start) {
            memmove(str->data, str->data + start, end - start);
        }
        str->length = end - start;
        str->data[str->length] = '\0';
    }
}

/* 预留容量 */
int vox_string_reserve(vox_string_t* str, size_t capacity) {
    if (!str || capacity <= str->capacity) return 0;
    
    /* 分配新内存 */
    char* new_data = (char*)vox_mpool_alloc(str->mpool, capacity);
    if (!new_data) {
        return -1;
    }
    
    /* 复制旧数据 */
    if (str->data) {
        memcpy(new_data, str->data, str->length + 1);
        vox_mpool_free(str->mpool, str->data);
    } else {
        new_data[0] = '\0';
    }
    
    str->data = new_data;
    str->capacity = capacity;
    
    return 0;
}

/* 调整字符串大小 */
int vox_string_resize(vox_string_t* str, size_t new_size) {
    if (!str) return -1;
    
    if (new_size == str->length) {
        return 0;
    }
    
    if (new_size < str->length) {
        /* 缩小 */
        str->length = new_size;
        str->data[str->length] = '\0';
        return 0;
    }
    
    /* 扩大 */
    if (ensure_capacity(str, new_size - str->length) != 0) {
        return -1;
    }
    
    /* 用空格填充新位置 */
    memset(str->data + str->length, ' ', new_size - str->length);
    str->length = new_size;
    str->data[str->length] = '\0';
    
    return 0;
}
