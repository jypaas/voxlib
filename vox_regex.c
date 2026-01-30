/*
 * vox_regex.c - 高性能正则表达式引擎实现
 * 使用NFA（非确定性有限自动机）和Thompson构造算法
 */

#include "vox_regex.h"
#include "vox_os.h"
#include <string.h>
#include <stdlib.h>

/* ===== 内部数据结构 ===== */

/* NFA状态类型 */
typedef enum {
    NFA_STATE_MATCH = 0,      /* 匹配状态（接受状态） */
    NFA_STATE_CHAR,           /* 字符匹配 */
    NFA_STATE_SPLIT,         /* 分割（epsilon转换） */
    NFA_STATE_CHARSET,       /* 字符集匹配 */
    NFA_STATE_ANCHOR_START,  /* 行首锚点 ^ */
    NFA_STATE_ANCHOR_END,    /* 行尾锚点 $ */
    NFA_STATE_WORD_BOUNDARY, /* 词边界 \b */
    NFA_STATE_LOOKAHEAD_POS,  /* 正向先行断言 (?=) */
    NFA_STATE_LOOKAHEAD_NEG,  /* 负向先行断言 (?!) */
    NFA_STATE_LOOKBEHIND_POS, /* 正向后行断言 (?<=) */
    NFA_STATE_LOOKBEHIND_NEG  /* 负向后行断言 (?<!) */
} nfa_state_type_t;

/* NFA状态结构 */
typedef struct nfa_state {
    nfa_state_type_t type;    /* 状态类型 */
    union {
        char ch;              /* 字符（用于CHAR类型） */
        struct {
            uint8_t bitmap[32];  /* 字符集位图（256个字符） */
        } charset;            /* 字符集（用于CHARSET类型） */
        struct {
            struct nfa_state* start; /* 断言子NFA起始状态 */
        } assertion;          /* 断言（用于LOOKAHEAD/LOOKBEHIND类型） */
    } u;
    struct nfa_state* out1;   /* 第一个出边 */
    struct nfa_state* out2;   /* 第二个出边（用于SPLIT） */
    int group_id;             /* 捕获组ID（-1表示非捕获组） */
    bool group_start;         /* 是否为捕获组开始 */
    bool non_greedy;          /* 非贪婪匹配标记（用于量词） */
    int id;                   /* 状态唯一ID（优化去重性能） */
} nfa_state_t;

/* NFA片段（用于构建NFA） */
typedef struct {
    nfa_state_t* start;       /* 起始状态 */
    nfa_state_t* end;         /* 结束状态 */
} nfa_fragment_t;

/* 正则表达式对象 */
struct vox_regex {
    vox_mpool_t* mpool;       /* 内存池 */
    nfa_state_t* start;       /* NFA起始状态 */
    int flags;                /* 编译选项 */
    int group_count;          /* 捕获组数量 */
    char* pattern;             /* 原始模式字符串 */
    bool has_non_greedy;       /* 是否包含非贪婪量词 */
    int state_count;           /* 状态总数 */
    char* prefix;              /* 字面量前缀 */
    size_t prefix_len;         /* 前缀长度 */
};

/* 匹配状态（用于NFA模拟） */
typedef struct {
    nfa_state_t* state;       /* NFA状态 */
    size_t* group_starts;     /* 捕获组开始位置 */
    size_t* group_ends;       /* 捕获组结束位置 */
} match_state_t;

/* 状态列表（用于NFA模拟） */
typedef struct {
    match_state_t* states;    /* 状态数组 */
    size_t count;             /* 状态数量 */
    size_t capacity;          /* 数组容量 */
} state_list_t;

/* 匹配上下文（复用缓冲区以提升性能） */
typedef struct {
    vox_mpool_t* mpool;
    uint32_t* visited;
    uint32_t generation;
    state_list_t current_list;
    state_list_t next_list;
    size_t* g_starts;
    size_t* g_ends;
    int state_count;
    int group_count;
} match_context_t;

/* ===== 辅助函数 ===== */

/* 扩展状态列表数组 */
static inline bool ensure_state_list_capacity(state_list_t* list, vox_mpool_t* mpool) {
    if (list->count >= list->capacity) {
        size_t new_capacity = (list->capacity == 0) ? 32 : (list->capacity * 2);
        match_state_t* new_states = (match_state_t*)vox_mpool_realloc(mpool, list->states, sizeof(match_state_t) * new_capacity);
        if (!new_states) return false;
        list->states = new_states;
        list->capacity = new_capacity;
    }
    return true;
}

/* 创建匹配上下文 */
static match_context_t* create_match_context(vox_regex_t* regex) {
    vox_mpool_t* mpool = regex->mpool;
    
    /* 优化：单次分配 ctx + visited + g_starts + g_ends */
    size_t visited_size = sizeof(uint32_t) * regex->state_count;
    size_t groups_size = (regex->group_count > 0) ? (sizeof(size_t) * (regex->group_count + 1) * 2) : 0;
    size_t total_size = sizeof(match_context_t) + visited_size + groups_size;
    
    match_context_t* ctx = (match_context_t*)vox_mpool_alloc(mpool, total_size);
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(match_context_t));
    
    ctx->mpool = mpool;
    ctx->state_count = regex->state_count;
    ctx->group_count = regex->group_count;
    
    char* ptr = (char*)ctx + sizeof(match_context_t);
    ctx->visited = (uint32_t*)ptr;
    memset(ctx->visited, 0, visited_size);
    ptr += visited_size;
    
    if (regex->group_count > 0) {
        ctx->g_starts = (size_t*)ptr;
        ptr += sizeof(size_t) * (regex->group_count + 1);
        ctx->g_ends = (size_t*)ptr;
    }
    
    ctx->generation = 1;
    return ctx;
}

/* 释放匹配上下文 */
static void free_match_context(match_context_t* ctx) {
    if (!ctx) return;
    if (ctx->current_list.states) vox_mpool_free(ctx->mpool, ctx->current_list.states);
    if (ctx->next_list.states) vox_mpool_free(ctx->mpool, ctx->next_list.states);
    vox_mpool_free(ctx->mpool, ctx);
}

/* 添加状态到列表（不检查重复，由调用者保证） */
static inline void append_state_to_list(state_list_t* list, nfa_state_t* state,
                                        size_t* group_starts, size_t* group_ends, vox_mpool_t* mpool) {
    if (!ensure_state_list_capacity(list, mpool)) return;
    
    list->states[list->count].state = state;
    list->states[list->count].group_starts = group_starts;
    list->states[list->count].group_ends = group_ends;
    list->count++;
}

/* 转换为小写（如果忽略大小写） */
static inline char to_lower_if_needed(char ch, bool ignore_case) {
    if (ignore_case && ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 'a';
    }
    return ch;
}

/* 安全复制数据到输出缓冲区（带边界检查） */
static inline int safe_copy_to_output(char* output, size_t* out_pos, size_t output_size,
                                      const char* src, size_t len) {
    if (*out_pos + len >= output_size) {
        return -1;  /* 缓冲区溢出 */
    }
    memcpy(output + *out_pos, src, len);
    *out_pos += len;
    return 0;
}

