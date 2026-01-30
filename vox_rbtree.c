/*
 * vox_rbtree.c - 高性能红黑树实现
 * 使用 vox_mpool 内存池管理所有内存分配
 */

#include "vox_rbtree.h"
#include "vox_mpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* 节点颜色 */
#define VOX_RBTREE_RED   0
#define VOX_RBTREE_BLACK 1

/* 红黑树节点 */
typedef struct vox_rbtree_node {
    uint8_t color;          /* 颜色：RED 或 BLACK */
    struct vox_rbtree_node* parent;
    struct vox_rbtree_node* left;
    struct vox_rbtree_node* right;
    void* key;               /* 键指针 */
    size_t key_len;          /* 键长度 */
    void* value;             /* 值指针 */
} vox_rbtree_node_t;

/* 红黑树结构 */
struct vox_rbtree {
    vox_mpool_t* mpool;      /* 内存池 */
    vox_rbtree_node_t* root; /* 根节点 */
    size_t size;             /* 元素数量 */
    vox_key_cmp_func_t key_cmp;   /* 键比较函数 */
    vox_key_free_func_t key_free; /* 键释放函数 */
    vox_value_free_func_t value_free; /* 值释放函数 */
};

/* 默认键比较函数 */
static int default_key_cmp(const void* key1, const void* key2, size_t key_len) {
    return memcmp(key1, key2, key_len);
}

/* 获取节点颜色 */
static inline uint8_t get_color(const vox_rbtree_node_t* node) {
    return node ? node->color : VOX_RBTREE_BLACK;
}

/* 设置节点颜色 */
static inline void set_color(vox_rbtree_node_t* node, uint8_t color) {
    if (node) {
        node->color = color;
    }
}

/* 判断节点是否为红色 */
static inline bool is_red(const vox_rbtree_node_t* node) {
    return node && node->color == VOX_RBTREE_RED;
}

/* 判断节点是否为黑色 */
static inline bool is_black(const vox_rbtree_node_t* node) {
    return !node || node->color == VOX_RBTREE_BLACK;
}

