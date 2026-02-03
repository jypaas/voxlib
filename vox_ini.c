/*
 * vox_ini.c - 高性能 INI 解析器和生成器实现
 */

#include "vox_ini.h"
#include "vox_string.h"
#include "vox_file.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ===== 内部辅助函数 ===== */

static char* mpool_strdup(vox_mpool_t* mpool, const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* copy = (char*)vox_mpool_alloc(mpool, len + 1);
    if (copy) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

static char* mpool_strndup(vox_mpool_t* mpool, const char* s, size_t n) {
    if (!s) return NULL;
    char* copy = (char*)vox_mpool_alloc(mpool, n + 1);
    if (copy) {
        memcpy(copy, s, n);
        copy[n] = '\0';
    }
    return copy;
}

static void skip_to_eol(vox_scanner_t* scanner) {
    while (!vox_scanner_eof(scanner)) {
        int ch = vox_scanner_get_char(scanner);
        if (ch == '\n' || ch == '\r') {
            if (ch == '\r' && vox_scanner_peek_char(scanner) == '\n') {
                vox_scanner_get_char(scanner);
            }
            break;
        }
    }
}

static vox_ini_section_t* find_section(const vox_ini_t* ini, const char* name) {
    vox_list_node_t* node;
    vox_list_for_each(node, &ini->sections) {
        vox_ini_section_t* sec = vox_container_of(node, vox_ini_section_t, node);
        if (name == NULL && sec->name == NULL) return sec;
        if (name && sec->name && strcmp(name, sec->name) == 0) return sec;
    }
    return NULL;
}

static vox_ini_keyvalue_t* find_keyvalue(const vox_ini_section_t* sec, const char* key) {
    vox_list_node_t* node;
    vox_list_for_each(node, &sec->keyvalues) {
        vox_ini_keyvalue_t* kv = vox_container_of(node, vox_ini_keyvalue_t, node);
        if (kv->key && strcmp(key, kv->key) == 0) return kv;
    }
    return NULL;
}

/* ===== 公共接口实现 ===== */

vox_ini_t* vox_ini_create(vox_mpool_t* mpool) {
    vox_ini_t* ini = (vox_ini_t*)vox_mpool_alloc(mpool, sizeof(vox_ini_t));
    if (ini) {
        ini->mpool = mpool;
        vox_list_init(&ini->sections);
    }
    return ini;
}

vox_ini_t* vox_ini_parse(vox_mpool_t* mpool, const char* ini_str, vox_ini_err_info_t* err_info) {
    if (!mpool || !ini_str) return NULL;

    vox_ini_t* ini = vox_ini_create(mpool);
    if (!ini) return NULL;

    vox_scanner_t scanner;
    vox_scanner_init(&scanner, (char*)ini_str, strlen(ini_str), VOX_SCANNER_NONE);

    vox_ini_section_t* current_section = NULL;
    char* pending_comment = NULL;

    while (!vox_scanner_eof(&scanner)) {
        vox_scanner_skip_ws(&scanner);
        if (vox_scanner_eof(&scanner)) break;

        int ch = vox_scanner_peek_char(&scanner);
        if (ch == ';' || ch == '#') {
            /* 解析注释 */
            const char* start = vox_scanner_curptr(&scanner);
            skip_to_eol(&scanner);
            const char* end = vox_scanner_curptr(&scanner);
            /* 这里简单地累积注释，真实实现可能需要更复杂的关联逻辑 */
            pending_comment = mpool_strndup(mpool, start, end - start);
            continue;
        }

        if (ch == '[') {
            /* 解析 Section */
            vox_scanner_get_char(&scanner); /* skip [ */
            vox_scanner_skip_ws(&scanner);
            const char* start = vox_scanner_curptr(&scanner);
            while (!vox_scanner_eof(&scanner) && vox_scanner_peek_char(&scanner) != ']' && vox_scanner_peek_char(&scanner) != '\n') {
                vox_scanner_get_char(&scanner);
            }
            const char* end = vox_scanner_curptr(&scanner);
            if (vox_scanner_peek_char(&scanner) == ']') {
                vox_scanner_get_char(&scanner);
            }

            /* 去掉末尾空白 */
            while (end > start && isspace((unsigned char)end[-1])) end--;

            current_section = (vox_ini_section_t*)vox_mpool_alloc(mpool, sizeof(vox_ini_section_t));
            if (current_section) {
                current_section->name = mpool_strndup(mpool, start, end - start);
                vox_list_init(&current_section->keyvalues);
                current_section->comment = pending_comment;
                pending_comment = NULL;
                vox_list_push_back(&ini->sections, &current_section->node);
            }
            skip_to_eol(&scanner);
        } else {
            /* 解析 Key-Value */
            const char* k_start = vox_scanner_curptr(&scanner);
            while (!vox_scanner_eof(&scanner) && vox_scanner_peek_char(&scanner) != '=' && vox_scanner_peek_char(&scanner) != '\n') {
                vox_scanner_get_char(&scanner);
            }
            const char* k_end = vox_scanner_curptr(&scanner);
            
            if (vox_scanner_peek_char(&scanner) == '=') {
                vox_scanner_get_char(&scanner); /* skip = */
                
                /* 去掉 key 后的空白 */
                while (k_end > k_start && isspace((unsigned char)k_end[-1])) k_end--;

                while (!vox_scanner_eof(&scanner) && (*vox_scanner_curptr(&scanner) == ' ' || *vox_scanner_curptr(&scanner) == '\t')) {
                    vox_scanner_get_char(&scanner);
                }
                const char* v_start = vox_scanner_curptr(&scanner);
                skip_to_eol(&scanner);
                const char* v_end = vox_scanner_curptr(&scanner);
                
                /* 去掉 value 后的空白和换行 */
                while (v_end > v_start && isspace((unsigned char)v_end[-1])) v_end--;

                /* 如果还没有 section，创建一个默认的 */
                if (!current_section) {
                    current_section = (vox_ini_section_t*)vox_mpool_alloc(mpool, sizeof(vox_ini_section_t));
                    if (current_section) {
                        current_section->name = NULL;
                        vox_list_init(&current_section->keyvalues);
                        current_section->comment = NULL;
                        vox_list_push_back(&ini->sections, &current_section->node);
                    }
                }

                if (current_section) {
                    vox_ini_keyvalue_t* kv = (vox_ini_keyvalue_t*)vox_mpool_alloc(mpool, sizeof(vox_ini_keyvalue_t));
                    if (kv) {
                        kv->key = mpool_strndup(mpool, k_start, k_end - k_start);
                        kv->value = mpool_strndup(mpool, v_start, v_end - v_start);
                        kv->comment = NULL; /* TODO: 支持行内注释 */
                        vox_list_push_back(&current_section->keyvalues, &kv->node);
                    }
                }
            } else {
                /* 可能是空行或者是没有值的 key */
                skip_to_eol(&scanner);
            }
        }
    }

    return ini;
}

vox_ini_t* vox_ini_parse_file(vox_mpool_t* mpool, const char* filepath, vox_ini_err_info_t* err_info) {
    if (!mpool || !filepath) return NULL;

    size_t size;
    char* content = (char*)vox_file_read_all(mpool, filepath, &size);
    if (!content) return NULL;

    /* vox_file_read_all 应该保证了末尾有 \0，但为了保险起见（或者 scanner 要求可写缓冲区） */
    /* 实际上 vox_file_read_all 分配的内存块大小通常比实际请求大一点点，或者正好。 */
    /* 我们直接传给 vox_ini_parse */
    vox_ini_t* ini = vox_ini_parse(mpool, content, err_info);
    
    /* 注意：vox_ini_parse 内部使用了 mpool_strndup，所以 content 可以释放 */
    vox_mpool_free(mpool, content);
    
    return ini;
}

void vox_ini_destroy(vox_ini_t* ini) {
    /* 内存池会自动释放所有分配的内存 */
    (void)ini;
}

const char* vox_ini_get_value(const vox_ini_t* ini, const char* section_name, const char* key) {
    if (!ini || !key) return NULL;
    vox_ini_section_t* sec = find_section(ini, section_name);
    if (!sec) return NULL;
    vox_ini_keyvalue_t* kv = find_keyvalue(sec, key);
    return kv ? kv->value : NULL;
}

int vox_ini_set_value(vox_ini_t* ini, const char* section_name, const char* key, const char* value) {
    if (!ini || !key || !value) return -1;

    vox_ini_section_t* sec = find_section(ini, section_name);
    if (!sec) {
        sec = (vox_ini_section_t*)vox_mpool_alloc(ini->mpool, sizeof(vox_ini_section_t));
        if (!sec) return -1;
        sec->name = mpool_strdup(ini->mpool, section_name);
        vox_list_init(&sec->keyvalues);
        sec->comment = NULL;
        vox_list_push_back(&ini->sections, &sec->node);
    }

    vox_ini_keyvalue_t* kv = find_keyvalue(sec, key);
    if (kv) {
        kv->value = mpool_strdup(ini->mpool, value);
    } else {
        kv = (vox_ini_keyvalue_t*)vox_mpool_alloc(ini->mpool, sizeof(vox_ini_keyvalue_t));
        if (!kv) return -1;
        kv->key = mpool_strdup(ini->mpool, key);
        kv->value = mpool_strdup(ini->mpool, value);
        kv->comment = NULL;
        vox_list_push_back(&sec->keyvalues, &kv->node);
    }

    return 0;
}

int vox_ini_remove_key(vox_ini_t* ini, const char* section_name, const char* key) {
    if (!ini || !key) return -1;
    vox_ini_section_t* sec = find_section(ini, section_name);
    if (!sec) return -1;
    vox_ini_keyvalue_t* kv = find_keyvalue(sec, key);
    if (kv) {
        vox_list_remove(&sec->keyvalues, &kv->node);
        return 0;
    }
    return -1;
}

int vox_ini_remove_section(vox_ini_t* ini, const char* section_name) {
    if (!ini) return -1;
    vox_ini_section_t* sec = find_section(ini, section_name);
    if (sec) {
        vox_list_remove(&ini->sections, &sec->node);
        return 0;
    }
    return -1;
}

char* vox_ini_to_string(const vox_ini_t* ini, size_t* out_size) {
    if (!ini) return NULL;

    /* 简单的动态字符串缓冲实现 */
    size_t capacity = 1024;
    size_t len = 0;
    char* buf = (char*)vox_mpool_alloc(ini->mpool, capacity);
    if (!buf) return NULL;

    vox_list_node_t* s_node;
    vox_list_for_each(s_node, &ini->sections) {
        vox_ini_section_t* sec = vox_container_of(s_node, vox_ini_section_t, node);
        
        /* 估算所需的额外空间 */
        size_t needed = 0;
        if (sec->comment) needed += strlen(sec->comment) + 2;
        if (sec->name) needed += strlen(sec->name) + 4;
        
        vox_list_node_t* kv_node;
        vox_list_for_each(kv_node, &sec->keyvalues) {
            vox_ini_keyvalue_t* kv = vox_container_of(kv_node, vox_ini_keyvalue_t, node);
            needed += strlen(kv->key) + strlen(kv->value) + 4;
        }
        needed += 2;

        if (len + needed >= capacity) {
            size_t new_cap = capacity * 2;
            while (len + needed >= new_cap) new_cap *= 2;
            char* new_buf = (char*)vox_mpool_realloc(ini->mpool, buf, new_cap);
            if (!new_buf) {
                vox_mpool_free(ini->mpool, buf);
                return NULL;
            }
            buf = new_buf;
            capacity = new_cap;
        }

        if (sec->comment) {
            len += sprintf(buf + len, "%s", sec->comment);
            if (buf[len-1] != '\n') buf[len++] = '\n';
        }

        if (sec->name) {
            len += sprintf(buf + len, "[%s]\n", sec->name);
        }

        vox_list_for_each(kv_node, &sec->keyvalues) {
            vox_ini_keyvalue_t* kv = vox_container_of(kv_node, vox_ini_keyvalue_t, node);
            len += sprintf(buf + len, "%s=%s\n", kv->key, kv->value);
        }
        len += sprintf(buf + len, "\n");
    }

    if (out_size) *out_size = len;
    buf[len] = '\0';
    return buf;
}

int vox_ini_write_file(const vox_ini_t* ini, const char* filepath) {
    if (!ini || !filepath) return -1;
    size_t size;
    char* content = vox_ini_to_string(ini, &size);
    if (!content) return -1;

    return vox_file_write_all(ini->mpool, filepath, content, size);
}