/* 设置字符集位 */
static inline void set_charset_bit(uint8_t* bitmap, int bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

/* 获取字符集位 */
static inline bool get_charset_bit(const uint8_t* bitmap, int bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

/* 初始化字符集 */
static inline void init_charset(uint8_t* bitmap) {
    memset(bitmap, 0, 32);
}

/* 添加字符到字符集 */
static inline void add_char_to_charset(uint8_t* bitmap, char ch) {
    set_charset_bit(bitmap, (unsigned char)ch);
}

/* 添加字符范围到字符集 */
static void add_range_to_charset(uint8_t* bitmap, char start, char end) {
    unsigned char s = (unsigned char)start;
    unsigned char e = (unsigned char)end;
    if (s > e) {
        unsigned char tmp = s;
        s = e;
        e = tmp;
    }
    for (unsigned char ch = s; ch <= e; ch++) {
        set_charset_bit(bitmap, ch);
    }
}

/* 检查字符是否在字符集中 */
static bool char_in_charset(const uint8_t* bitmap, char ch) {
    return get_charset_bit(bitmap, (unsigned char)ch);
}

/* 判断字符是否为单词字符 */
static bool is_word_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || 
           (ch >= 'A' && ch <= 'Z') || 
           (ch >= '0' && ch <= '9') || 
           ch == '_';
}

/* 创建NFA状态 */
static nfa_state_t* create_nfa_state(vox_mpool_t* mpool, nfa_state_type_t type) {
    nfa_state_t* state = (nfa_state_t*)vox_mpool_alloc(mpool, sizeof(nfa_state_t));
    if (!state) return NULL;
    
    memset(state, 0, sizeof(nfa_state_t));
    state->type = type;
    state->out1 = NULL;
    state->out2 = NULL;
    state->group_id = -1;
    state->group_start = false;
    state->non_greedy = false;
    state->id = -1;  /* 初始化为-1表示未分配ID */
    
    return state;
}

/* 添加epsilon转换 */
VOX_UNUSED_FUNC static void add_epsilon_transition(nfa_state_t* from, nfa_state_t* to) {
    if (from->out1 == NULL) {
        from->out1 = to;
    } else if (from->out2 == NULL) {
        from->out2 = to;
    }
}

/* ===== 字符类处理 ===== */

/* 前向声明 */
static bool parse_escape_sequence(vox_mpool_t *mpool, const char **pattern,
                                  uint8_t *charset, bool *is_charset);

/* 解析字符类（如[abc]或[^abc]） */
static bool parse_char_class(vox_mpool_t *mpool, const char **pattern,
                             uint8_t *charset, bool ignore_case) {
    VOX_UNUSED(mpool);
    const char* p = *pattern;
    bool negated = false;
    
    if (*p == '^') {
        negated = true;
        p++;
    }
    
    init_charset(charset);
    
    while (*p != '\0' && *p != ']') {
        if (*p == '\\' && p[1] != '\0') {
            /* 转义序列 */
            const char* escape_ptr = p;  /* 指向\ */
            uint8_t escape_charset[32];
            bool is_charset_escape = false;
            
            /* 尝试解析转义序列（传递指向\的指针） */
            if (parse_escape_sequence(mpool, &escape_ptr, escape_charset, &is_charset_escape)) {
                if (is_charset_escape) {
                    /* 转义序列返回字符集，合并到当前字符集 */
                    for (int i = 0; i < 32; i++) {
                        charset[i] |= escape_charset[i];
                    }
                    p = escape_ptr;  /* parse_escape_sequence 已经更新了指针 */
                } else {
                    /* 普通转义字符 */
                    p++;  /* 跳过\ */
                    char ch = *p;
                    if (ignore_case && ch >= 'a' && ch <= 'z') {
                        ch = ch - 'a' + 'A';
                        add_char_to_charset(charset, ch);
                        add_char_to_charset(charset, ch - 'A' + 'a');
                    } else {
                        add_char_to_charset(charset, ch);
                    }
                    p++;
                }
            } else {
                /* 解析失败，作为普通字符处理 */
                p++;  /* 跳过\ */
                char ch = *p;
                if (ch == '\0') break;  /* 字符串结束 */
                if (ignore_case && ch >= 'a' && ch <= 'z') {
                    ch = ch - 'a' + 'A';
                    add_char_to_charset(charset, ch);
                    add_char_to_charset(charset, ch - 'A' + 'a');
                } else {
                    add_char_to_charset(charset, ch);
                }
                p++;
            }
        } else if (p[1] == '-' && p[2] != '\0' && p[2] != ']' && p[2] != '\\') {
            /* 字符范围（注意：如果范围结束符是转义序列，需要特殊处理） */
            char start = *p;
            char end = p[2];
            if (ignore_case) {
                if (start >= 'a' && start <= 'z') start = start - 'a' + 'A';
                if (end >= 'a' && end <= 'z') end = end - 'a' + 'A';
            }
            add_range_to_charset(charset, start, end);
            if (ignore_case && start >= 'A' && start <= 'Z') {
                add_range_to_charset(charset, start - 'A' + 'a', end - 'A' + 'a');
            }
            p += 3;
        } else {
            /* 单个字符 */
            char ch = *p;
            if (ignore_case && ch >= 'a' && ch <= 'z') {
                ch = ch - 'a' + 'A';
                add_char_to_charset(charset, ch);
                add_char_to_charset(charset, ch - 'A' + 'a');
            } else {
                add_char_to_charset(charset, ch);
            }
            p++;
        }
    }
    
    if (*p != ']') {
        return false;  /* 未找到结束的] */
    }
    
    *pattern = p + 1;
    
    if (negated) {
        /* 取反 */
        for (int i = 0; i < 32; i++) {
            charset[i] = ~charset[i];
        }
    }
    
    return true;
}

/* 解析转义序列（用于字符类内部和外部） */
static bool parse_escape_sequence(vox_mpool_t *mpool, const char **pattern,
                                  uint8_t *charset, bool *is_charset) {
    VOX_UNUSED(mpool);
    const char* p = *pattern;
    
    /* 注意：如果从字符类内部调用，p已经指向转义字符后的字符 */
    /* 如果从外部调用，p指向\ */
    if (*p == '\\') {
        p++;
        if (*p == '\0') {
            *is_charset = false;
            return false;
        }
    }
    
    init_charset(charset);
    *is_charset = true;
    
    switch (*p) {
        case 'd':  /* 数字 */
            add_range_to_charset(charset, '0', '9');
            break;
        case 'D':  /* 非数字 */
            for (int i = 0; i < 256; i++) {
                if (i < '0' || i > '9') {
                    set_charset_bit(charset, i);
                }
            }
            break;
        case 'w':  /* 单词字符 */
            add_range_to_charset(charset, 'a', 'z');
            add_range_to_charset(charset, 'A', 'Z');
            add_range_to_charset(charset, '0', '9');
            add_char_to_charset(charset, '_');
            break;
        case 'W':  /* 非单词字符 */
            for (int i = 0; i < 256; i++) {
                if (!((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || 
                      (i >= '0' && i <= '9') || i == '_')) {
                    set_charset_bit(charset, i);
                }
            }
            break;
        case 's':  /* 空白字符 */
            add_char_to_charset(charset, ' ');
            add_char_to_charset(charset, '\t');
            add_char_to_charset(charset, '\n');
            add_char_to_charset(charset, '\r');
            add_char_to_charset(charset, '\v');
            add_char_to_charset(charset, '\f');
            break;
        case 'S':  /* 非空白字符 */
            for (int i = 0; i < 256; i++) {
                if (i != ' ' && i != '\t' && i != '\n' && 
                    i != '\r' && i != '\v' && i != '\f') {
                    set_charset_bit(charset, i);
                }
            }
            break;
        default:
            /* 普通转义字符 */
            add_char_to_charset(charset, *p);
            break;
    }
    
    /* 更新pattern指针 */
    *pattern = p + 1;  /* 跳过转义字符 */
    return true;
}

/* ===== NFA构建 ===== */

/* 复制NFA片段（用于量词） */
/* 注意：此函数只复制start和end状态，适用于简单的原子片段（如单个字符、字符类等） */
/* 对于复杂的片段（包含多个中间状态），需要完整的深度复制，但当前使用场景不需要 */
static nfa_fragment_t clone_fragment(vox_mpool_t* mpool, nfa_fragment_t frag) {
    nfa_fragment_t new_frag = { NULL, NULL };
    
    if (!frag.start) {
        return new_frag;
    }
    
    /* 复制start状态 */
    nfa_state_t* new_start = create_nfa_state(mpool, frag.start->type);
    if (!new_start) return new_frag;
    
    /* 复制状态内容 */
    memcpy(new_start, frag.start, sizeof(nfa_state_t));
    /* 保留out1和out2的原始连接，因为它们指向frag的内部状态 */
    /* 注意：对于简单的原子片段，start->out1通常直接指向end */
    
    /* 复制end状态（如果存在） */
    nfa_state_t* new_end = NULL;
    if (frag.end) {
        new_end = create_nfa_state(mpool, frag.end->type);
        if (!new_end) {
            return new_frag;
        }
        memcpy(new_end, frag.end, sizeof(nfa_state_t));
        new_end->out1 = NULL;
        new_end->out2 = NULL;
    }
    
    /* 连接：如果原始frag中start->out1指向end，则新frag中也应该这样 */
    if (frag.start->out1 == frag.end && new_end) {
        new_start->out1 = new_end;
    } else {
        /* 对于复杂片段，保持原始连接（但这不是完整的复制） */
        /* 当前使用场景（量词）中，frag通常是简单原子，所以这是安全的 */
        new_start->out1 = frag.start->out1;
    }
    new_start->out2 = NULL;
    
    new_frag.start = new_start;
    new_frag.end = new_end;
    
    return new_frag;
}

/* 前向声明 - 必须在nfa_fragment_t定义之后 */
static nfa_fragment_t parse_expr(vox_mpool_t* mpool, const char** pattern,
                                 int flags, int* group_id);
static nfa_fragment_t parse_term(vox_mpool_t* mpool, const char** pattern,
                                 int flags, int* group_id);
static nfa_fragment_t parse_factor(vox_mpool_t* mpool, const char** pattern,
                                   int flags, int* group_id);
static nfa_fragment_t parse_atom(vox_mpool_t* mpool, const char** pattern,
                                 int flags, int* group_id);

/* 解析单个字符或字符类，返回NFA片段 */
static nfa_fragment_t parse_atom(vox_mpool_t *mpool, const char **pattern,
                                 int flags, int *group_id) {
    nfa_fragment_t frag = { NULL, NULL };
    const char* p = *pattern;
    bool ignore_case = (flags & VOX_REGEX_IGNORE_CASE) != 0;
    
    if (*p == '\0') {
        return frag;
    }
    
    if (*p == '(') {
        /* 捕获组、非捕获组或断言 */
        p++;
        
        bool non_capturing = false;
        nfa_state_type_t assertion_type = NFA_STATE_MATCH;
        bool is_assertion = false;
        
        if (*p == '?' && p[1] == ':') {
            /* 非捕获组 (?:pattern) */
            non_capturing = true;
            p += 2;
        } else if (*p == '?' && p[1] == '=') {
            /* 正向先行断言 (?=pattern) */
            is_assertion = true;
            assertion_type = NFA_STATE_LOOKAHEAD_POS;
            p += 2;
        } else if (*p == '?' && p[1] == '!') {
            /* 负向先行断言 (?!pattern) */
            is_assertion = true;
            assertion_type = NFA_STATE_LOOKAHEAD_NEG;
            p += 2;
        } else if (*p == '?' && p[1] == '<' && p[2] == '=') {
            /* 正向后行断言 (?<=pattern) */
            is_assertion = true;
            assertion_type = NFA_STATE_LOOKBEHIND_POS;
            p += 3;
        } else if (*p == '?' && p[1] == '<' && p[2] == '!') {
            /* 负向后行断言 (?<!pattern) */
            is_assertion = true;
            assertion_type = NFA_STATE_LOOKBEHIND_NEG;
            p += 3;
        }
        
        if (is_assertion) {
            /* 解析断言内部的表达式 */
            nfa_fragment_t sub_frag = parse_expr(mpool, &p, flags, NULL);
            if (sub_frag.start == NULL || *p != ')') {
                return frag;
            }
            p++;
            
            /* 为断言子NFA添加结束匹配状态 */
            nfa_state_t* sub_match = create_nfa_state(mpool, NFA_STATE_MATCH);
            if (!sub_match) return frag;
            
            if (sub_frag.end) {
                if (sub_frag.end->type == NFA_STATE_SPLIT) {
                    sub_frag.end->out1 = sub_match;
                } else {
                    nfa_state_t* split = create_nfa_state(mpool, NFA_STATE_SPLIT);
                    if (!split) return frag;
                    split->out1 = sub_match;
                    sub_frag.end->out1 = split;
                }
            } else if (sub_frag.start) {
                /* 如果没有明确的结束状态，尝试连接开始状态 */
                if (sub_frag.start->out1 == NULL) {
                    sub_frag.start->out1 = sub_match;
                } else if (sub_frag.start->out2 == NULL && sub_frag.start->type == NFA_STATE_SPLIT) {
                    sub_frag.start->out2 = sub_match;
                }
            }
            
            /* 创建断言状态 */
            nfa_state_t* state = create_nfa_state(mpool, assertion_type);
            if (!state) return frag;
            state->u.assertion.start = sub_frag.start;
            
            /* 创建结束状态 */
            nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
            if (!end) return frag;
            
            state->out1 = end;
            frag.start = state;
            frag.end = end;
            *pattern = p;
            return frag;
        }
        
        /* 分配组ID（如果是捕获组） */
        int current_group_id = -1;
        if (!non_capturing && group_id) {
            current_group_id = (*group_id)++;
        }
        
        /* 递归解析组内的表达式 */
        nfa_fragment_t group_frag = parse_expr(mpool, &p, flags, group_id);
        if (group_frag.start == NULL) {
            return frag;  /* 解析失败 */
        }
        
        if (*p != ')') {
            return frag;  /* 缺少结束的) */
        }
        p++;
        
        /* 标记组的开始和结束状态 */
        if (current_group_id >= 0) {
            /* 标记组开始 */
            nfa_state_t* group_start_marker = create_nfa_state(mpool, NFA_STATE_SPLIT);
            if (!group_start_marker) return frag;
            group_start_marker->group_id = current_group_id;
            group_start_marker->group_start = true;
            group_start_marker->out1 = group_frag.start;
            group_start_marker->out2 = NULL;
            
            /* 标记组结束 */
            if (group_frag.end) {
                group_frag.end->group_id = current_group_id;
                group_frag.end->group_start = false;
                frag.end = group_frag.end;
            } else {
                /* 如果group_frag.end为NULL，创建一个结束状态 */
                nfa_state_t* group_end = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!group_end) return frag;
                group_end->out1 = NULL;
                group_end->out2 = NULL;
                group_end->group_id = current_group_id;
                group_end->group_start = false;
                if (group_frag.start) {
                    group_frag.start->out1 = group_end;
                }
                frag.end = group_end;
            }
            
            frag.start = group_start_marker;
        } else {
            /* 非捕获组，直接使用 */
            frag = group_frag;
        }
        
        *pattern = p;
        return frag;
    } else if (*p == '^') {
        /* 行首锚点 */
        nfa_state_t* state = create_nfa_state(mpool, NFA_STATE_ANCHOR_START);
        if (!state) return frag;
        
        nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!end) return frag;
        end->out1 = NULL;
        end->out2 = NULL;
        
        state->out1 = end;
        frag.start = state;
        frag.end = end;
        *pattern = p + 1;
        return frag;
    } else if (*p == '$') {
        /* 行尾锚点 */
        nfa_state_t* state = create_nfa_state(mpool, NFA_STATE_ANCHOR_END);
        if (!state) return frag;
        
        nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!end) return frag;
        end->out1 = NULL;
        end->out2 = NULL;
        
        state->out1 = end;
        frag.start = state;
        frag.end = end;
        *pattern = p + 1;
        return frag;
    } else if (*p == '.') {
        /* 匹配任意字符 */
        nfa_state_t* state = create_nfa_state(mpool, NFA_STATE_CHARSET);
        if (!state) return frag;
        
        init_charset(state->u.charset.bitmap);
        if (flags & VOX_REGEX_DOTALL) {
            /* 匹配所有字符包括换行符 */
            for (int i = 0; i < 256; i++) {
                set_charset_bit(state->u.charset.bitmap, i);
            }
        } else {
            /* 匹配除换行符外的所有字符 */
            for (int i = 0; i < 256; i++) {
                if (i != '\n' && i != '\r') {
                    set_charset_bit(state->u.charset.bitmap, i);
                }
            }
        }
        
        /* 创建一个中间状态作为end（不是匹配状态） */
        nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!end) return frag;
        end->out1 = NULL;  /* 暂时为空，连接时会设置 */
        end->out2 = NULL;
        
        state->out1 = end;
        frag.start = state;
        frag.end = end;
        *pattern = p + 1;
        return frag;
    } else if (*p == '[') {
        /* 字符类 */
        p++;
        uint8_t charset[32];
        if (!parse_char_class(mpool, &p, charset, ignore_case)) {
            return frag;
        }
        
        nfa_state_t* state = create_nfa_state(mpool, NFA_STATE_CHARSET);
        if (!state) return frag;
        
        memcpy(state->u.charset.bitmap, charset, 32);
        
        /* 创建一个中间状态作为end */
        nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!end) return frag;
        end->out1 = NULL;
        end->out2 = NULL;
        
        state->out1 = end;
        frag.start = state;
        frag.end = end;
        *pattern = p;
        return frag;
    } else if (*p == '\\') {
        /* 转义序列 */
        p++;  /* 跳过 \ */
        if (*p == '\0') return frag;
        
        if (*p == 'b') {
            /* 词边界 \b */
            nfa_state_t* state = create_nfa_state(mpool, NFA_STATE_WORD_BOUNDARY);
            if (!state) return frag;
            
            nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
            if (!end) return frag;
            end->out1 = NULL;
            end->out2 = NULL;
            
            state->out1 = end;
            frag.start = state;
            frag.end = end;
            *pattern = p + 1;
            return frag;
        }
        
        uint8_t charset[32];
        bool is_charset = false;
        const char* save_p = p;
        
        if (parse_escape_sequence(mpool, &save_p, charset, &is_charset)) {
            if (is_charset) {
                /* 字符类转义 */
                nfa_state_t* state = create_nfa_state(mpool, NFA_STATE_CHARSET);
                if (!state) return frag;
                
                memcpy(state->u.charset.bitmap, charset, 32);
                
                nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!end) return frag;
                end->out1 = NULL;
                end->out2 = NULL;
                
                state->out1 = end;
                frag.start = state;
                frag.end = end;
                *pattern = save_p;
                return frag;
            }
        }
        
        /* 普通转义字符 */
        char ch = *p;
        /* 注意：不在编译时转换大小写，而是在匹配时处理 */
        
        nfa_state_t* state = create_nfa_state(mpool, NFA_STATE_CHAR);
        if (!state) return frag;
        
        state->u.ch = ch;
        
        nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!end) return frag;
        end->out1 = NULL;
        end->out2 = NULL;
        
        state->out1 = end;
        frag.start = state;
        frag.end = end;
        *pattern = p + 1;
        return frag;
    } else {
        /* 普通字符 */
        char ch = *p;
        /* 注意：不在编译时转换大小写，而是在匹配时处理 */
        
        nfa_state_t* state = create_nfa_state(mpool, NFA_STATE_CHAR);
        if (!state) return frag;
        
        state->u.ch = ch;
        
        nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!end) return frag;
        end->out1 = NULL;
        end->out2 = NULL;
        
        state->out1 = end;
        frag.start = state;
        frag.end = end;
        *pattern = p + 1;
        return frag;
    }
}