/* 左旋 */
static void left_rotate(vox_rbtree_t* tree, vox_rbtree_node_t* x) {
    vox_rbtree_node_t* y = x->right;
    x->right = y->left;
    
    if (y->left) {
        y->left->parent = x;
    }
    
    y->parent = x->parent;
    
    if (!x->parent) {
        tree->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    
    y->left = x;
    x->parent = y;
}

/* 右旋 */
static void right_rotate(vox_rbtree_t* tree, vox_rbtree_node_t* y) {
    vox_rbtree_node_t* x = y->left;
    y->left = x->right;
    
    if (x->right) {
        x->right->parent = y;
    }
    
    x->parent = y->parent;
    
    if (!y->parent) {
        tree->root = x;
    } else if (y == y->parent->left) {
        y->parent->left = x;
    } else {
        y->parent->right = x;
    }
    
    x->right = y;
    y->parent = x;
}

/* 查找节点（内部函数） */
static vox_rbtree_node_t* find_node(vox_rbtree_t* tree, const void* key, size_t key_len) {
    vox_rbtree_node_t* node = tree->root;
    
    while (node) {
        /* 先比较长度，如果长度不同，长度短的键小于长度长的键 */
        if (key_len != node->key_len) {
            if (key_len < node->key_len) {
                node = node->left;
            } else {
                node = node->right;
            }
            continue;
        }
        
        /* 长度相同，使用比较函数比较内容 */
        int cmp = tree->key_cmp(key, node->key, key_len);
        if (cmp == 0) {
            return node;
        } else if (cmp < 0) {
            node = node->left;
        } else {
            node = node->right;
        }
    }
    
    return NULL;
}

/* 查找最小节点 */
static vox_rbtree_node_t* min_node(vox_rbtree_node_t* node) {
    while (node && node->left) {
        node = node->left;
    }
    return node;
}

/* 查找最大节点 */
static vox_rbtree_node_t* max_node(vox_rbtree_node_t* node) {
    while (node && node->right) {
        node = node->right;
    }
    return node;
}

/* 插入后修复红黑树性质 */
static void insert_fixup(vox_rbtree_t* tree, vox_rbtree_node_t* node) {
    while (node != tree->root && is_red(node->parent)) {
        if (node->parent == node->parent->parent->left) {
            /* 父节点是祖父节点的左子节点 */
            vox_rbtree_node_t* uncle = node->parent->parent->right;
            
            if (is_red(uncle)) {
                /* 情况1：叔叔节点是红色 */
                set_color(node->parent, VOX_RBTREE_BLACK);
                set_color(uncle, VOX_RBTREE_BLACK);
                set_color(node->parent->parent, VOX_RBTREE_RED);
                node = node->parent->parent;
            } else {
                /* 情况2和3：叔叔节点是黑色 */
                if (node == node->parent->right) {
                    /* 情况2：节点是父节点的右子节点 */
                    node = node->parent;
                    left_rotate(tree, node);
                }
                /* 情况3：节点是父节点的左子节点 */
                set_color(node->parent, VOX_RBTREE_BLACK);
                set_color(node->parent->parent, VOX_RBTREE_RED);
                right_rotate(tree, node->parent->parent);
            }
        } else {
            /* 父节点是祖父节点的右子节点 */
            vox_rbtree_node_t* uncle = node->parent->parent->left;
            
            if (is_red(uncle)) {
                /* 情况1：叔叔节点是红色 */
                set_color(node->parent, VOX_RBTREE_BLACK);
                set_color(uncle, VOX_RBTREE_BLACK);
                set_color(node->parent->parent, VOX_RBTREE_RED);
                node = node->parent->parent;
            } else {
                /* 情况2和3：叔叔节点是黑色 */
                if (node == node->parent->left) {
                    /* 情况2：节点是父节点的左子节点 */
                    node = node->parent;
                    right_rotate(tree, node);
                }
                /* 情况3：节点是父节点的右子节点 */
                set_color(node->parent, VOX_RBTREE_BLACK);
                set_color(node->parent->parent, VOX_RBTREE_RED);
                left_rotate(tree, node->parent->parent);
            }
        }
    }
    
    set_color(tree->root, VOX_RBTREE_BLACK);
}

/* 删除后修复红黑树性质 */
static void delete_fixup(vox_rbtree_t* tree, vox_rbtree_node_t* node, vox_rbtree_node_t* parent, bool is_left) {
    while (node != tree->root && is_black(node) && parent) {
        if (is_left) {
            /* 节点是父节点的左子节点 */
            vox_rbtree_node_t* sibling = parent->right;
            
            if (is_red(sibling)) {
                /* 情况1：兄弟节点是红色 */
                set_color(sibling, VOX_RBTREE_BLACK);
                set_color(parent, VOX_RBTREE_RED);
                left_rotate(tree, parent);
                sibling = parent->right;
            }
            
            if (!sibling || (is_black(sibling->left) && is_black(sibling->right))) {
                /* 情况2：兄弟节点的两个子节点都是黑色（或兄弟节点为NULL） */
                if (sibling) {
                    set_color(sibling, VOX_RBTREE_RED);
                }
                node = parent;
                parent = node ? node->parent : NULL;
                is_left = (node && parent && node == parent->left);
            } else {
                if (sibling && is_black(sibling->right)) {
                    /* 情况3：兄弟节点的右子节点是黑色，左子节点是红色 */
                    if (sibling->left) {
                        set_color(sibling->left, VOX_RBTREE_BLACK);
                    }
                    set_color(sibling, VOX_RBTREE_RED);
                    right_rotate(tree, sibling);
                    sibling = parent->right;
                }
                /* 情况4：兄弟节点的右子节点是红色 */
                if (sibling) {
                    set_color(sibling, get_color(parent));
                    set_color(parent, VOX_RBTREE_BLACK);
                    if (sibling->right) {
                        set_color(sibling->right, VOX_RBTREE_BLACK);
                    }
                    left_rotate(tree, parent);
                }
                node = tree->root;
                is_left = false;
            }
        } else {
            /* 节点是父节点的右子节点 */
            vox_rbtree_node_t* sibling = parent->left;
            
            if (is_red(sibling)) {
                /* 情况1：兄弟节点是红色 */
                set_color(sibling, VOX_RBTREE_BLACK);
                set_color(parent, VOX_RBTREE_RED);
                right_rotate(tree, parent);
                sibling = parent->left;
            }
            
            if (!sibling || (is_black(sibling->left) && is_black(sibling->right))) {
                /* 情况2：兄弟节点的两个子节点都是黑色（或兄弟节点为NULL） */
                if (sibling) {
                    set_color(sibling, VOX_RBTREE_RED);
                }
                node = parent;
                parent = node ? node->parent : NULL;
                is_left = (node && parent && node == parent->left);
            } else {
                if (sibling && is_black(sibling->left)) {
                    /* 情况3：兄弟节点的左子节点是黑色，右子节点是红色 */
                    if (sibling->right) {
                        set_color(sibling->right, VOX_RBTREE_BLACK);
                    }
                    set_color(sibling, VOX_RBTREE_RED);
                    left_rotate(tree, sibling);
                    sibling = parent->left;
                }
                /* 情况4：兄弟节点的左子节点是红色 */
                if (sibling) {
                    set_color(sibling, get_color(parent));
                    set_color(parent, VOX_RBTREE_BLACK);
                    if (sibling->left) {
                        set_color(sibling->left, VOX_RBTREE_BLACK);
                    }
                    right_rotate(tree, parent);
                }
                node = tree->root;
                is_left = false;
            }
        }
    }
    
    set_color(node, VOX_RBTREE_BLACK);
}

/* 替换节点（用于删除） */
static void transplant(vox_rbtree_t* tree, vox_rbtree_node_t* u, vox_rbtree_node_t* v) {
    if (!u->parent) {
        tree->root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    
    if (v) {
        v->parent = u->parent;
    }
}

/* 创建红黑树 */
vox_rbtree_t* vox_rbtree_create(vox_mpool_t* mpool) {
    return vox_rbtree_create_with_config(mpool, NULL);
}

/* 使用配置创建红黑树 */
vox_rbtree_t* vox_rbtree_create_with_config(vox_mpool_t* mpool, const vox_rbtree_config_t* config) {
    if (!mpool) return NULL;
    
    /* 使用内存池分配红黑树结构 */
    vox_rbtree_t* tree = (vox_rbtree_t*)vox_mpool_alloc(mpool, sizeof(vox_rbtree_t));
    if (!tree) {
        return NULL;
    }
    
    /* 初始化结构 */
    memset(tree, 0, sizeof(vox_rbtree_t));
    tree->mpool = mpool;
    tree->key_cmp = default_key_cmp;
    
    /* 应用配置 */
    if (config) {
        if (config->key_cmp) {
            tree->key_cmp = config->key_cmp;
        }
        if (config->key_free) {
            tree->key_free = config->key_free;
        }
        if (config->value_free) {
            tree->value_free = config->value_free;
        }
    }
    
    return tree;
}

/* 插入或更新键值对 */
int vox_rbtree_insert(vox_rbtree_t* tree, const void* key, size_t key_len, void* value) {
    if (!tree || !key || key_len == 0) return -1;
    
    /* 查找插入位置 */
    vox_rbtree_node_t* parent = NULL;
    vox_rbtree_node_t* node = tree->root;
    int cmp = 0;
    
    while (node) {
        parent = node;
        
        /* 先比较长度，如果长度不同，长度短的键小于长度长的键 */
        if (key_len != node->key_len) {
            if (key_len < node->key_len) {
                cmp = -1;
                node = node->left;
            } else {
                cmp = 1;
                node = node->right;
            }
            continue;
        }
        
        /* 长度相同，使用比较函数比较内容 */
        cmp = tree->key_cmp(key, node->key, key_len);
        if (cmp == 0) {
            /* 键已存在，更新值 */
            if (tree->value_free && node->value) {
                tree->value_free(node->value);
            }
            node->value = value;
            return 0;
        } else if (cmp < 0) {
            node = node->left;
        } else {
            node = node->right;
        }
    }
    
    /* 创建新节点 */
    vox_rbtree_node_t* new_node = (vox_rbtree_node_t*)vox_mpool_alloc(tree->mpool, sizeof(vox_rbtree_node_t));
    if (!new_node) return -1;
    
    memset(new_node, 0, sizeof(vox_rbtree_node_t));
    
    /* 分配并复制键 */
    new_node->key = vox_mpool_alloc(tree->mpool, key_len);
    if (!new_node->key) {
        vox_mpool_free(tree->mpool, new_node);
        return -1;
    }
    memcpy(new_node->key, key, key_len);
    new_node->key_len = key_len;
    new_node->value = value;
    new_node->color = VOX_RBTREE_RED;
    new_node->parent = parent;
    
    /* 插入节点 */
    if (!parent) {
        tree->root = new_node;
    } else if (cmp < 0) {
        parent->left = new_node;
    } else {
        parent->right = new_node;
    }
    
    tree->size++;
    
    /* 修复红黑树性质 */
    insert_fixup(tree, new_node);
    
    return 0;
}

/* 查找值 */
void* vox_rbtree_find(const vox_rbtree_t* tree, const void* key, size_t key_len) {
    if (!tree || !key || key_len == 0) return NULL;
    
    vox_rbtree_node_t* node = find_node((vox_rbtree_t*)tree, key, key_len);
    return node ? node->value : NULL;
}

/* 删除键值对 */
int vox_rbtree_delete(vox_rbtree_t* tree, const void* key, size_t key_len) {
    if (!tree || !key || key_len == 0) return -1;
    
    vox_rbtree_node_t* node = find_node(tree, key, key_len);
    if (!node) return -1;
    
    vox_rbtree_node_t* y = node;
    vox_rbtree_node_t* x = NULL;
    uint8_t y_original_color = y->color;
    
    /* 保存要释放的键和值 */
    void* key_to_free = node->key;
    void* value_to_free = node->value;
    
    bool is_left_child = false;  /* 记录被删除节点是左子节点还是右子节点 */
    if (node->parent) {
        is_left_child = (node == node->parent->left);
    }
    
    if (!node->left) {
        /* 节点没有左子节点 */
        x = node->right;
        transplant(tree, node, node->right);
    } else if (!node->right) {
        /* 节点没有右子节点 */
        x = node->left;
        transplant(tree, node, node->left);
    } else {
        /* 节点有两个子节点 */
        y = min_node(node->right);
        y_original_color = y->color;
        x = y->right;
        
        if (y->parent != node) {
            transplant(tree, y, y->right);
            y->right = node->right;
            if (y->right) {
                y->right->parent = y;
            }
        } else {
            if (x) x->parent = y;
        }
        
        transplant(tree, node, y);
        y->left = node->left;
        if (y->left) {
            y->left->parent = y;
        }
        set_color(y, node->color);
    }
    
    /* 释放节点资源 */
    if (tree->key_free && key_to_free) {
        tree->key_free(key_to_free);
    }
    if (tree->value_free && value_to_free) {
        tree->value_free(value_to_free);
    }
    
    /* 释放键内存和节点内存 */
    vox_mpool_free(tree->mpool, key_to_free);
    vox_mpool_free(tree->mpool, node);
    
    tree->size--;
    
    /* 如果删除的是黑色节点，需要修复红黑树性质 */
    if (y_original_color == VOX_RBTREE_BLACK) {
        vox_rbtree_node_t* fixup_parent = NULL;
        bool fixup_is_left = false;
        
        if (x) {
            fixup_parent = x->parent;
            fixup_is_left = (fixup_parent && x == fixup_parent->left);
        } else {
            /* x为NULL时，需要找到正确的父节点 */
            if (y->parent == node) {
                fixup_parent = y;
                fixup_is_left = is_left_child;
            } else {
                fixup_parent = y->parent;
                fixup_is_left = (fixup_parent && y == fixup_parent->left);
            }
        }
        delete_fixup(tree, x, fixup_parent, fixup_is_left);
    }
    
    return 0;
}

/* 检查键是否存在 */
bool vox_rbtree_contains(const vox_rbtree_t* tree, const void* key, size_t key_len) {
    return vox_rbtree_find(tree, key, key_len) != NULL;
}

/* 获取元素数量 */
size_t vox_rbtree_size(const vox_rbtree_t* tree) {
    return tree ? tree->size : 0;
}

/* 检查是否为空 */
bool vox_rbtree_empty(const vox_rbtree_t* tree) {
    return tree ? (tree->size == 0) : true;
}

/* 清空红黑树 */
void vox_rbtree_clear(vox_rbtree_t* tree) {
    if (!tree) return;
    
    /* 递归删除所有节点 */
    while (tree->root) {
        vox_rbtree_delete(tree, tree->root->key, tree->root->key_len);
    }
}

/* 销毁红黑树 */
void vox_rbtree_destroy(vox_rbtree_t* tree) {
    if (!tree) return;
    
    /* 清空所有节点 */
    vox_rbtree_clear(tree);
    
    /* 保存内存池指针 */
    vox_mpool_t* mpool = tree->mpool;
    
    /* 释放红黑树结构 */
    vox_mpool_free(mpool, tree);
}

/* 中序遍历辅助函数 */
static size_t inorder_recursive(vox_rbtree_node_t* node, vox_rbtree_visit_func_t visit, void* user_data) {
    if (!node) return 0;
    
    size_t count = 0;
    count += inorder_recursive(node->left, visit, user_data);
    visit(node->key, node->key_len, node->value, user_data);
    count++;
    count += inorder_recursive(node->right, visit, user_data);
    return count;
}

/* 前序遍历辅助函数 */
static size_t preorder_recursive(vox_rbtree_node_t* node, vox_rbtree_visit_func_t visit, void* user_data) {
    if (!node) return 0;
    
    size_t count = 0;
    visit(node->key, node->key_len, node->value, user_data);
    count++;
    count += preorder_recursive(node->left, visit, user_data);
    count += preorder_recursive(node->right, visit, user_data);
    return count;
}

/* 后序遍历辅助函数 */
static size_t postorder_recursive(vox_rbtree_node_t* node, vox_rbtree_visit_func_t visit, void* user_data) {
    if (!node) return 0;
    
    size_t count = 0;
    count += postorder_recursive(node->left, visit, user_data);
    count += postorder_recursive(node->right, visit, user_data);
    visit(node->key, node->key_len, node->value, user_data);
    count++;
    return count;
}

/* 中序遍历 */
size_t vox_rbtree_inorder(vox_rbtree_t* tree, vox_rbtree_visit_func_t visit, void* user_data) {
    if (!tree || !visit) return 0;
    return inorder_recursive(tree->root, visit, user_data);
}

/* 前序遍历 */
size_t vox_rbtree_preorder(vox_rbtree_t* tree, vox_rbtree_visit_func_t visit, void* user_data) {
    if (!tree || !visit) return 0;
    return preorder_recursive(tree->root, visit, user_data);
}

/* 后序遍历 */
size_t vox_rbtree_postorder(vox_rbtree_t* tree, vox_rbtree_visit_func_t visit, void* user_data) {
    if (!tree || !visit) return 0;
    return postorder_recursive(tree->root, visit, user_data);
}

/* 获取最小键 */
int vox_rbtree_min(const vox_rbtree_t* tree, const void** key_out, size_t* key_len_out) {
    if (!tree || !tree->root) return -1;
    
    vox_rbtree_node_t* min = min_node(tree->root);
    if (min) {
        if (key_out) *key_out = min->key;
        if (key_len_out) *key_len_out = min->key_len;
        return 0;
    }
    return -1;
}

/* 获取最大键 */
int vox_rbtree_max(const vox_rbtree_t* tree, const void** key_out, size_t* key_len_out) {
    if (!tree || !tree->root) return -1;
    
    vox_rbtree_node_t* max = max_node(tree->root);
    if (max) {
        if (key_out) *key_out = max->key;
        if (key_len_out) *key_len_out = max->key_len;
        return 0;
    }
    return -1;
}
