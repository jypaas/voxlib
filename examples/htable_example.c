/*
 * htable_example.c - 哈希表示例程序
 * 演示 vox_htable 的基本用法
 */

#include "../vox_htable.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* 内存池字符串复制函数 */
static char* mpool_strdup(vox_mpool_t* mpool, const char* str) {
    if (!mpool || !str) return NULL;
    size_t len = strlen(str) + 1;
    char* dup = (char*)vox_mpool_alloc(mpool, len);
    if (dup) {
        memcpy(dup, str, len);
    }
    return dup;
}

/* 内存池释放包装函数（用于哈希表自动释放） */
static vox_mpool_t* g_htable_mpool = NULL;

static void mpool_free_wrapper_htable(void* value) {
    if (value && g_htable_mpool) {
        vox_mpool_free(g_htable_mpool, value);
    }
}

/* 打印统计信息 */
static void print_stats(const vox_htable_t* htable) {
    size_t capacity, size;
    double load_factor;
    vox_htable_stats(htable, &capacity, &size, &load_factor);
    printf("  容量: %zu, 元素数: %zu, 负载因子: %.2f%%\n", 
           capacity, size, load_factor * 100.0);
}

/* 遍历回调函数 */
static void print_entry(const void* key, size_t key_len, void* value, void* user_data) {
    (void)user_data;
    if (key_len <= 20) {
        printf("  键: %.*s, 值: %s\n", (int)key_len, (const char*)key, (const char*)value);
    } else {
        printf("  键: (长度 %zu), 值: %s\n", key_len, (const char*)value);
    }
}

int main(void) {
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return 1;
    }
    
    printf("=== 创建哈希表 ===\n");
    vox_htable_t* htable = vox_htable_create(mpool);
    if (!htable) {
        fprintf(stderr, "创建哈希表失败\n");
        return 1;
    }
    print_stats(htable);
    
    printf("\n=== 插入键值对 ===\n");
    const char* keys[] = {"apple", "banana", "cherry", "date", "elderberry"};
    const char* values[] = {"苹果", "香蕉", "樱桃", "枣子", "接骨木莓"};
    int count = sizeof(keys) / sizeof(keys[0]);
    
    for (int i = 0; i < count; i++) {
        int ret = vox_htable_set(htable, keys[i], strlen(keys[i]), (void*)values[i]);
        if (ret == 0) {
            printf("插入: %s -> %s\n", keys[i], values[i]);
        } else {
            printf("插入失败: %s\n", keys[i]);
        }
    }
    print_stats(htable);
    
    printf("\n=== 查找值 ===\n");
    for (int i = 0; i < count; i++) {
        const char* value = (const char*)vox_htable_get(htable, keys[i], strlen(keys[i]));
        if (value) {
            printf("查找 %s: 找到 -> %s\n", keys[i], value);
        } else {
            printf("查找 %s: 未找到\n", keys[i]);
        }
    }
    
    printf("\n=== 检查键是否存在 ===\n");
    printf("contains('apple'): %s\n", vox_htable_contains(htable, "apple", 5) ? "true" : "false");
    printf("contains('grape'): %s\n", vox_htable_contains(htable, "grape", 5) ? "true" : "false");
    
    printf("\n=== 更新值 ===\n");
    vox_htable_set(htable, "apple", 5, (void*)"红苹果");
    const char* new_value = (const char*)vox_htable_get(htable, "apple", 5);
    printf("更新后 apple 的值: %s\n", new_value);
    
    printf("\n=== 遍历所有键值对 ===\n");
    vox_htable_foreach(htable, print_entry, NULL);
    
    printf("\n=== 删除键值对 ===\n");
    int ret = vox_htable_delete(htable, "banana", 6);
    if (ret == 0) {
        printf("删除 'banana' 成功\n");
    } else {
        printf("删除 'banana' 失败\n");
    }
    print_stats(htable);
    
    printf("\n=== 尝试获取已删除的键 ===\n");
    const char* deleted_value = (const char*)vox_htable_get(htable, "banana", 6);
    printf("查找 'banana': %s\n", deleted_value ? deleted_value : "未找到（已删除）");
    
    printf("\n=== 测试大量插入（自动扩容） ===\n");
    /* 注意：这里使用字符串字面量，不需要释放 */
    for (int i = 0; i < 100; i++) {
        char key[32];
        char value[32];
        snprintf(key, sizeof(key), "key_%d", i);
        snprintf(value, sizeof(value), "value_%d", i);
        /* 使用静态缓冲区，不需要释放 */
        static char value_bufs[100][32];
        strcpy(value_bufs[i], value);
        vox_htable_set(htable, key, strlen(key), value_bufs[i]);
    }
    print_stats(htable);
    printf("插入100个元素后，哈希表自动扩容\n");
    
    printf("\n=== 测试整数键（使用自动内存管理） ===\n");
    /* 创建带自动释放功能的哈希表 */
    g_htable_mpool = mpool;  /* 设置全局内存池指针 */
    vox_htable_config_t int_config = {0};
    int_config.value_free = mpool_free_wrapper_htable;  /* 使用内存池释放函数 */
    vox_htable_t* int_htable = vox_htable_create_with_config(mpool, &int_config);
    for (int i = 0; i < 10; i++) {
        int key = i * 10;
        char value[32];
        snprintf(value, sizeof(value), "num_%d", key);
        char* value_dup = mpool_strdup(mpool, value);
        if (value_dup) {
            vox_htable_set(int_htable, &key, sizeof(int), value_dup);
        }
    }
    
    for (int i = 0; i < 10; i++) {
        int key = i * 10;
        const char* value = (const char*)vox_htable_get(int_htable, &key, sizeof(int));
        if (value) {
            printf("  %d -> %s\n", key, value);
        }
    }
    
    printf("\n=== 测试自定义配置 ===\n");
    vox_htable_config_t config = {0};
    config.initial_capacity = 32;
    config.load_factor = 0.8;
    vox_htable_t* custom_htable = vox_htable_create_with_config(mpool, &config);
    if (custom_htable) {
        print_stats(custom_htable);
        printf("使用自定义配置创建哈希表成功\n");
        vox_htable_destroy(custom_htable);
    }
    
    printf("\n=== 清空哈希表 ===\n");
    printf("清空前大小: %zu\n", vox_htable_size(htable));
    vox_htable_clear(htable);
    printf("清空后大小: %zu\n", vox_htable_size(htable));
    printf("是否为空: %s\n", vox_htable_empty(htable) ? "是" : "否");
    print_stats(htable);
    
    printf("\n=== 清理资源 ===\n");
    vox_htable_destroy(htable);
    vox_htable_destroy(int_htable);
    
    /* 销毁内存池 */
    vox_mpool_destroy(mpool);
    
    printf("\n所有测试完成！\n");
    return 0;
}