/* 解析量词（*, +, ?, {n,m}） */
static nfa_fragment_t apply_quantifier(vox_mpool_t* mpool, nfa_fragment_t frag,
                                       const char** pattern) {
    const char* p = *pattern;
    bool non_greedy = false;
    
    /* 检查是否有非贪婪标记 ? */
    if (p[1] == '?') {
        non_greedy = true;
    }
    
    if (*p == '*') {
        /* 0次或多次 */
        nfa_state_t* split = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!split) return frag;
        
        nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!end) return frag;
        end->out1 = NULL;
        end->out2 = NULL;
        
        split->non_greedy = non_greedy;
        if (non_greedy) {
            /* 非贪婪：优先尝试不匹配（out1），然后尝试匹配（out2） */
            split->out1 = end;      /* 不匹配，直接结束 */
            split->out2 = frag.start; /* 匹配，继续 */
            if (frag.end) {
                frag.end->out1 = split;  /* 循环回split */
                frag.end->out2 = end;    /* 或者直接到end */
            }
        } else {
            /* 贪婪：优先尝试匹配（out1），然后尝试不匹配（out2） */
            split->out1 = frag.start;
            split->out2 = end;
            if (frag.end) {
                frag.end->out1 = split;  /* 循环回split */
                frag.end->out2 = end;    /* 或者直接到end */
            } else if (frag.start) {
                /* 如果frag.end为NULL，但frag.start存在，直接连接 */
                frag.start->out1 = split;
            }
        }
        
        frag.start = split;
        frag.end = end;
        *pattern = p + (non_greedy ? 2 : 1);  /* 跳过*或*? */
    } else if (*p == '+') {
        /* 1次或多次 */
        nfa_state_t* split = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!split) return frag;
        
        nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!end) return frag;
        end->out1 = NULL;
        end->out2 = NULL;
        
        split->non_greedy = non_greedy;
        
        if (non_greedy) {
            /* 非贪婪：必须先匹配一次，然后优先尝试不匹配 */
            split->out1 = frag.start;  /* 必须匹配 */
            if (frag.end) {
                frag.end->non_greedy = non_greedy;
                frag.end->out1 = end;      /* 不匹配，直接结束 */
                frag.end->out2 = split;    /* 匹配，继续循环 */
            }
        } else {
            /* 贪婪：必须先匹配一次，然后优先尝试匹配 */
            split->out1 = frag.start;
            if (frag.end) {
                frag.end->non_greedy = non_greedy;
                frag.end->out1 = split;  /* 循环回split */
                frag.end->out2 = end;    /* 或者直接到end */
            }
        }
        
        frag.start = split;
        frag.end = end;
        *pattern = p + (non_greedy ? 2 : 1);  /* 跳过+或+? */
    } else if (*p == '?') {
        /* 0次或1次 */
        nfa_state_t* split = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!split) return frag;
        
        nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!end) return frag;
        end->out1 = NULL;
        end->out2 = NULL;
        
        split->non_greedy = non_greedy;
        if (non_greedy) {
            /* 非贪婪：优先尝试不匹配（out1），然后尝试匹配（out2） */
            split->out1 = end;        /* 不匹配，直接结束 */
            split->out2 = frag.start; /* 匹配，继续 */
            if (frag.end) {
                frag.end->out1 = end;
            }
        } else {
            /* 贪婪：优先尝试匹配（out1），然后尝试不匹配（out2） */
            split->out1 = frag.start;
            split->out2 = end;
            if (frag.end) {
                frag.end->out1 = end;
            }
        }
        
        frag.start = split;
        frag.end = end;
        *pattern = p + (non_greedy ? 2 : 1);  /* 跳过?或?? */
    } else if (*p == '{') {
        /* {n}, {n,}, {n,m} */
        p++;
        int min = 0, max = -1;
        
        /* 解析最小次数 */
        while (*p >= '0' && *p <= '9') {
            min = min * 10 + (*p - '0');
            p++;
        }
        
        if (*p == ',') {
            p++;
            if (*p == '}') {
                /* {n,} - 至少n次 */
                max = -1;
            } else {
                /* {n,m} */
                max = 0;
                while (*p >= '0' && *p <= '9') {
                    max = max * 10 + (*p - '0');
                    p++;
                }
            }
        } else {
            /* {n} - 恰好n次 */
            max = min;
        }
        
        if (*p != '}') {
            *pattern = p;
            return frag;  /* 语法错误 */
        }
        p++;
        
        /* 检查是否有非贪婪标记 ? */
        if (*p == '?') {
            non_greedy = true;
            p++;
        }
        
        /* 构建量词NFA */
        nfa_fragment_t result = { NULL, NULL };
        
        if (max == -1) {
            /* 无上限 {n,} - 至少n次 */
            if (min == 0) {
                /* {0,} 等同于 * */
                nfa_state_t* split = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!split) return result;
                
                nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!end) return result;
                end->out1 = NULL;
                end->out2 = NULL;
                
                split->non_greedy = non_greedy;
                if (non_greedy) {
                    split->out1 = end;      /* 不匹配，直接结束 */
                    split->out2 = frag.start; /* 匹配，继续 */
                } else {
                    split->out1 = frag.start;
                    split->out2 = end;
                }
                frag.end->non_greedy = non_greedy;
                if (non_greedy) {
                    frag.end->out1 = end;    /* 优先结束 */
                    frag.end->out2 = split;  /* 然后循环 */
                } else {
                    frag.end->out1 = split;  /* 循环回split */
                    frag.end->out2 = end;    /* 或者直接到end */
                }
                result.start = split;
                result.end = end;
                *pattern = p;
            } else {
                /* 至少n次 {n,} - 需要先匹配n次，然后可以继续 */
                /* 使用循环结构：先匹配min次，然后可以继续循环 */
                nfa_state_t* split = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!split) return result;
                
                nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!end) return result;
                end->out1 = NULL;
                end->out2 = NULL;
                
                /* 至少n次 {n,} - 需要先匹配n次，然后可以继续 */
                /* 使用循环结构：先匹配min次，然后可以继续循环 */
                /* 方法：split -> frag -> ... -> frag -> (可以循环或结束) */
                
                result.start = split;
                result.end = end;
                
                if (min == 0) {
                    /* {0,} 等同于 * */
                    split->out1 = frag.start;
                    split->out2 = end;
                    frag.end->out1 = frag.start;  /* 循环 */
                    frag.end->out2 = end;          /* 或结束 */
                } else {
                    /* 必须匹配min次，然后可以继续 */
                    /* 构建线性路径：split -> frag -> intermediate -> frag -> ... -> intermediate -> (循环或结束) */
                    nfa_state_t* current = split;
                    //nfa_state_t* last_intermediate = NULL;
                    
                    /* 匹配min次，每次匹配使用独立的片段副本 */
                    nfa_fragment_t frags[128];
                    if (min > 128) return result;
                    
                    for (int i = 0; i < min; i++) {
                        frags[i] = clone_fragment(mpool, frag);
                        if (!frags[i].start) return result;
                    }
                    
                    /* 连接所有片段 */
                    current = split;
                    for (int i = 0; i < min; i++) {
                        current->out1 = frags[i].start;
                        current->out2 = NULL;
                        current = frags[i].end;
                    }
                    
                    /* 在匹配min次后，可以继续循环或结束 */
                    nfa_state_t* loop_split = create_nfa_state(mpool, NFA_STATE_SPLIT);
                    if (!loop_split) return result;
                    loop_split->out1 = frag.start;  /* 可以继续循环 */
                    loop_split->out2 = end;          /* 或结束 */
                    
                    current->out1 = loop_split;
                    current->out2 = end;
                    
                    /* 设置循环：匹配后可以继续或结束 */
                    loop_split->non_greedy = non_greedy;
                    if (non_greedy) {
                        loop_split->out1 = end;      /* 优先结束 */
                        loop_split->out2 = frag.start; /* 然后循环 */
                    } else {
                        loop_split->out1 = frag.start;  /* 可以继续循环 */
                        loop_split->out2 = end;          /* 或结束 */
                    }
                    frag.end->non_greedy = non_greedy;
                    if (non_greedy) {
                        frag.end->out1 = end;      /* 优先结束 */
                        frag.end->out2 = frag.start; /* 然后循环 */
                    } else {
                        frag.end->out1 = frag.start;  /* 循环 */
                        frag.end->out2 = end;          /* 或结束 */
                    }
                }
                *pattern = p;
            }
        } else if (max > min) {
            /* 有上限 {n,m} - n到m次 */
            nfa_state_t* quant_end = create_nfa_state(mpool, NFA_STATE_SPLIT);
            if (!quant_end) return result;
            quant_end->out1 = NULL;
            quant_end->out2 = NULL;
            
            if (min == 0) {
                /* {0,m} - 0到m次 */
                nfa_state_t* split = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!split) return result;
                
                split->out1 = frag.start;
                split->out2 = quant_end;
                
                nfa_state_t* last_split = split;
                for (int i = 1; i < max; i++) {
                    nfa_state_t* new_split = create_nfa_state(mpool, NFA_STATE_SPLIT);
                    if (!new_split) return result;
                    
                    last_split->out2 = new_split;
                    new_split->out1 = frag.start;
                    new_split->out2 = quant_end;
                    
                    nfa_state_t* new_end = create_nfa_state(mpool, NFA_STATE_SPLIT);
                    if (!new_end) return result;
                    new_end->out1 = NULL;
                    new_end->out2 = NULL;
                    
                    frag.end->out1 = new_end;
                    last_split = new_end;
                }
                
                split->non_greedy = non_greedy;
                if (non_greedy) {
                    split->out1 = quant_end;  /* 优先结束（不匹配） */
                    split->out2 = frag.start; /* 然后匹配 */
                } else {
                    split->out1 = frag.start;
                    split->out2 = quant_end;
                }
                result.start = split;
                result.end = quant_end;
                *pattern = p;
            } else {
                /* n > 0，需要先匹配n次，然后可以匹配到m次 {n,m} */
                nfa_state_t* final_end = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!final_end) return result;
                final_end->out1 = NULL;
                final_end->out2 = NULL;
                
                /* n > 0，需要先匹配n次，然后可以匹配到m次 {n,m} */
                /* 构建线性路径：split -> frag -> ... -> frag -> (可选匹配) -> final_end */
                nfa_state_t* split = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!split) return result;
                
                result.start = split;
                result.end = final_end;
                
                nfa_state_t* current = split;
                
                /* 必须匹配min次，每次匹配使用独立的片段副本 */
                nfa_fragment_t frags[128];
                if (min > 128) return result;
                
                for (int i = 0; i < min; i++) {
                    frags[i] = clone_fragment(mpool, frag);
                    if (!frags[i].start) return result;
                }
                
                /* 连接所有片段 */
                current = split;
                for (int i = 0; i < min; i++) {
                    current->out1 = frags[i].start;
                    current->out2 = NULL;
                    current = frags[i].end;
                }
                
                /* 在匹配min次后，可以继续匹配0到(max-min)次，或者结束 */
                /* 对于非贪婪：优先结束（out1），然后继续匹配（out2） */
                /* 对于贪婪：优先继续匹配（out1），然后结束（out2） */
                
                /* 创建可选匹配路径：可以再匹配1次、2次、...、max-min次 */
                /* 关键：每个可选路径匹配后，只能继续到下一个可选路径或结束，不能循环 */
                /* 方法：创建(max-min)个可选片段，每个片段匹配后只能到下一个或结束 */
                nfa_state_t* opt_splits[128];
                nfa_fragment_t opt_frags[128];
                if ((max - min) > 128) return result;
                
                /* 先创建所有可选片段和split状态 */
                for (int i = 0; i < (max - min); i++) {
                    opt_frags[i] = clone_fragment(mpool, frag);
                    if (!opt_frags[i].start) return result;
                    
                    opt_splits[i] = create_nfa_state(mpool, NFA_STATE_SPLIT);
                    if (!opt_splits[i]) return result;
                    opt_splits[i]->non_greedy = non_greedy;
                    if (non_greedy) {
                        /* 非贪婪：优先结束（out1），然后继续匹配（out2） */
                        opt_splits[i]->out1 = final_end;  /* 不匹配，直接结束 */
                        opt_splits[i]->out2 = opt_frags[i].start;  /* 匹配，继续 */
                    } else {
                        /* 贪婪：优先继续匹配（out1），然后结束（out2） */
                        opt_splits[i]->out1 = opt_frags[i].start;
                        opt_splits[i]->out2 = final_end;  /* 可以不匹配，直接结束 */
                    }
                }
                
                /* 连接可选路径 */
                /* 对于非贪婪：优先结束（out1），然后继续匹配（out2） */
                /* 对于贪婪：优先继续匹配（out1），然后结束（out2） */
                /* 关键：必须设置 non_greedy 标记，这样 epsilon_closure 才能正确识别并优先处理结束路径 */
                current->non_greedy = non_greedy;
                if (non_greedy) {
                    current->out1 = final_end;  /* 优先结束（匹配恰好min次） */
                    current->out2 = opt_splits[0];  /* 然后可以继续匹配 */
                } else {
                    current->out1 = opt_splits[0];  /* 优先继续匹配 */
                    current->out2 = final_end;  /* 然后可以结束 */
                }
                
                for (int i = 0; i < (max - min); i++) {
                    opt_frags[i].end->non_greedy = non_greedy;
                    if (i < (max - min) - 1) {
                        /* 不是最后一个可选路径，匹配后可以继续到下一个或结束 */
                        if (non_greedy) {
                            opt_frags[i].end->out1 = final_end;  /* 优先结束 */
                            opt_frags[i].end->out2 = opt_splits[i + 1];  /* 然后继续到下一个 */
                        } else {
                            opt_frags[i].end->out1 = opt_splits[i + 1];  /* 优先继续到下一个 */
                            opt_frags[i].end->out2 = final_end;  /* 然后结束 */
                        }
                    } else {
                        /* 最后一个可选路径，匹配后只能结束 */
                        opt_frags[i].end->out1 = final_end;
                        opt_frags[i].end->out2 = NULL;
                    }
                }
                
                /* 更新pattern指针 */
                *pattern = p;
            }
        } else {
            /* max == min，恰好n次 {n} */
            if (min == 0) {
                /* {0} - 不匹配任何内容，直接到end */
                nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!end) return result;
                end->out1 = NULL;
                end->out2 = NULL;
                result.start = end;
                result.end = end;
                *pattern = p;
            } else {
                /* 恰好匹配n次，不允许更多也不允许更少 */
                /* 构建必须匹配恰好n次的结构 */
                nfa_state_t* split = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!split) return result;
                
                nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
                if (!end) return result;
                end->out1 = NULL;
                end->out2 = NULL;
                
                /* 恰好匹配n次：创建n个独立的片段副本并线性连接 */
                result.start = split;
                result.end = end;
                *pattern = p;
                
                nfa_state_t* current = split;
                
                /* 创建n个独立的片段副本（每个副本匹配一次） */
                nfa_fragment_t frags[128];
                if (min > 128) return result;
                
                for (int i = 0; i < min; i++) {
                    frags[i] = clone_fragment(mpool, frag);
                    if (!frags[i].start) return result;
                }
                
                /* 连接所有片段 */
                for (int i = 0; i < min; i++) {
                    current->out1 = frags[i].start;
                    current->out2 = NULL;
                    
                    if (i < min - 1) {
                        current = frags[i].end;
                    } else {
                        frags[i].end->out1 = end;
                        frags[i].end->out2 = NULL;
                    }
                }
            }
        }
        
        frag = result;
        *pattern = p;
    }
    
    return frag;
}

