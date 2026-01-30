/*
 * mheap_example.c - 最小堆示例程序
 * 演示 vox_mheap 的基本用法
 */

#include "../vox_mheap.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* 内存池释放包装函数（用于堆自动释放） */
static vox_mpool_t* g_mheap_mpool = NULL;

static void mpool_free_wrapper_mheap(void* elem) {
    if (elem && g_mheap_mpool) {
        vox_mpool_free(g_mheap_mpool, elem);
    }
}

/* 遍历回调函数 */
static void print_elem(void* elem, void* user_data) {
    (void)user_data;
    int* value = (int*)elem;
    printf("  %d", *value);
}

/* 整数比较函数 */
static int int_cmp(const void* elem1, const void* elem2) {
    int v1 = *(const int*)elem1;
    int v2 = *(const int*)elem2;
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
    
    printf("=== 创建最小堆 ===\n");
    vox_mheap_config_t heap_config = {0};
    heap_config.cmp_func = int_cmp;  /* 使用整数比较函数 */
    vox_mheap_t* heap = vox_mheap_create_with_config(mpool, &heap_config);
    if (!heap) {
        fprintf(stderr, "创建最小堆失败\n");
        return 1;
    }
    printf("最小堆创建成功，大小: %zu\n", vox_mheap_size(heap));
    
    printf("\n=== 插入元素 ===\n");
    int values[] = {30, 10, 50, 20, 40, 60, 5, 15, 25, 35};
    int count = sizeof(values) / sizeof(values[0]);
    
    for (int i = 0; i < count; i++) {
        /* 分配整数内存（使用内存池） */
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (!elem) {
            fprintf(stderr, "分配内存失败\n");
            continue;
        }
        *elem = values[i];
        int ret = vox_mheap_push(heap, elem);
        if (ret == 0) {
            printf("插入: %d\n", *elem);
        } else {
            printf("插入失败: %d\n", values[i]);
            vox_mpool_free(mpool, elem);
        }
    }
    printf("当前大小: %zu\n", vox_mheap_size(heap));
    
    printf("\n=== 查看最小元素（不移除） ===\n");
    int* min = (int*)vox_mheap_peek(heap);
    if (min) {
        printf("最小元素: %d\n", *min);
    }
    
    printf("\n=== 遍历堆（注意：不保证顺序） ===\n");
    printf("堆中元素:");
    vox_mheap_foreach(heap, print_elem, NULL);
    printf("\n");
    
    printf("\n=== 依次弹出最小元素 ===\n");
    while (!vox_mheap_empty(heap)) {
        int* elem = (int*)vox_mheap_pop(heap);
        if (elem) {
            printf("弹出: %d\n", *elem);
            vox_mpool_free(mpool, elem);
        }
    }
    printf("弹出后大小: %zu\n", vox_mheap_size(heap));
    
    printf("\n=== 测试自定义比较函数 ===\n");
    vox_mheap_config_t custom_config = {0};
    custom_config.cmp_func = int_cmp;
    custom_config.initial_capacity = 32;
    vox_mheap_t* custom_heap = vox_mheap_create_with_config(mpool, &custom_config);
    
    int test_values[] = {100, 50, 200, 25, 75, 150, 300};
    int test_count = sizeof(test_values) / sizeof(test_values[0]);
    
    for (int i = 0; i < test_count; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = test_values[i];
            vox_mheap_push(custom_heap, elem);
        }
    }
    
    printf("插入后，堆大小: %zu\n", vox_mheap_size(custom_heap));
    printf("依次弹出:\n");
    while (!vox_mheap_empty(custom_heap)) {
        int* elem = (int*)vox_mheap_pop(custom_heap);
        if (elem) {
            printf("  %d\n", *elem);
            vox_mpool_free(mpool, elem);
        }
    }
    
    printf("\n=== 测试自动内存管理 ===\n");
    g_mheap_mpool = mpool;  /* 设置全局内存池指针 */
    vox_mheap_config_t auto_config = {0};
    auto_config.cmp_func = int_cmp;
    auto_config.elem_free = mpool_free_wrapper_mheap;  /* 使用内存池释放函数 */
    vox_mheap_t* auto_heap = vox_mheap_create_with_config(mpool, &auto_config);
    
    for (int i = 0; i < 10; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = i * 10;
            vox_mheap_push(auto_heap, elem);
        }
    }
    
    printf("插入10个元素后，堆大小: %zu\n", vox_mheap_size(auto_heap));
    printf("依次弹出（自动释放）:\n");
    while (!vox_mheap_empty(auto_heap)) {
        int* elem = (int*)vox_mheap_pop(auto_heap);
        if (elem) {
            printf("  %d\n", *elem);
            vox_mpool_free(mpool, elem);  /* pop后需要手动释放 */
        }
    }
    
    /* 实际上，由于我们pop后立即释放，所以这里不需要额外处理 */
    /* 但如果使用clear或destroy，会自动释放所有元素 */
    
    printf("\n=== 测试大量插入（自动扩容） ===\n");
    vox_mheap_config_t large_config = {0};
    large_config.cmp_func = int_cmp;  /* 使用整数比较函数 */
    vox_mheap_t* large_heap = vox_mheap_create_with_config(mpool, &large_config);
    for (int i = 0; i < 100; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = 100 - i;  /* 倒序插入 */
            vox_mheap_push(large_heap, elem);
        }
    }
    printf("插入100个元素后，堆大小: %zu\n", vox_mheap_size(large_heap));
    
    /* 验证堆性质：每次弹出的应该是最小值（递增顺序） */
    int prev = -1;
    int pop_count = 0;
    while (!vox_mheap_empty(large_heap)) {
        int* elem = (int*)vox_mheap_pop(large_heap);
        if (elem) {
            if (prev >= 0 && *elem < prev) {
                printf("错误：堆性质被破坏！%d < %d\n", *elem, prev);
            }
            prev = *elem;
            vox_mpool_free(mpool, elem);
            pop_count++;
        }
    }
    printf("成功弹出 %d 个元素，堆性质保持正确\n", pop_count);
    
    printf("\n=== 测试清空堆 ===\n");
    vox_mheap_t* test_heap = vox_mheap_create(mpool);
    for (int i = 0; i < 5; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = i;
            vox_mheap_push(test_heap, elem);
        }
    }
    printf("清空前大小: %zu\n", vox_mheap_size(test_heap));
    
    /* 清理已插入的元素 */
    while (!vox_mheap_empty(test_heap)) {
        int* elem = (int*)vox_mheap_pop(test_heap);
        if (elem) vox_mpool_free(mpool, elem);
    }
    
    /* 使用自动释放配置 */
    vox_mheap_config_t clear_config = {0};
    clear_config.cmp_func = int_cmp;
    clear_config.elem_free = mpool_free_wrapper_mheap;
    vox_mheap_destroy(test_heap);
    test_heap = vox_mheap_create_with_config(mpool, &clear_config);
    
    for (int i = 0; i < 5; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = i;
            vox_mheap_push(test_heap, elem);
        }
    }
    printf("清空前大小: %zu\n", vox_mheap_size(test_heap));
    vox_mheap_clear(test_heap);
    printf("清空后大小: %zu\n", vox_mheap_size(test_heap));
    printf("是否为空: %s\n", vox_mheap_empty(test_heap) ? "是" : "否");
    
    printf("\n=== 清理资源 ===\n");
    vox_mheap_destroy(heap);
    vox_mheap_destroy(custom_heap);
    vox_mheap_destroy(auto_heap);
    vox_mheap_destroy(large_heap);
    vox_mheap_destroy(test_heap);
    
    g_mheap_mpool = NULL;  /* 清除全局指针 */
    
    /* 销毁内存池 */
    vox_mpool_destroy(mpool);
    
    printf("\n所有测试完成！\n");
    return 0;
}
