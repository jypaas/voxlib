/*
 * vector_example.c - 动态数组示例程序
 * 演示 vox_vector 的基本用法
 */

#include "../vox_vector.h"
#include "../vox_mpool.h"
#include "../vox_os.h"
#include <stdio.h>
#include <string.h>

/* 内存池释放包装函数（用于向量自动释放） */
static vox_mpool_t* g_vector_mpool = NULL;

static void mpool_free_wrapper_vector(void* elem) {
    if (elem && g_vector_mpool) {
        vox_mpool_free(g_vector_mpool, elem);
    }
}

/* 遍历回调函数 */
static void print_elem(void* elem, size_t index, void* user_data) {
    (void)user_data;
    int* value = (int*)elem;
    printf("  [%zu] = %d\n", index, *value);
}

/* 统计回调函数（未使用，保留作为示例） */
VOX_UNUSED_FUNC static void count_elem(void* elem, size_t index, void* user_data) {
    (void)elem;
    (void)index;
    size_t* count = (size_t*)user_data;
    (*count)++;
}

int main(void) {
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return 1;
    }
    
    printf("=== 创建动态数组 ===\n");
    vox_vector_t* vec = vox_vector_create(mpool);
    if (!vec) {
        fprintf(stderr, "创建动态数组失败\n");
        return 1;
    }
    printf("动态数组创建成功，大小: %zu, 容量: %zu\n", 
           vox_vector_size(vec), vox_vector_capacity(vec));
    
    printf("\n=== 添加元素（push） ===\n");
    int values[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    int count = sizeof(values) / sizeof(values[0]);
    
    for (int i = 0; i < count; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (!elem) {
            fprintf(stderr, "分配内存失败\n");
            continue;
        }
        *elem = values[i];
        int ret = vox_vector_push(vec, elem);
        if (ret == 0) {
            printf("添加: %d (大小: %zu, 容量: %zu)\n", 
                   *elem, vox_vector_size(vec), vox_vector_capacity(vec));
        } else {
            printf("添加失败: %d\n", values[i]);
            vox_mpool_free(mpool, elem);
        }
    }
    
    printf("\n=== 遍历数组 ===\n");
    vox_vector_foreach(vec, print_elem, NULL);
    
    printf("\n=== 访问元素（get） ===\n");
    for (size_t i = 0; i < vox_vector_size(vec); i++) {
        int* elem = (int*)vox_vector_get(vec, i);
        if (elem) {
            printf("vec[%zu] = %d\n", i, *elem);
        }
    }
    
    printf("\n=== 修改元素（set） ===\n");
    int* old_val = (int*)vox_vector_get(vec, 5);
    int* new_val = (int*)vox_mpool_alloc(mpool, sizeof(int));
    if (new_val) {
        *new_val = 999;
        vox_vector_set(vec, 5, new_val);
        if (old_val) vox_mpool_free(mpool, old_val);  /* 释放旧值 */
        printf("修改 vec[5] = %d\n", *new_val);
        printf("当前 vec[5] = %d\n", *(int*)vox_vector_get(vec, 5));
    }
    
    printf("\n=== 在指定位置插入元素 ===\n");
    int* insert_val = (int*)vox_mpool_alloc(mpool, sizeof(int));
    if (insert_val) {
        *insert_val = 55;
        vox_vector_insert(vec, 3, insert_val);
    }
    printf("在位置 3 插入: %d\n", *insert_val);
    printf("插入后大小: %zu\n", vox_vector_size(vec));
    printf("插入后的数组:\n");
    vox_vector_foreach(vec, print_elem, NULL);
    
    printf("\n=== 移除指定位置的元素 ===\n");
    int* removed = (int*)vox_vector_remove(vec, 2);
    if (removed) {
        printf("移除位置 2 的元素: %d\n", *removed);
        vox_mpool_free(mpool, removed);
    }
    printf("移除后大小: %zu\n", vox_vector_size(vec));
    printf("移除后的数组:\n");
    vox_vector_foreach(vec, print_elem, NULL);
    
    printf("\n=== 弹出末尾元素（pop） ===\n");
    while (!vox_vector_empty(vec)) {
        int* elem = (int*)vox_vector_pop(vec);
        if (elem) {
            printf("弹出: %d (剩余大小: %zu)\n", *elem, vox_vector_size(vec));
            vox_mpool_free(mpool, elem);
        }
    }
    printf("弹出后大小: %zu\n", vox_vector_size(vec));
    
    printf("\n=== 测试自动内存管理 ===\n");
    g_vector_mpool = mpool;  /* 设置全局内存池指针 */
    vox_vector_config_t auto_config = {0};
    auto_config.elem_free = mpool_free_wrapper_vector;  /* 使用内存池释放函数 */
    vox_vector_t* auto_vec = vox_vector_create_with_config(mpool, &auto_config);
    
    for (int i = 0; i < 10; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = i * 10;
            vox_vector_push(auto_vec, elem);
        }
    }
    
    printf("插入10个元素后，大小: %zu\n", vox_vector_size(auto_vec));
    printf("清空数组（自动释放元素）\n");
    vox_vector_clear(auto_vec);
    printf("清空后大小: %zu\n", vox_vector_size(auto_vec));
    
    printf("\n=== 测试调整大小（resize） ===\n");
    vox_vector_resize(auto_vec, 5);
    printf("调整大小到 5 后，大小: %zu, 容量: %zu\n", 
           vox_vector_size(auto_vec), vox_vector_capacity(auto_vec));
    
    /* 填充一些值 */
    for (size_t i = 0; i < vox_vector_size(auto_vec); i++) {
        int* old_elem = (int*)vox_vector_get(auto_vec, i);
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = (int)(i * 100);
            vox_vector_set(auto_vec, i, elem);
            if (old_elem) vox_mpool_free(mpool, old_elem);  /* 释放旧值 */
        }
    }
    printf("填充后的数组:\n");
    vox_vector_foreach(auto_vec, print_elem, NULL);
    
    printf("\n=== 测试预留容量（reserve） ===\n");
    printf("预留前，大小: %zu, 容量: %zu\n", 
           vox_vector_size(auto_vec), vox_vector_capacity(auto_vec));
    vox_vector_reserve(auto_vec, 100);
    printf("预留100后，大小: %zu, 容量: %zu\n", 
           vox_vector_size(auto_vec), vox_vector_capacity(auto_vec));
    
    printf("\n=== 测试大量插入（自动扩容） ===\n");
    vox_vector_t* large_vec = vox_vector_create(mpool);
    for (int i = 0; i < 100; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = i;
            vox_vector_push(large_vec, elem);
        }
    }
    printf("插入100个元素后，大小: %zu, 容量: %zu\n", 
           vox_vector_size(large_vec), vox_vector_capacity(large_vec));
    
    /* 验证所有元素 */
    bool all_correct = true;
    for (size_t i = 0; i < vox_vector_size(large_vec); i++) {
        int* elem = (int*)vox_vector_get(large_vec, i);
        if (!elem || *elem != (int)i) {
            printf("错误：vec[%zu] 期望 %zu，实际 %d\n", i, i, elem ? *elem : -1);
            all_correct = false;
        }
    }
    if (all_correct) {
        printf("所有元素验证正确！\n");
    }
    
    /* 清理 */
    for (size_t i = 0; i < vox_vector_size(large_vec); i++) {
        int* elem = (int*)vox_vector_get(large_vec, i);
        if (elem) vox_mpool_free(mpool, elem);
    }
    
    printf("\n=== 清理资源 ===\n");
    vox_vector_destroy(vec);
    vox_vector_destroy(auto_vec);
    vox_vector_destroy(large_vec);
    
    g_vector_mpool = NULL;  /* 清除全局指针 */
    
    /* 销毁内存池 */
    vox_mpool_destroy(mpool);
    
    printf("\n所有测试完成！\n");
    return 0;
}