/* 解析因子（atom + quantifier） */
static nfa_fragment_t parse_factor(vox_mpool_t* mpool, const char** pattern,
                                    int flags, int* group_id) {
    nfa_fragment_t frag = parse_atom(mpool, pattern, flags, group_id);
    if (frag.start == NULL) {
        return frag;
    }
    
    /* 应用量词 */
    frag = apply_quantifier(mpool, frag, pattern);
    return frag;
}

/* 解析项（factor | factor | ...） */
static nfa_fragment_t parse_term(vox_mpool_t* mpool, const char** pattern,
                                 int flags, int* group_id) {
    nfa_fragment_t frag = parse_factor(mpool, pattern, flags, group_id);
    if (frag.start == NULL) {
        return frag;
    }
    
    /* 处理连接（concatenation） */
    while (**pattern != '\0' && **pattern != '|' && **pattern != ')') {
        nfa_fragment_t next = parse_factor(mpool, pattern, flags, group_id);
        if (next.start == NULL) {
            break;
        }
        
        /* 连接两个片段 */
        frag.end->out1 = next.start;
        frag.end = next.end;
    }
    
    return frag;
}

/* 解析表达式（term | term | ...） */
static nfa_fragment_t parse_expr(vox_mpool_t* mpool, const char** pattern,
                                 int flags, int* group_id) {
    nfa_fragment_t frag = parse_term(mpool, pattern, flags, group_id);
    if (frag.start == NULL) {
        return frag;
    }
    
    /* 处理选择（alternation） */
    while (**pattern == '|') {
        (*pattern)++;
        nfa_fragment_t next = parse_term(mpool, pattern, flags, group_id);
        if (next.start == NULL) {
            break;
        }
        
        /* 创建分割状态 */
        nfa_state_t* split = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!split) return frag;
        
        /* 创建结束状态（SPLIT类型，用于连接后续表达式） */
        nfa_state_t* end = create_nfa_state(mpool, NFA_STATE_SPLIT);
        if (!end) return frag;
        end->out1 = NULL;
        end->out2 = NULL;
        
        split->out1 = frag.start;
        split->out2 = next.start;
        
        /* 连接两个分支到结束状态 */
        if (frag.end) {
            frag.end->out1 = end;
        }
        if (next.end) {
            next.end->out1 = end;
        }
        
        frag.start = split;
        frag.end = end;
    }
    
    return frag;
}

