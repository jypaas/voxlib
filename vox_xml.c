/*
 * vox_xml.c - 高性能 XML 解析器实现
 * 使用 vox_scanner 实现零拷贝解析，使用 vox_mpool 进行内存管理
 */

#include "vox_xml.h"
#include "vox_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>

/* ===== 内部辅助函数 ===== */

/* 更新错误信息 */
static void set_error(vox_xml_err_info_t* err_info, vox_scanner_t* scanner,
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

/* 跳过空白字符 */
static void skip_whitespace(vox_scanner_t* scanner) {
    vox_scanner_skip_ws(scanner);
}

/* 解析 XML 名称（标签名、属性名等） */
static int parse_name(vox_scanner_t* scanner, vox_strview_t* name,
                      vox_xml_err_info_t* err_info) {
    const char* start = vox_scanner_curptr(scanner);
    
    /* XML 名称必须以字母、下划线或冒号开头 */
    int ch = vox_scanner_peek_char(scanner);
    if (ch < 0) {
        set_error(err_info, scanner, "Unexpected end of input while parsing name");
        return -1;
    }
    
    if (!isalpha(ch) && ch != '_' && ch != ':') {
        set_error(err_info, scanner, "Invalid name start character");
        return -1;
    }
    
    vox_scanner_get_char(scanner);
    
    /* 继续读取名称字符（字母、数字、下划线、冒号、连字符、点） */
    while (!vox_scanner_eof(scanner)) {
        ch = vox_scanner_peek_char(scanner);
        if (ch < 0) break;
        
        if (isalnum(ch) || ch == '_' || ch == ':' || ch == '-' || ch == '.') {
            vox_scanner_get_char(scanner);
        } else {
            break;
        }
    }
    
    const char* end = vox_scanner_curptr(scanner);
    name->ptr = start;
    name->len = end - start;
    
    return 0;
}

/* 解析 XML 属性值（支持单引号和双引号） */
static int parse_attr_value(vox_scanner_t* scanner, vox_strview_t* value,
                            vox_xml_err_info_t* err_info) {
    skip_whitespace(scanner);
    
    int quote = vox_scanner_peek_char(scanner);
    if (quote != '"' && quote != '\'') {
        set_error(err_info, scanner, "Expected quote character for attribute value");
        return -1;
    }
    
    vox_scanner_get_char(scanner);  /* 跳过开始引号 */
    const char* start = vox_scanner_curptr(scanner);
    
    /* 查找结束引号 */
    while (!vox_scanner_eof(scanner)) {
        int ch = vox_scanner_peek_char(scanner);
        if (ch < 0) {
            set_error(err_info, scanner, "Unterminated attribute value");
            return -1;
        }
        
        if (ch == quote) {
            break;
        }
        
        /* 处理转义字符（在 XML 中，& 和 < 需要转义，但这里简化处理） */
        if (ch == '&') {
            /* 跳过实体引用（简化处理：跳过直到分号） */
            vox_scanner_get_char(scanner);
            while (!vox_scanner_eof(scanner)) {
                ch = vox_scanner_peek_char(scanner);
                if (ch < 0 || ch == ';') {
                    if (ch == ';') {
                        vox_scanner_get_char(scanner);
                    }
                    break;
                }
                vox_scanner_get_char(scanner);
            }
        } else {
            vox_scanner_get_char(scanner);
        }
    }
    
    if (vox_scanner_eof(scanner)) {
        set_error(err_info, scanner, "Unterminated attribute value");
        return -1;
    }
    
    const char* end = vox_scanner_curptr(scanner);
    vox_scanner_get_char(scanner);  /* 跳过结束引号 */
    
    value->ptr = start;
    value->len = end - start;
    
    return 0;
}

/* 解析 XML 属性 */
static int parse_attr(vox_mpool_t* mpool, vox_scanner_t* scanner,
                      vox_xml_attr_t** attr, vox_xml_err_info_t* err_info) {
    vox_strview_t name, value;
    
    if (parse_name(scanner, &name, err_info) != 0) {
        return -1;
    }
    
    skip_whitespace(scanner);
    
    int ch = vox_scanner_peek_char(scanner);
    if (ch != '=') {
        set_error(err_info, scanner, "Expected '=' after attribute name");
        return -1;
    }
    
    vox_scanner_get_char(scanner);  /* 跳过 '=' */
    
    if (parse_attr_value(scanner, &value, err_info) != 0) {
        return -1;
    }
    
    vox_xml_attr_t* a = (vox_xml_attr_t*)vox_mpool_alloc(mpool, sizeof(vox_xml_attr_t));
    if (!a) {
        set_error(err_info, scanner, "Memory allocation failed");
        return -1;
    }
    
    memset(a, 0, sizeof(vox_xml_attr_t));
    a->name = name;
    a->value = value;
    a->parent = NULL;  /* 将在添加属性时设置 */
    vox_list_node_init(&a->node);
    
    *attr = a;
    return 0;
}

/* 跳过处理指令（<? ... ?>） */
static int skip_processing_instruction(vox_scanner_t* scanner,
                                        vox_xml_err_info_t* err_info) {
    /* 已经读取了 '<?' */
    while (!vox_scanner_eof(scanner)) {
        int ch = vox_scanner_peek_char(scanner);
        if (ch < 0) {
            set_error(err_info, scanner, "Unterminated processing instruction");
            return -1;
        }
        
        if (ch == '?') {
            vox_scanner_get_char(scanner);
            ch = vox_scanner_peek_char(scanner);
            if (ch == '>') {
                vox_scanner_get_char(scanner);
                return 0;
            }
        } else {
            vox_scanner_get_char(scanner);
        }
    }
    
    set_error(err_info, scanner, "Unterminated processing instruction");
    return -1;
}

/* 跳过注释（<!-- ... -->） */
static int skip_comment(vox_scanner_t* scanner, vox_xml_err_info_t* err_info) {
    /* 已经读取了 '<!--' */
    while (!vox_scanner_eof(scanner)) {
        int ch = vox_scanner_peek_char(scanner);
        if (ch < 0) {
            set_error(err_info, scanner, "Unterminated comment");
            return -1;
        }
        
        if (ch == '-') {
            vox_scanner_get_char(scanner);
            ch = vox_scanner_peek_char(scanner);
            if (ch == '-') {
                vox_scanner_get_char(scanner);
                ch = vox_scanner_peek_char(scanner);
                if (ch == '>') {
                    vox_scanner_get_char(scanner);
                    return 0;
                }
            }
        } else {
            vox_scanner_get_char(scanner);
        }
    }
    
    set_error(err_info, scanner, "Unterminated comment");
    return -1;
}

/* 跳过 CDATA 部分（<![CDATA[...]]>） */
static int skip_cdata(vox_scanner_t* scanner, vox_xml_err_info_t* err_info) {
    /* 已经读取了 '<![CDATA[' */
    while (!vox_scanner_eof(scanner)) {
        int ch = vox_scanner_peek_char(scanner);
        if (ch < 0) {
            set_error(err_info, scanner, "Unterminated CDATA section");
            return -1;
        }
        
        if (ch == ']') {
            vox_scanner_get_char(scanner);
            ch = vox_scanner_peek_char(scanner);
            if (ch == ']') {
                vox_scanner_get_char(scanner);
                ch = vox_scanner_peek_char(scanner);
                if (ch == '>') {
                    vox_scanner_get_char(scanner);
                    return 0;
                }
            }
        } else {
            vox_scanner_get_char(scanner);
        }
    }
    
    set_error(err_info, scanner, "Unterminated CDATA section");
    return -1;
}

/* 解析文本内容 */
static int parse_text_content(vox_scanner_t* scanner, vox_strview_t* content) {
    const char* start = vox_scanner_curptr(scanner);
    const char* end = start;
    
    while (!vox_scanner_eof(scanner)) {
        int ch = vox_scanner_peek_char(scanner);
        if (ch < 0) break;
        
        if (ch == '<') {
            break;
        }
        
        vox_scanner_get_char(scanner);
        end = vox_scanner_curptr(scanner);
    }
    
    /* 去除尾部空白 */
    while (end > start && isspace((unsigned char)(end[-1]))) {
        end--;
    }
    
    content->ptr = start;
    content->len = end - start;
    
    return 0;
}

/* 解析 XML 节点 */
static vox_xml_node_t* parse_node(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                  vox_xml_err_info_t* err_info);

/* 解析 XML 元素 */
static vox_xml_node_t* parse_element(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                      vox_xml_err_info_t* err_info) {
    /* 跳过 '<' */
    vox_scanner_get_char(scanner);
    
    skip_whitespace(scanner);
    
    /* 解析标签名 */
    vox_strview_t name;
    if (parse_name(scanner, &name, err_info) != 0) {
        return NULL;
    }
    
    /* 创建节点 */
    vox_xml_node_t* node = (vox_xml_node_t*)vox_mpool_alloc(mpool, sizeof(vox_xml_node_t));
    if (!node) {
        set_error(err_info, scanner, "Memory allocation failed");
        return NULL;
    }
    
    memset(node, 0, sizeof(vox_xml_node_t));
    node->name = name;
    vox_list_init(&node->children);
    vox_list_init(&node->attrs);
    node->parent = NULL;
    vox_list_node_init(&node->node);
    
    /* 解析属性 */
    skip_whitespace(scanner);
    while (!vox_scanner_eof(scanner)) {
        int ch = vox_scanner_peek_char(scanner);
        if (ch < 0) {
            set_error(err_info, scanner, "Unexpected end of input");
            vox_mpool_free(mpool, node);
            return NULL;
        }
        
        if (ch == '/' || ch == '>') {
            break;
        }
        
        vox_xml_attr_t* attr = NULL;
        if (parse_attr(mpool, scanner, &attr, err_info) != 0) {
            vox_mpool_free(mpool, node);
            return NULL;
        }
        
        attr->parent = node;  /* 设置父节点指针 */
        vox_list_push_back(&node->attrs, &attr->node);
        
        skip_whitespace(scanner);
    }
    
    /* 检查自闭合标签 */
    int ch = vox_scanner_peek_char(scanner);
    if (ch == '/') {
        vox_scanner_get_char(scanner);
        ch = vox_scanner_peek_char(scanner);
        if (ch != '>') {
            set_error(err_info, scanner, "Expected '>' after '/'");
            vox_mpool_free(mpool, node);
            return NULL;
        }
        vox_scanner_get_char(scanner);
        return node;  /* 自闭合标签，没有子节点 */
    }
    
    if (ch != '>') {
        set_error(err_info, scanner, "Expected '>' or '/>' after tag name");
        vox_mpool_free(mpool, node);
        return NULL;
    }
    
    vox_scanner_get_char(scanner);  /* 跳过 '>' */
    
    /* 解析子节点和文本内容 */
    while (!vox_scanner_eof(scanner)) {
        skip_whitespace(scanner);
        
        ch = vox_scanner_peek_char(scanner);
        if (ch < 0) {
            set_error(err_info, scanner, "Unexpected end of input");
            vox_mpool_free(mpool, node);
            return NULL;
        }
        
        if (ch == '<') {
            /* 检查是否是结束标签 */
            vox_scanner_state_t save;
            vox_scanner_save_state(scanner, &save);
            vox_scanner_get_char(scanner);
            
            ch = vox_scanner_peek_char(scanner);
            if (ch == '/') {
                /* 结束标签 */
                vox_scanner_get_char(scanner);
                
                skip_whitespace(scanner);
                
                vox_strview_t end_name;
                if (parse_name(scanner, &end_name, err_info) != 0) {
                    vox_mpool_free(mpool, node);
                    return NULL;
                }
                
                /* 检查标签名是否匹配 */
                if (end_name.len != name.len ||
                    memcmp(end_name.ptr, name.ptr, name.len) != 0) {
                    set_error(err_info, scanner, "Mismatched closing tag");
                    vox_mpool_free(mpool, node);
                    return NULL;
                }
                
                skip_whitespace(scanner);
                
                ch = vox_scanner_peek_char(scanner);
                if (ch != '>') {
                    set_error(err_info, scanner, "Expected '>' after closing tag name");
                    vox_mpool_free(mpool, node);
                    return NULL;
                }
                
                vox_scanner_get_char(scanner);
                break;  /* 结束标签解析完成 */
            } else {
                /* 子节点 */
                vox_scanner_restore_state(scanner, &save);
                vox_xml_node_t* child = parse_node(mpool, scanner, err_info);
                if (!child) {
                    vox_mpool_free(mpool, node);
                    return NULL;
                }
                
                child->parent = node;
                vox_list_push_back(&node->children, &child->node);
            }
        } else {
            /* 文本内容 */
            vox_strview_t text;
            if (parse_text_content(scanner, &text) == 0 && text.len > 0) {
                /* 如果已经有文本内容，追加（简化处理：只保留第一个文本片段） */
                if (node->content.len == 0) {
                    node->content = text;
                }
            }
        }
    }
    
    return node;
}

/* 解析 XML 节点 */
static vox_xml_node_t* parse_node(vox_mpool_t* mpool, vox_scanner_t* scanner,
                                  vox_xml_err_info_t* err_info) {
    skip_whitespace(scanner);
    
    int ch = vox_scanner_peek_char(scanner);
    if (ch < 0) {
        set_error(err_info, scanner, "Unexpected end of input");
        return NULL;
    }
    
    if (ch != '<') {
        set_error(err_info, scanner, "Expected '<' to start element");
        return NULL;
    }
    
    /* 检查是否是处理指令、注释或 CDATA */
    vox_scanner_state_t save;
    vox_scanner_save_state(scanner, &save);
    vox_scanner_get_char(scanner);  /* 跳过 '<' */
    
    ch = vox_scanner_peek_char(scanner);
    if (ch == '?') {
        /* 处理指令 */
        vox_scanner_get_char(scanner);
        if (skip_processing_instruction(scanner, err_info) != 0) {
            return NULL;
        }
        /* 递归解析下一个节点 */
        return parse_node(mpool, scanner, err_info);
    } else if (ch == '!') {
        vox_scanner_get_char(scanner);
        ch = vox_scanner_peek_char(scanner);
        if (ch == '-') {
            vox_scanner_get_char(scanner);
            ch = vox_scanner_peek_char(scanner);
            if (ch == '-') {
                /* 注释 */
                vox_scanner_get_char(scanner);
                if (skip_comment(scanner, err_info) != 0) {
                    return NULL;
                }
                /* 递归解析下一个节点 */
                return parse_node(mpool, scanner, err_info);
            }
        } else if (ch == '[') {
            /* 可能是 CDATA */
            if (vox_scanner_remaining(scanner) >= 7) {
                vox_strview_t peek;
                if (vox_scanner_peek(scanner, 7, &peek) == 0 &&
                    memcmp(peek.ptr, "[CDATA[", 7) == 0) {
                    vox_scanner_skip(scanner, 7);
                    if (skip_cdata(scanner, err_info) != 0) {
                        return NULL;
                    }
                    /* CDATA 内容被跳过，递归解析下一个节点 */
                    return parse_node(mpool, scanner, err_info);
                }
            }
        }
    }
    
    /* 恢复状态，解析元素 */
    vox_scanner_restore_state(scanner, &save);
    return parse_element(mpool, scanner, err_info);
}

/* ===== 公共接口实现 ===== */

/* 解析 XML */
vox_xml_node_t* vox_xml_parse(vox_mpool_t* mpool, char* buffer, size_t* size,
                              vox_xml_err_info_t* err_info) {
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
    if (vox_scanner_init(&scanner, buffer, buf_len, 0) != 0) {
        if (err_info) {
            err_info->line = 0;
            err_info->column = 0;
            err_info->offset = 0;
            err_info->message = "Failed to initialize scanner";
        }
        return NULL;
    }
    
    vox_xml_node_t* root = parse_node(mpool, &scanner, err_info);
    
    if (root) {
        if (size) {
            *size = vox_scanner_offset(&scanner);
        }
    }
    
    vox_scanner_destroy(&scanner);
    return root;
}

/* 从 C 字符串解析 */
vox_xml_node_t* vox_xml_parse_str(vox_mpool_t* mpool, const char* xml_str,
                                  vox_xml_err_info_t* err_info) {
    if (!mpool || !xml_str) {
        if (err_info) {
            err_info->line = 0;
            err_info->column = 0;
            err_info->offset = 0;
            err_info->message = "Invalid parameters";
        }
        return NULL;
    }
    
    size_t len = strlen(xml_str);
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
    
    memcpy(buffer, xml_str, len);
    buffer[len] = '\0';
    
    size_t size = len;
    vox_xml_node_t* root = vox_xml_parse(mpool, buffer, &size, err_info);
    
    return root;
}

vox_xml_node_t* vox_xml_parse_file(vox_mpool_t* mpool, const char* filepath,
                                   vox_xml_err_info_t* err_info) {
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
    vox_xml_node_t* root = vox_xml_parse(mpool, buffer, &parse_size, err_info);

    return root;
}

/* ===== 节点创建接口 ===== */

vox_xml_node_t* vox_xml_node_new(vox_mpool_t* mpool, const vox_strview_t* name) {
    if (!mpool || !name) {
        return NULL;
    }
    
    vox_xml_node_t* node = (vox_xml_node_t*)vox_mpool_alloc(mpool, sizeof(vox_xml_node_t));
    if (!node) {
        return NULL;
    }
    
    memset(node, 0, sizeof(vox_xml_node_t));
    node->name = *name;
    vox_list_init(&node->children);
    vox_list_init(&node->attrs);
    node->parent = NULL;
    vox_list_node_init(&node->node);
    
    return node;
}

vox_xml_attr_t* vox_xml_attr_new(vox_mpool_t* mpool, const vox_strview_t* name,
                                  const vox_strview_t* value) {
    if (!mpool || !name || !value) {
        return NULL;
    }
    
    vox_xml_attr_t* attr = (vox_xml_attr_t*)vox_mpool_alloc(mpool, sizeof(vox_xml_attr_t));
    if (!attr) {
        return NULL;
    }
    
    memset(attr, 0, sizeof(vox_xml_attr_t));
    attr->name = *name;
    attr->value = *value;
    attr->parent = NULL;  /* 将在添加属性时设置 */
    vox_list_node_init(&attr->node);
    
    return attr;
}

vox_xml_node_t* vox_xml_clone(vox_mpool_t* mpool, const vox_xml_node_t* src) {
    if (!mpool || !src) {
        return NULL;
    }
    
    vox_xml_node_t* node = vox_xml_node_new(mpool, &src->name);
    if (!node) {
        return NULL;
    }
    
    node->content = src->content;
    
    /* 克隆属性 */
    vox_xml_attr_t* attr;
    vox_list_for_each_entry(attr, &src->attrs, vox_xml_attr_t, node) {
        vox_xml_attr_t* new_attr = vox_xml_attr_new(mpool, &attr->name, &attr->value);
        if (!new_attr) {
            return NULL;
        }
        new_attr->parent = node;  /* 设置父节点指针 */
        vox_list_push_back(&node->attrs, &new_attr->node);
    }
    
    /* 克隆子节点 */
    vox_xml_node_t* child;
    vox_list_for_each_entry(child, &src->children, vox_xml_node_t, node) {
        vox_xml_node_t* new_child = vox_xml_clone(mpool, child);
        if (!new_child) {
            return NULL;
        }
        new_child->parent = node;
        vox_list_push_back(&node->children, &new_child->node);
    }
    
    return node;
}

/* ===== 节点操作接口 ===== */

void vox_xml_add_child(vox_xml_node_t* parent, vox_xml_node_t* child) {
    if (!parent || !child) {
        return;
    }
    
    child->parent = parent;
    vox_list_push_back(&parent->children, &child->node);
}

void vox_xml_add_attr(vox_xml_node_t* node, vox_xml_attr_t* attr) {
    if (!node || !attr) {
        return;
    }
    
    attr->parent = node;  /* 设置父节点指针 */
    vox_list_push_back(&node->attrs, &attr->node);
}

void vox_xml_set_content(vox_xml_node_t* node, const vox_strview_t* content) {
    if (!node || !content) {
        return;
    }
    
    node->content = *content;
}

/* ===== 查询接口 ===== */

vox_strview_t vox_xml_get_name(const vox_xml_node_t* node) {
    vox_strview_t null_view = VOX_STRVIEW_NULL;
    if (!node) {
        return null_view;
    }
    return node->name;
}

vox_strview_t vox_xml_get_content(const vox_xml_node_t* node) {
    vox_strview_t null_view = VOX_STRVIEW_NULL;
    if (!node) {
        return null_view;
    }
    return node->content;
}

size_t vox_xml_get_child_count(const vox_xml_node_t* node) {
    if (!node) {
        return 0;
    }
    return vox_list_size(&node->children);
}

size_t vox_xml_get_attr_count(const vox_xml_node_t* node) {
    if (!node) {
        return 0;
    }
    return vox_list_size(&node->attrs);
}

vox_xml_node_t* vox_xml_find_child(const vox_xml_node_t* node, const char* name) {
    if (!node || !name) {
        return NULL;
    }
    
    size_t name_len = strlen(name);
    vox_xml_node_t* child;
    vox_list_for_each_entry(child, &node->children, vox_xml_node_t, node) {
        if (child->name.len == name_len &&
            memcmp(child->name.ptr, name, name_len) == 0) {
            return child;
        }
    }
    
    return NULL;
}

vox_xml_attr_t* vox_xml_find_attr(const vox_xml_node_t* node, const char* name) {
    if (!node || !name) {
        return NULL;
    }
    
    size_t name_len = strlen(name);
    vox_xml_attr_t* attr;
    vox_list_for_each_entry(attr, &node->attrs, vox_xml_attr_t, node) {
        if (attr->name.len == name_len &&
            memcmp(attr->name.ptr, name, name_len) == 0) {
            return attr;
        }
    }
    
    return NULL;
}

vox_strview_t vox_xml_get_attr_value(const vox_xml_node_t* node, const char* name) {
    vox_strview_t null_view = VOX_STRVIEW_NULL;
    vox_xml_attr_t* attr = vox_xml_find_attr(node, name);
    if (!attr) {
        return null_view;
    }
    return attr->value;
}

/* ===== 遍历接口 ===== */

vox_xml_node_t* vox_xml_first_child(const vox_xml_node_t* node) {
    if (!node) {
        return NULL;
    }
    
    vox_list_node_t* first = vox_list_first(&node->children);
    if (!first) {
        return NULL;
    }
    
    return vox_container_of(first, vox_xml_node_t, node);
}

vox_xml_node_t* vox_xml_next_child(const vox_xml_node_t* child) {
    if (!child) {
        return NULL;
    }
    
    vox_list_node_t* next = child->node.next;
    if (!next || next == &child->parent->children.head) {
        return NULL;
    }
    
    return vox_container_of(next, vox_xml_node_t, node);
}

vox_xml_attr_t* vox_xml_first_attr(const vox_xml_node_t* node) {
    if (!node) {
        return NULL;
    }
    
    vox_list_node_t* first = vox_list_first(&node->attrs);
    if (!first) {
        return NULL;
    }
    
    return vox_container_of(first, vox_xml_attr_t, node);
}

vox_xml_attr_t* vox_xml_next_attr(const vox_xml_attr_t* attr) {
    if (!attr || !attr->node.next || !attr->parent) {
        return NULL;
    }
    
    vox_list_node_t* next = attr->node.next;
    
    /* 检查是否到达链表末尾：使用父节点的 head 指针 */
    if (next == &attr->parent->attrs.head) {
        return NULL;
    }
    
    return vox_container_of(next, vox_xml_attr_t, node);
}

/* ===== 输出接口 ===== */

/* 转义 XML 特殊字符 */
static size_t escape_xml(const char* src, size_t src_len, char* dst, size_t dst_size) {
    size_t written = 0;
    const char* end = src + src_len;
    
    for (const char* p = src; p < end; p++) {
        switch (*p) {
            case '<':
                if (written + 4 > dst_size) {
                    return (size_t)-1;  /* 缓冲区不足 */
                }
                memcpy(dst + written, "&lt;", 4);
                written += 4;
                break;
            case '>':
                if (written + 4 > dst_size) {
                    return (size_t)-1;  /* 缓冲区不足 */
                }
                memcpy(dst + written, "&gt;", 4);
                written += 4;
                break;
            case '&':
                if (written + 5 > dst_size) {
                    return (size_t)-1;  /* 缓冲区不足 */
                }
                memcpy(dst + written, "&amp;", 5);
                written += 5;
                break;
            case '"':
                if (written + 6 > dst_size) {
                    return (size_t)-1;  /* 缓冲区不足 */
                }
                memcpy(dst + written, "&quot;", 6);
                written += 6;
                break;
            case '\'':
                if (written + 6 > dst_size) {
                    return (size_t)-1;  /* 缓冲区不足 */
                }
                memcpy(dst + written, "&apos;", 6);
                written += 6;
                break;
            default:
                if (written >= dst_size) {
                    return (size_t)-1;  /* 缓冲区不足 */
                }
                dst[written++] = *p;
                break;
        }
    }
    
    return written;
}

/* 递归打印节点 */
static int print_node_recursive(const vox_xml_node_t* node, char* buffer, size_t* size,
                                int indent, bool prolog) {
    (void)prolog;  /* 未使用的参数，保留用于将来扩展 */
    size_t written = 0;
    size_t remaining = *size;
    
    /* 打印开始标签 */
    if (written < remaining) {
        buffer[written++] = '<';
        remaining--;
    } else {
        return -1;
    }
    
    /* 打印标签名 */
    size_t name_len = node->name.len;
    if (written + name_len <= remaining) {
        memcpy(buffer + written, node->name.ptr, name_len);
        written += name_len;
        remaining -= name_len;
    } else {
        return -1;
    }
    
    /* 打印属性 */
    vox_xml_attr_t* attr;
    vox_list_for_each_entry(attr, &node->attrs, vox_xml_attr_t, node) {
        if (written >= remaining) {
            return -1;
        }
        buffer[written++] = ' ';
        remaining--;
        
        size_t attr_name_len = attr->name.len;
        if (written + attr_name_len <= remaining) {
            memcpy(buffer + written, attr->name.ptr, attr_name_len);
            written += attr_name_len;
            remaining -= attr_name_len;
        } else {
            return -1;
        }
        
        if (written >= remaining) {
            return -1;
        }
        buffer[written++] = '=';
        remaining--;
        
        if (written >= remaining) {
            return -1;
        }
        buffer[written++] = '"';
        remaining--;
        
        /* 转义属性值 */
        size_t escaped_len = escape_xml(attr->value.ptr, attr->value.len,
                                       buffer + written, remaining);
        if (escaped_len == (size_t)-1) {
            return -1;
        }
        written += escaped_len;
        remaining -= escaped_len;
        
        if (written >= remaining) {
            return -1;
        }
        buffer[written++] = '"';
        remaining--;
    }
    
    /* 检查是否有子节点或文本内容 */
    bool has_children = !vox_list_empty(&node->children);
    bool has_content = node->content.len > 0;
    
    if (!has_children && !has_content) {
        /* 自闭合标签 */
        if (written >= remaining) {
            return -1;
        }
        buffer[written++] = '/';
        remaining--;
        
        if (written >= remaining) {
            return -1;
        }
        buffer[written++] = '>';
        remaining--;
    } else {
        /* 闭合标签 */
        if (written >= remaining) {
            return -1;
        }
        buffer[written++] = '>';
        remaining--;
        
        /* 打印文本内容 */
        if (has_content) {
            size_t content_escaped = escape_xml(node->content.ptr, node->content.len,
                                                buffer + written, remaining);
            if (content_escaped == (size_t)-1) {
                return -1;
            }
            written += content_escaped;
            remaining -= content_escaped;
        }
        
        /* 打印子节点 */
        vox_xml_node_t* child;
        vox_list_for_each_entry(child, &node->children, vox_xml_node_t, node) {
            size_t child_size = remaining;
            int child_written = print_node_recursive(child, buffer + written, &child_size,
                                                     indent + 1, false);
            if (child_written < 0) {
                return -1;
            }
            written += child_written;
            remaining = child_size;  /* child_size 现在包含剩余空间 */
        }
        
        /* 打印结束标签 */
        if (written >= remaining) {
            return -1;
        }
        buffer[written++] = '<';
        remaining--;
        
        if (written >= remaining) {
            return -1;
        }
        buffer[written++] = '/';
        remaining--;
        
        if (written + name_len <= remaining) {
            memcpy(buffer + written, node->name.ptr, name_len);
            written += name_len;
            remaining -= name_len;
        } else {
            return -1;
        }
        
        if (written >= remaining) {
            return -1;
        }
        buffer[written++] = '>';
        remaining--;
    }
    
    *size = remaining;  /* 更新剩余空间 */
    return (int)written;
}

int vox_xml_print(const vox_xml_node_t* node, char* buffer, size_t* size, bool prolog) {
    if (!node || !buffer || !size) {
        return -1;
    }
    
    size_t remaining = *size;
    size_t written = 0;
    
    /* 打印 XML 声明 */
    if (prolog) {
        const char* prolog_str = "<?xml version=\"1.0\"?>";
        size_t prolog_len = strlen(prolog_str);
        if (prolog_len > remaining) {
            return -1;
        }
        memcpy(buffer, prolog_str, prolog_len);
        written = prolog_len;
        remaining -= prolog_len;
    }
    
    /* 打印节点 */
    size_t node_size = remaining;
    int node_written = print_node_recursive(node, buffer + written, &node_size, 0, false);
    if (node_written < 0) {
        return -1;
    }
    written += node_written;
    remaining = node_size;  /* node_size 现在包含剩余空间 */
    
    *size = written;
    return (int)written;
}

int vox_xml_write_file(vox_mpool_t* mpool, const vox_xml_node_t* node, const char* filepath, bool prolog) {
    if (!mpool || !node || !filepath) {
        return -1;
    }

    /* 逐步扩容缓冲区，直到 vox_xml_print 成功 */
    size_t capacity = 1024;
    char* buffer = (char*)vox_mpool_alloc(mpool, capacity);
    if (!buffer) {
        return -1;
    }

    while (1) {
        size_t size = capacity;
        int written = vox_xml_print(node, buffer, &size, prolog);
        if (written >= 0) {
            /* 写入文件，size 是实际写入字节数 */
            return vox_file_write_all(mpool, filepath, buffer, size);
        }

        /* 容量不足，尝试扩容 */
        size_t new_capacity = capacity * 2;
        char* new_buf = (char*)vox_mpool_realloc(mpool, buffer, new_capacity);
        if (!new_buf) {
            return -1;
        }
        buffer = new_buf;
        capacity = new_capacity;
    }
}

void vox_xml_print_debug(const vox_xml_node_t* node, int indent) {
    if (!node) {
        return;
    }
    
    /* 打印缩进 */
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    
    /* 打印节点名 */
    printf("<%.*s", (int)node->name.len, node->name.ptr);
    
    /* 打印属性 */
    vox_xml_attr_t* attr;
    vox_list_for_each_entry(attr, &node->attrs, vox_xml_attr_t, node) {
        printf(" %.*s=\"%.*s\"", (int)attr->name.len, attr->name.ptr,
               (int)attr->value.len, attr->value.ptr);
    }
    
    printf(">\n");
    
    /* 打印文本内容 */
    if (node->content.len > 0) {
        for (int i = 0; i < indent + 1; i++) {
            printf("  ");
        }
        printf("%.*s\n", (int)node->content.len, node->content.ptr);
    }
    
    /* 打印子节点 */
    vox_xml_node_t* child;
    vox_list_for_each_entry(child, &node->children, vox_xml_node_t, node) {
        vox_xml_print_debug(child, indent + 1);
    }
    
    /* 打印结束标签 */
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    printf("</%.*s>\n", (int)node->name.len, node->name.ptr);
}
