/*
 * rbtree_example.c - 红黑树示例程序
 * 演示 vox_rbtree 的基本用法
 */

#include "../vox_rbtree.h"
#include "../vox_mpool.h"
#include "../vox_os.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* 遍历回调函数 */
static void print_entry(const void* key, size_t key_len, void* value, void* user_data) {
    (void)user_data;
    if (key_len <= 20) {
        printf("  键: %.*s, 值: %s\n", (int)key_len, (const char*)key, (const char*)value);
    } else {
        printf("  键: (长度 %zu), 值: %s\n", key_len, (const char*)value);
    }
}

/* 统计回调函数（未使用，保留作为示例） */
VOX_UNUSED_FUNC static void count_entry(const void* key, size_t key_len, void* value, void* user_data) {
    (void)key;
    (void)key_len;
    (void)value;
    size_t* count = (size_t*)user_data;
    (*count)++;
}

/* 整数比较函数 */
static int int_cmp(const void* k1, const void* k2, size_t len) {
    (void)len;
    int v1 = *(const int*)k1;
    int v2 = *(const int*)k2;
    if (v1 < v2) return -1;
    if (v1 > v2) return 1;
    return 0;
}

int main(void) {
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return 1;
    }
    
    printf("=== 创建红黑树 ===\n");
    vox_rbtree_t* tree = vox_rbtree_create(mpool);
    if (!tree) {
        fprintf(stderr, "创建红黑树失败\n");
        return 1;
    }
    printf("红黑树创建成功，大小: %zu\n", vox_rbtree_size(tree));
    
    printf("\n=== 插入键值对 ===\n");
    const char* keys[] = {"dog", "cat", "bird", "fish", "elephant", "tiger", "lion", "bear"};
    const char* values[] = {"狗", "猫", "鸟", "鱼", "大象", "老虎", "狮子", "熊"};
    int count = sizeof(keys) / sizeof(keys[0]);
    
    for (int i = 0; i < count; i++) {
        int ret = vox_rbtree_insert(tree, keys[i], strlen(keys[i]), (void*)values[i]);
        if (ret == 0) {
            printf("插入: %s -> %s\n", keys[i], values[i]);
        } else {
            printf("插入失败: %s\n", keys[i]);
        }
    }
    printf("当前大小: %zu\n", vox_rbtree_size(tree));
    
    printf("\n=== 查找值 ===\n");
    for (int i = 0; i < count; i++) {
        const char* value = (const char*)vox_rbtree_find(tree, keys[i], strlen(keys[i]));
        if (value) {
            printf("查找 %s: 找到 -> %s\n", keys[i], value);
        } else {
            printf("查找 %s: 未找到\n", keys[i]);
        }
    }
    
    printf("\n=== 检查键是否存在 ===\n");
    printf("contains('cat'): %s\n", vox_rbtree_contains(tree, "cat", 3) ? "true" : "false");
    printf("contains('wolf'): %s\n", vox_rbtree_contains(tree, "wolf", 4) ? "true" : "false");
    
    printf("\n=== 中序遍历（按键排序） ===\n");
    size_t visited = vox_rbtree_inorder(tree, print_entry, NULL);
    printf("共遍历 %zu 个元素\n", visited);
    
    printf("\n=== 前序遍历 ===\n");
    visited = vox_rbtree_preorder(tree, print_entry, NULL);
    printf("共遍历 %zu 个元素\n", visited);
    
    printf("\n=== 获取最小和最大键 ===\n");
    const void* min_key;
    size_t min_key_len;
    if (vox_rbtree_min(tree, &min_key, &min_key_len) == 0) {
        printf("最小键: %.*s\n", (int)min_key_len, (const char*)min_key);
    }
    
    const void* max_key;
    size_t max_key_len;
    if (vox_rbtree_max(tree, &max_key, &max_key_len) == 0) {
        printf("最大键: %.*s\n", (int)max_key_len, (const char*)max_key);
    }
    
    printf("\n=== 更新值 ===\n");
    vox_rbtree_insert(tree, "cat", 3, (void*)"小猫");
    const char* new_value = (const char*)vox_rbtree_find(tree, "cat", 3);
    printf("更新后 cat 的值: %s\n", new_value);
    
    printf("\n=== 删除键值对 ===\n");
    int ret = vox_rbtree_delete(tree, "bird", 4);
    if (ret == 0) {
        printf("删除 'bird' 成功\n");
    } else {
        printf("删除 'bird' 失败\n");
    }
    printf("当前大小: %zu\n", vox_rbtree_size(tree));
    
    printf("\n=== 尝试获取已删除的键 ===\n");
    const char* deleted_value = (const char*)vox_rbtree_find(tree, "bird", 4);
    printf("查找 'bird': %s\n", deleted_value ? deleted_value : "未找到（已删除）");
    
    printf("\n=== 删除后中序遍历 ===\n");
    vox_rbtree_inorder(tree, print_entry, NULL);
    
    printf("\n=== 测试大量插入 ===\n");
    for (int i = 0; i < 100; i++) {
        char key[32] = {0};
        char value[32] = {0};
        snprintf(key, sizeof(key), "key_%03d", i);
        snprintf(value, sizeof(value), "value_%d", i);
        /* 使用静态缓冲区，不需要释放 */
        static char value_bufs[100][32];
        strcpy(value_bufs[i], value);
        vox_rbtree_insert(tree, key, strlen(key), value_bufs[i]);
    }
    printf("插入100个元素后，大小: %zu\n", vox_rbtree_size(tree));
    
    printf("\n=== 测试整数键 ===\n");
    vox_rbtree_config_t int_config = {0};
    int_config.key_cmp = int_cmp;
    vox_rbtree_t* int_tree = vox_rbtree_create_with_config(mpool, &int_config);
    
    for (int i = 0; i < 10; i++) {
        int key = i * 10;
        char value[32] = {0};
        snprintf(value, sizeof(value), "num_%d", key);
        static char int_value_bufs[10][32];
        strcpy(int_value_bufs[i], value);
        vox_rbtree_insert(int_tree, &key, sizeof(int), int_value_bufs[i]);
    }
    
    printf("整数键中序遍历:\n");
    vox_rbtree_inorder(int_tree, print_entry, NULL);
    
    printf("\n=== 测试自定义配置 ===\n");
    vox_rbtree_config_t config = {0};
    vox_rbtree_t* custom_tree = vox_rbtree_create_with_config(mpool, &config);
    if (custom_tree) {
        printf("使用自定义配置创建红黑树成功\n");
        vox_rbtree_destroy(custom_tree);
    }
    
    printf("\n=== 清空红黑树 ===\n");
    printf("清空前大小: %zu\n", vox_rbtree_size(tree));
    vox_rbtree_clear(tree);
    printf("清空后大小: %zu\n", vox_rbtree_size(tree));
    printf("是否为空: %s\n", vox_rbtree_empty(tree) ? "是" : "否");
    
    printf("\n=== 清理资源 ===\n");
    vox_rbtree_destroy(tree);
    vox_rbtree_destroy(int_tree);
    
    /* 销毁内存池 */
    vox_mpool_destroy(mpool);
    
    printf("\n所有测试完成！\n");
    return 0;
}