/* 为所有状态分配ID并统计总数 */
static void assign_state_ids(nfa_state_t* state, int* count) {
    if (!state || state->id != -1) return;
    
    state->id = (*count)++;
    
    assign_state_ids(state->out1, count);
    assign_state_ids(state->out2, count);
    
    /* 处理断言子NFA中的状态 */
    if (state->type >= NFA_STATE_LOOKAHEAD_POS && state->type <= NFA_STATE_LOOKBEHIND_NEG) {
        assign_state_ids(state->u.assertion.start, count);
    }
}

/* 编译正则表达式 */
vox_regex_t* vox_regex_compile(vox_mpool_t* mpool, const char* pattern, int flags) {
    if (!mpool || !pattern) {
        return NULL;
    }
    
    vox_regex_t* regex = (vox_regex_t*)vox_mpool_alloc(mpool, sizeof(vox_regex_t));
    if (!regex) {
        return NULL;
    }
    
    memset(regex, 0, sizeof(vox_regex_t));
    regex->mpool = mpool;
    regex->flags = flags;
    regex->group_count = 0;
    regex->has_non_greedy = false;
    
    /* 复制模式字符串 */
    size_t pattern_len = strlen(pattern);
    regex->pattern = (char*)vox_mpool_alloc(mpool, pattern_len + 1);
    if (!regex->pattern) {
        return NULL;
    }
    memcpy(regex->pattern, pattern, pattern_len + 1);
    
    /* 解析并构建NFA */
    const char* p = pattern;
    int group_id = 0;
    nfa_fragment_t frag = parse_expr(mpool, &p, flags, &group_id);
    
    if (frag.start == NULL || *p != '\0') {
        return NULL;
    }
    
    /* 检查是否包含非贪婪量词 */
    const char* p_check = pattern;
    while (*p_check != '\0') {
        if (*p_check == '?' && p_check > pattern) {
            char prev = p_check[-1];
            if (prev == '*' || prev == '+' || prev == '?' || prev == '}') {
                regex->has_non_greedy = true;
                break;
            }
        }
        p_check++;
    }
    
    /* 创建匹配状态 */
    nfa_state_t* match_state = create_nfa_state(mpool, NFA_STATE_MATCH);
    if (!match_state) return NULL;
    
    if (frag.end && frag.end->type == NFA_STATE_SPLIT) {
        frag.end->out1 = match_state;
    } else if (frag.end) {
        nfa_state_t* split = create_nfa_state(mpool, NFA_STATE_SPLIT);
        split->out1 = match_state;
        frag.end->out1 = split;
    }
    
    regex->start = frag.start;
    regex->group_count = group_id;
    
    /* 分配状态ID并统计总数 */
    int total_states = 0;
    assign_state_ids(regex->start, &total_states);
    regex->state_count = total_states;
    
    /* 提取字面量前缀用于快速搜索优化 */
    nfa_state_t* curr = regex->start;
    char prefix_buf[256];
    size_t plen = 0;
    while (curr && plen < sizeof(prefix_buf) - 1) {
        if (curr->type == NFA_STATE_CHAR) {
            prefix_buf[plen++] = curr->u.ch;
            curr = curr->out1;
        } else if (curr->type == NFA_STATE_SPLIT && curr->out1 && !curr->out2) {
            curr = curr->out1;
        } else {
            break;
        }
    }
    if (plen > 0) {
        regex->prefix = (char*)vox_mpool_alloc(mpool, plen + 1);
        if (regex->prefix) {
            memcpy(regex->prefix, prefix_buf, plen);
            regex->prefix[plen] = '\0';
            regex->prefix_len = plen;
        }
    }
    
    return regex;
}

