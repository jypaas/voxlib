/*
 * vox_rbtree.h - 高性能红黑树
 * 使用 vox_mpool 内存池管理所有内存分配
 */

#ifndef VOX_RBTREE_H
#define VOX_RBTREE_H

#include "vox_mpool.h"
#include "vox_kv_types.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 红黑树不透明类型 */
typedef struct vox_rbtree vox_rbtree_t;

/* 遍历回调函数类型 */
typedef void (*vox_rbtree_visit_func_t)(const void* key, size_t key_len, void* value, void* user_data);

/* 红黑树配置 */
typedef struct {
    vox_key_cmp_func_t key_cmp;     /* 键比较函数，NULL表示使用memcmp */
    vox_key_free_func_t key_free;   /* 键释放函数，NULL表示不释放 */
    vox_value_free_func_t value_free; /* 值释放函数，NULL表示不释放 */
} vox_rbtree_config_t;

/**
 * 使用默认配置创建红黑树
 * @param mpool 内存池指针，必须非NULL
 * @return 成功返回红黑树指针，失败返回NULL
 */
vox_rbtree_t* vox_rbtree_create(vox_mpool_t* mpool);

/**
 * 使用自定义配置创建红黑树
 * @param mpool 内存池指针，必须非NULL
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回红黑树指针，失败返回NULL
 */
vox_rbtree_t* vox_rbtree_create_with_config(vox_mpool_t* mpool, const vox_rbtree_config_t* config);

/**
 * 插入或更新键值对
 * @param tree 红黑树指针
 * @param key 键指针
 * @param key_len 键长度（字节）
 * @param value 值指针
 * @return 成功返回0，失败返回-1
 */
int vox_rbtree_insert(vox_rbtree_t* tree, const void* key, size_t key_len, void* value);

/**
 * 查找值
 * @param tree 红黑树指针
 * @param key 键指针
 * @param key_len 键长度（字节）
 * @return 成功返回值指针，不存在返回NULL
 */
void* vox_rbtree_find(const vox_rbtree_t* tree, const void* key, size_t key_len);

/**
 * 删除键值对
 * @param tree 红黑树指针
 * @param key 键指针
 * @param key_len 键长度（字节）
 * @return 成功返回0，不存在返回-1
 */
int vox_rbtree_delete(vox_rbtree_t* tree, const void* key, size_t key_len);

/**
 * 检查键是否存在
 * @param tree 红黑树指针
 * @param key 键指针
 * @param key_len 键长度（字节）
 * @return 存在返回true，不存在返回false
 */
bool vox_rbtree_contains(const vox_rbtree_t* tree, const void* key, size_t key_len);

/**
 * 获取红黑树中的元素数量
 * @param tree 红黑树指针
 * @return 返回元素数量
 */
size_t vox_rbtree_size(const vox_rbtree_t* tree);

/**
 * 检查红黑树是否为空
 * @param tree 红黑树指针
 * @return 为空返回true，否则返回false
 */
bool vox_rbtree_empty(const vox_rbtree_t* tree);

/**
 * 清空红黑树（保留树结构）
 * @param tree 红黑树指针
 */
void vox_rbtree_clear(vox_rbtree_t* tree);

/**
 * 销毁红黑树并释放所有资源
 * @param tree 红黑树指针
 */
void vox_rbtree_destroy(vox_rbtree_t* tree);

/**
 * 中序遍历红黑树（按键排序）
 * @param tree 红黑树指针
 * @param visit 访问函数
 * @param user_data 用户数据指针
 * @return 返回遍历的元素数量
 */
size_t vox_rbtree_inorder(vox_rbtree_t* tree, vox_rbtree_visit_func_t visit, void* user_data);

/**
 * 前序遍历红黑树
 * @param tree 红黑树指针
 * @param visit 访问函数
 * @param user_data 用户数据指针
 * @return 返回遍历的元素数量
 */
size_t vox_rbtree_preorder(vox_rbtree_t* tree, vox_rbtree_visit_func_t visit, void* user_data);

/**
 * 后序遍历红黑树
 * @param tree 红黑树指针
 * @param visit 访问函数
 * @param user_data 用户数据指针
 * @return 返回遍历的元素数量
 */
size_t vox_rbtree_postorder(vox_rbtree_t* tree, vox_rbtree_visit_func_t visit, void* user_data);

/**
 * 获取最小键
 * @param tree 红黑树指针
 * @param key_out 输出键指针
 * @param key_len_out 输出键长度
 * @return 成功返回0，树为空返回-1
 */
int vox_rbtree_min(const vox_rbtree_t* tree, const void** key_out, size_t* key_len_out);

/**
 * 获取最大键
 * @param tree 红黑树指针
 * @param key_out 输出键指针
 * @param key_len_out 输出键长度
 * @return 成功返回0，树为空返回-1
 */
int vox_rbtree_max(const vox_rbtree_t* tree, const void** key_out, size_t* key_len_out);

#ifdef __cplusplus
}
#endif

#endif /* VOX_RBTREE_H */