/* ===== 匹配算法 ===== */

/* 内部匹配函数（前向声明） */
static bool match_internal(vox_regex_t* regex, const char* text, size_t text_len,
                           size_t start_pos, bool full_match, 
                           vox_regex_matches_t* matches);

static bool match_internal_with_context(vox_regex_t* regex, match_context_t* ctx,
                                       const char* text, size_t text_len,
                                       size_t start_pos, bool full_match, 
                                       vox_regex_matches_t* matches);

/* 辅助函数：验证断言子NFA是否匹配 */
static bool verify_assertion(nfa_state_t* start_state, const char* text, size_t text_len, 
                             size_t pos, int flags, vox_mpool_t* mpool, bool lookbehind, int state_count) {
    if (!start_state) return true;
    
    struct vox_regex tmp_regex;
    tmp_regex.mpool = mpool;
    tmp_regex.start = start_state;
    tmp_regex.flags = flags;
    tmp_regex.group_count = 0;
    tmp_regex.pattern = "";
    tmp_regex.has_non_greedy = true;
    tmp_regex.state_count = state_count;
    tmp_regex.prefix = NULL;
    tmp_regex.prefix_len = 0;
    
    if (lookbehind) {
        /* 对于后行断言，我们需要找到一个起始位置 i <= pos，使得子表达式从 i 到 pos 恰好完全匹配 */
        for (int i = (int)pos; i >= 0; i--) {
            /* 使用 full_match = true 并限制文本长度为 pos，确保匹配正好在 pos 结束 */
            if (match_internal(&tmp_regex, text, pos, (size_t)i, true, NULL)) {
                return true;
            }
        }
        return false;
    } else {
        return match_internal(&tmp_regex, text, text_len, pos, false, NULL);
    }
}

/* 添加状态到列表（epsilon闭包） */
static void add_state(match_context_t* ctx, state_list_t* list, nfa_state_t* state, size_t pos,
                      size_t* group_starts, size_t* group_ends,
                      const char* text, size_t text_len, int flags) {
    if (!state || ctx->visited[state->id] == ctx->generation) return;
    ctx->visited[state->id] = ctx->generation;
    
    if (state->type == NFA_STATE_SPLIT) {
        /* Thompson NFA 优先级：out1 优先于 out2 */
        add_state(ctx, list, state->out1, pos, group_starts, group_ends, text, text_len, flags);
        add_state(ctx, list, state->out2, pos, group_starts, group_ends, text, text_len, flags);
    } else if (state->type == NFA_STATE_ANCHOR_START) {
        bool match = (pos == 0) || ((flags & VOX_REGEX_MULTILINE) && pos > 0 && (text[pos-1] == '\n' || text[pos-1] == '\r'));
        if (match) add_state(ctx, list, state->out1, pos, group_starts, group_ends, text, text_len, flags);
    } else if (state->type == NFA_STATE_ANCHOR_END) {
        bool match = (pos == text_len) || ((flags & VOX_REGEX_MULTILINE) && pos < text_len && (text[pos] == '\n' || text[pos] == '\r'));
        if (match) add_state(ctx, list, state->out1, pos, group_starts, group_ends, text, text_len, flags);
    } else if (state->group_id >= 0 && group_starts && group_ends) {
        /* 捕获组标记不消耗字符，递归继续 */
        size_t old_val = state->group_start ? group_starts[state->group_id] : group_ends[state->group_id];
        if (state->group_start) group_starts[state->group_id] = pos;
        else group_ends[state->group_id] = pos;
        
        add_state(ctx, list, state->out1, pos, group_starts, group_ends, text, text_len, flags);
        
        /* 恢复值（回溯语义，虽然Thompson NFA通常不回溯，但在闭包展开时需要保持状态独立性） */
        if (state->group_start) group_starts[state->group_id] = old_val;
        else group_ends[state->group_id] = old_val;
    } else if (state->type == NFA_STATE_WORD_BOUNDARY) {
        bool left = (pos > 0) && is_word_char(text[pos-1]);
        bool right = (pos < text_len) && is_word_char(text[pos]);
        if (left != right) add_state(ctx, list, state->out1, pos, group_starts, group_ends, text, text_len, flags);
    } else if (state->type >= NFA_STATE_LOOKAHEAD_POS && state->type <= NFA_STATE_LOOKBEHIND_NEG) {
        bool res = verify_assertion(state->u.assertion.start, text, text_len, pos, flags, ctx->mpool, (state->type >= NFA_STATE_LOOKBEHIND_POS), ctx->state_count);
        bool expected = (state->type == NFA_STATE_LOOKAHEAD_POS || state->type == NFA_STATE_LOOKBEHIND_POS);
        if (res == expected) add_state(ctx, list, state->out1, pos, group_starts, group_ends, text, text_len, flags);
    } else {
        append_state_to_list(list, state, group_starts, group_ends, ctx->mpool);
    }
}

/* 检查状态列表中是否有匹配状态 */
static inline bool has_match_state(const state_list_t* list) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->states[i].state->type == NFA_STATE_MATCH) {
            return true;
        }
    }
    return false;
}

/* 内部匹配函数实现 */
static bool match_internal_with_context(vox_regex_t* regex, match_context_t* ctx,
                                       const char* text, size_t text_len,
                                       size_t start_pos, bool full_match, 
                                       vox_regex_matches_t* matches) {
    if (!regex || !ctx || !text) return false;
    
    /* 重置上下文状态 */
    ctx->current_list.count = 0;
    ctx->next_list.count = 0;
    ctx->generation = 1;
    memset(ctx->visited, 0, sizeof(uint32_t) * ctx->state_count);
    
    if (ctx->group_count > 0) {
        for (int i = 0; i <= ctx->group_count; i++) {
            ctx->g_starts[i] = ctx->g_ends[i] = (size_t)-1;
        }
    }
    
    size_t best_match_pos = (size_t)-1;
    add_state(ctx, &ctx->current_list, regex->start, start_pos, ctx->g_starts, ctx->g_ends, text, text_len, regex->flags);
    
    size_t pos = start_pos;
    while (1) {
        if (has_match_state(&ctx->current_list)) {
            if (full_match) {
                if (pos == text_len) best_match_pos = pos;
            } else {
                best_match_pos = pos;
                if (regex->has_non_greedy) break;
            }
        }
        
        if (pos >= text_len || ctx->current_list.count == 0) break;
        
        char ch = text[pos];
        ctx->next_list.count = 0;
        ctx->generation++;
        
        for (size_t i = 0; i < ctx->current_list.count; i++) {
            nfa_state_t* s = ctx->current_list.states[i].state;
            if (s->type == NFA_STATE_CHAR) {
                bool m = (regex->flags & VOX_REGEX_IGNORE_CASE) ? 
                         (to_lower_if_needed(ch, true) == to_lower_if_needed(s->u.ch, true)) : 
                         (s->u.ch == ch);
                if (m) {
                    add_state(ctx, &ctx->next_list, s->out1, pos + 1, 
                              ctx->current_list.states[i].group_starts, 
                              ctx->current_list.states[i].group_ends, 
                              text, text_len, regex->flags);
                }
            } else if (s->type == NFA_STATE_CHARSET) {
                if (char_in_charset(s->u.charset.bitmap, ch)) {
                    add_state(ctx, &ctx->next_list, s->out1, pos + 1, 
                              ctx->current_list.states[i].group_starts, 
                              ctx->current_list.states[i].group_ends, 
                              text, text_len, regex->flags);
                }
            }
        }
        
        state_list_t tmp = ctx->current_list;
        ctx->current_list = ctx->next_list;
        ctx->next_list = tmp;
        pos++;
    }
    
    if (best_match_pos != (size_t)-1) {
        if (matches) {
            matches->count = regex->group_count + 1;
            matches->matches = (vox_regex_match_t*)vox_mpool_alloc(ctx->mpool, sizeof(vox_regex_match_t) * matches->count);
            matches->matches[0].start = start_pos;
            matches->matches[0].end = best_match_pos;
            for (int i = 1; i <= regex->group_count; i++) {
                matches->matches[i].start = ctx->g_starts[i];
                matches->matches[i].end = ctx->g_ends[i];
            }
        }
        return true;
    }
    return false;
}

/* 内部匹配函数（用于兼容和递归） */
static bool match_internal(vox_regex_t* regex, const char* text, size_t text_len,
                           size_t start_pos, bool full_match, 
                           vox_regex_matches_t* matches) {
    match_context_t* ctx = create_match_context(regex);
    if (!ctx) return false;
    bool res = match_internal_with_context(regex, ctx, text, text_len, start_pos, full_match, matches);
    free_match_context(ctx);
    return res;
}

/* 匹配函数实现 */
bool vox_regex_match(vox_regex_t* regex, const char* text, size_t text_len,
                     vox_regex_matches_t* matches) {
    return match_internal(regex, text, text_len, 0, true, matches);
}

/* 内部搜索函数（支持上下文复用） */
static bool vox_regex_search_with_context(vox_regex_t* regex, match_context_t* ctx,
                                         const char* text, size_t text_len,
                                         size_t start_pos, vox_regex_match_t* match) {
    if (!regex || !text || start_pos > text_len || !ctx) return false;
    
    bool is_anchor_start = (regex->start->type == NFA_STATE_ANCHOR_START);
    bool found = false;
    
    for (size_t pos = start_pos; pos <= text_len; pos++) {
        if (is_anchor_start) {
            if (pos > 0) {
                if (!(regex->flags & VOX_REGEX_MULTILINE)) break;
                if (text[pos-1] != '\n' && text[pos-1] != '\r') continue;
            }
        }
        
        if (regex->prefix && !(regex->flags & VOX_REGEX_IGNORE_CASE) && pos < text_len) {
            const char* next_p = strstr(text + pos, regex->prefix);
            if (!next_p) break;
            pos = (size_t)(next_p - text);
        }

        vox_regex_matches_t matches = { NULL, 0, 0 };
        if (match_internal_with_context(regex, ctx, text, text_len, pos, false, &matches)) {
            if (match && matches.matches) {
                match->start = matches.matches[0].start;
                match->end = matches.matches[0].end;
            }
            found = true;
            break;
        }
        if (pos == text_len) break;
    }
    
    return found;
}

/* 搜索函数实现 */
bool vox_regex_search(vox_regex_t* regex, const char* text, size_t text_len,
                      size_t start_pos, vox_regex_match_t* match) {
    if (!regex || !text || start_pos > text_len) return false;
    
    match_context_t* ctx = create_match_context(regex);
    if (!ctx) return false;
    
    bool res = vox_regex_search_with_context(regex, ctx, text, text_len, start_pos, match);
    free_match_context(ctx);
    return res;
}

/* 查找所有匹配 */
int vox_regex_findall(vox_regex_t* regex, const char* text, size_t text_len,
                      vox_regex_match_t** matches, size_t* match_count) {
    if (!regex || !text || !matches || !match_count) {
        return -1;
    }
    
    *matches = NULL;
    *match_count = 0;
    
    match_context_t* ctx = create_match_context(regex);
    if (!ctx) return -1;
    
    size_t capacity = 16;
    *matches = (vox_regex_match_t*)vox_mpool_alloc(
        regex->mpool, sizeof(vox_regex_match_t) * capacity);
    if (!*matches) {
        return -1;
    }
    
    size_t pos = 0;
    while (pos <= text_len) {
        vox_regex_match_t match;
        if (vox_regex_search_with_context(regex, ctx, text, text_len, pos, &match)) {
            if (*match_count >= capacity) {
                size_t old_capacity = capacity;
                capacity *= 2;
                vox_regex_match_t* new_matches = (vox_regex_match_t*)vox_mpool_alloc(
                    regex->mpool, sizeof(vox_regex_match_t) * capacity);
                if (!new_matches) break;
                memcpy(new_matches, *matches, sizeof(vox_regex_match_t) * old_capacity);
                vox_mpool_free(regex->mpool, *matches);
                *matches = new_matches;
            }
            
            (*matches)[*match_count] = match;
            (*match_count)++;
            
            if (match.end > pos) {
                pos = match.end;
            } else {
                pos++;
            }
        } else {
            break;
        }
    }
    
    free_match_context(ctx);
    return 0;
}

/* 替换函数实现 */
int vox_regex_replace(vox_regex_t* regex, const char* text, size_t text_len,
                      const char* replacement, char* output, size_t output_size,
                      size_t* output_len) {
    if (!regex || !text || !replacement || !output || !output_len) {
        return -1;
    }
    
    match_context_t* ctx = create_match_context(regex);
    if (!ctx) return -1;
    
    size_t out_pos = 0;
    size_t text_pos = 0;
    
    while (text_pos <= text_len) {
        vox_regex_match_t match;
        if (vox_regex_search_with_context(regex, ctx, text, text_len, text_pos, &match)) {
            /* 复制匹配前的文本 */
            size_t copy_len = match.start - text_pos;
            if (safe_copy_to_output(output, &out_pos, output_size, text + text_pos, copy_len) < 0) {
                return -1;
            }
            
            /* 处理替换字符串 */
            const char* rep = replacement;
            while (*rep != '\0') {
                if (*rep == '$' && rep[1] >= '0' && rep[1] <= '9') {
                    /* 引用捕获组 */
                    int group_num = rep[1] - '0';
                    /* 简化：只支持$0（完整匹配） */
                    if (group_num == 0) {
                        size_t match_len = match.end - match.start;
                        if (safe_copy_to_output(output, &out_pos, output_size, text + match.start, match_len) < 0) {
                            return -1;
                        }
                    }
                    rep += 2;
                } else {
                    if (out_pos >= output_size) {
                        return -1;
                    }
                    output[out_pos++] = *rep++;
                }
            }
            
            text_pos = match.end;
            if (match.end == match.start) {
                if (text_pos < text_len) text_pos++;  /* 避免无限循环 */
                else break;
            }
        } else {
            /* 复制剩余文本 */
            size_t copy_len = text_len - text_pos;
            if (safe_copy_to_output(output, &out_pos, output_size, text + text_pos, copy_len) < 0) {
                free_match_context(ctx);
                return -1;
            }
            break;
        }
    }
    
    free_match_context(ctx);
    output[out_pos] = '\0';
    *output_len = out_pos;
    return 0;
}

/* 销毁正则表达式 */
void vox_regex_destroy(vox_regex_t* regex) {
    if (regex) {
        /* 内存池会自动管理所有内存，这里不需要手动释放 */
    }
}

/* 释放匹配结果 */
void vox_regex_free_matches(vox_regex_t* regex, vox_regex_match_t* matches, size_t match_count) {
    if (regex && matches) {
        vox_mpool_free(regex->mpool, matches);
    }
    (void)match_count;
}
