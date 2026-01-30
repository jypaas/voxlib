/*
 * queue_example.c - 队列示例程序
 * 演示 vox_queue 的基本用法
 */

#include "../vox_queue.h"
#include "../vox_mpool.h"
#include "../vox_thread.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* 遍历回调函数 */
static void print_elem(void* elem, size_t index, void* user_data) {
    (void)user_data;
    int* value = (int*)elem;
    printf("  [%zu] %d\n", index, *value);
}

/* 内存池释放包装函数（用于队列自动释放） */
static vox_mpool_t* g_queue_mpool = NULL;

static void mpool_free_wrapper(void* elem) {
    if (elem && g_queue_mpool) {
        vox_mpool_free(g_queue_mpool, elem);
    }
}

/* MPSC 生产者线程函数 */
typedef struct {
    int thread_id;
    vox_queue_t* queue;
    vox_mpool_t* mpool;  /* 添加内存池指针 */
    int count;
} mpsc_producer_data_t;

static int mpsc_producer_thread(void* user_data) {
    mpsc_producer_data_t* pd = (mpsc_producer_data_t*)user_data;
    for (int j = 0; j < pd->count; j++) {
        int* elem = (int*)vox_mpool_alloc(pd->mpool, sizeof(int));
        if (!elem) {
            fprintf(stderr, "分配内存失败\n");
            continue;
        }
        *elem = pd->thread_id * 1000 + j;
        if (vox_queue_enqueue(pd->queue, elem) == 0) {
            printf("  生产者 %d 入队: %d\n", pd->thread_id, *elem);
        } else {
            printf("  生产者 %d 入队失败（队列已满）: %d\n", pd->thread_id, *elem);
            vox_mpool_free(pd->mpool, elem);
        }
        vox_thread_sleep(10);  /* 模拟工作 */
    }
    return 0;
}

int main(void) {
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return 1;
    }
    
    printf("=== 创建队列 ===\n");
    vox_queue_t* queue = vox_queue_create(mpool);
    if (!queue) {
        fprintf(stderr, "创建队列失败\n");
        vox_mpool_destroy(mpool);
        return 1;
    }
    printf("队列创建成功，大小: %zu, 容量: %zu\n", 
           vox_queue_size(queue), vox_queue_capacity(queue));
    
    printf("\n=== 入队操作 ===\n");
    int values[] = {10, 20, 30, 40, 50};
    int count = sizeof(values) / sizeof(values[0]);
    
    for (int i = 0; i < count; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (!elem) {
            fprintf(stderr, "分配内存失败\n");
            continue;
        }
        *elem = values[i];
        int ret = vox_queue_enqueue(queue, elem);
        if (ret == 0) {
            printf("入队: %d\n", *elem);
        } else {
            printf("入队失败: %d\n", *elem);
            vox_mpool_free(mpool, elem);
        }
    }
    printf("当前大小: %zu, 容量: %zu\n", 
           vox_queue_size(queue), vox_queue_capacity(queue));
    
    printf("\n=== 查看队首元素 ===\n");
    int* peek_elem = (int*)vox_queue_peek(queue);
    if (peek_elem) {
        printf("队首元素: %d\n", *peek_elem);
    }
    
    printf("\n=== 遍历队列 ===\n");
    size_t visited = vox_queue_foreach(queue, print_elem, NULL);
    printf("共遍历 %zu 个元素\n", visited);
    
    printf("\n=== 出队操作 ===\n");
    while (!vox_queue_empty(queue)) {
        int* elem = (int*)vox_queue_dequeue(queue);
        if (elem) {
            printf("出队: %d\n", *elem);
            vox_mpool_free(mpool, elem);
        }
    }
    printf("出队后大小: %zu\n", vox_queue_size(queue));
    
    printf("\n=== 测试大量入队（自动扩容） ===\n");
    for (int i = 0; i < 100; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = i;
            vox_queue_enqueue(queue, elem);
        }
    }
    printf("插入100个元素后，大小: %zu, 容量: %zu\n", 
           vox_queue_size(queue), vox_queue_capacity(queue));
    
    printf("\n=== 测试自动内存管理 ===\n");
    g_queue_mpool = mpool;  /* 设置全局内存池指针 */
    vox_queue_config_t config = {0};
    config.elem_free = mpool_free_wrapper;  /* 使用内存池释放函数 */
    vox_queue_t* auto_queue = vox_queue_create_with_config(mpool, &config);
    
    for (int i = 0; i < 10; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = i * 10;
            vox_queue_enqueue(auto_queue, elem);
        }
    }
    
    printf("自动管理队列大小: %zu\n", vox_queue_size(auto_queue));
    printf("清空队列（自动释放元素）\n");
    vox_queue_clear(auto_queue);
    printf("清空后大小: %zu\n", vox_queue_size(auto_queue));
    
    printf("\n=== 测试循环数组特性 ===\n");
    vox_queue_t* test_queue = vox_queue_create(mpool);
    /* 先入队一些元素 */
    for (int i = 0; i < 5; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = i;
            vox_queue_enqueue(test_queue, elem);
        }
    }
    /* 出队一些元素 */
    for (int i = 0; i < 3; i++) {
        int* elem = (int*)vox_queue_dequeue(test_queue);
        if (elem) vox_mpool_free(mpool, elem);
    }
    /* 再入队一些元素，测试循环特性 */
    for (int i = 10; i < 15; i++) {
        int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
        if (elem) {
            *elem = i;
            vox_queue_enqueue(test_queue, elem);
        }
    }
    printf("循环数组测试，大小: %zu, 容量: %zu\n", 
           vox_queue_size(test_queue), vox_queue_capacity(test_queue));
    printf("遍历结果:\n");
    vox_queue_foreach(test_queue, print_elem, NULL);
    
    /* 清理 */
    while (!vox_queue_empty(test_queue)) {
        int* elem = (int*)vox_queue_dequeue(test_queue);
        if (elem) vox_mpool_free(mpool, elem);
    }
    
    printf("\n=== 测试 SPSC 无锁队列 ===\n");
    vox_queue_config_t spsc_config = {0};
    spsc_config.type = VOX_QUEUE_TYPE_SPSC;
    spsc_config.initial_capacity = 64;  /* SPSC 必须指定容量 */
    vox_queue_t* spsc_queue = vox_queue_create_with_config(mpool, &spsc_config);
    
    if (spsc_queue) {
        printf("SPSC 队列创建成功，容量: %zu\n", vox_queue_capacity(spsc_queue));
        
        /* 单生产者入队 */
        for (int i = 0; i < 10; i++) {
            int* elem = (int*)vox_mpool_alloc(mpool, sizeof(int));
            if (!elem) {
                fprintf(stderr, "分配内存失败\n");
                continue;
            }
            *elem = i * 100;
            if (vox_queue_enqueue(spsc_queue, elem) == 0) {
                printf("SPSC 入队: %d\n", *elem);
            } else {
                printf("SPSC 入队失败（队列已满）: %d\n", *elem);
                vox_mpool_free(mpool, elem);
            }
        }
        
        /* 单消费者出队 */
        printf("SPSC 出队:\n");
        while (!vox_queue_empty(spsc_queue)) {
            int* elem = (int*)vox_queue_dequeue(spsc_queue);
            if (elem) {
                printf("  出队: %d\n", *elem);
                vox_mpool_free(mpool, elem);
            }
        }
        
        vox_queue_destroy(spsc_queue);
    }
    
    printf("\n=== 测试 MPSC 无锁队列（多生产者单消费者） ===\n");
    vox_queue_config_t mpsc_config = {0};
    mpsc_config.type = VOX_QUEUE_TYPE_MPSC;
    mpsc_config.initial_capacity = 128;  /* MPSC 必须指定容量 */
    vox_queue_t* mpsc_queue = vox_queue_create_with_config(mpool, &mpsc_config);
    
    if (mpsc_queue) {
        printf("MPSC 队列创建成功，容量: %zu\n", vox_queue_capacity(mpsc_queue));
        
        #define MPSC_PRODUCER_COUNT 3
        #define MPSC_ITEMS_PER_PRODUCER 5
        
        mpsc_producer_data_t producer_data[MPSC_PRODUCER_COUNT];
        vox_thread_t* producer_threads[MPSC_PRODUCER_COUNT];
        
        /* 创建生产者线程 */
        for (int i = 0; i < MPSC_PRODUCER_COUNT; i++) {
            producer_data[i].thread_id = i;
            producer_data[i].queue = mpsc_queue;
            producer_data[i].mpool = mpool;  /* 传递内存池指针 */
            producer_data[i].count = MPSC_ITEMS_PER_PRODUCER;
            
            producer_threads[i] = vox_thread_create(mpool, mpsc_producer_thread, &producer_data[i]);
        }
        
        /* 等待所有生产者完成 */
        for (int i = 0; i < MPSC_PRODUCER_COUNT; i++) {
            int ret = 0;
            vox_thread_join(producer_threads[i], &ret);
        }
        
        printf("所有生产者完成，队列大小: %zu\n", vox_queue_size(mpsc_queue));
        
        /* 单消费者出队 */
        printf("单消费者出队:\n");
        int total_dequeued = 0;
        while (!vox_queue_empty(mpsc_queue)) {
            int* elem = (int*)vox_queue_dequeue(mpsc_queue);
            if (elem) {
                printf("  出队: %d\n", *elem);
                vox_mpool_free(mpool, elem);
                total_dequeued++;
            }
        }
        printf("总共出队 %d 个元素\n", total_dequeued);
        
        vox_queue_destroy(mpsc_queue);
    }
    
    printf("\n=== 清理资源 ===\n");
    /* 清理 queue 中剩余的元素 */
    while (!vox_queue_empty(queue)) {
        int* elem = (int*)vox_queue_dequeue(queue);
        if (elem) vox_mpool_free(mpool, elem);
    }
    
    vox_queue_destroy(queue);
    vox_queue_destroy(auto_queue);
    vox_queue_destroy(test_queue);
    
    g_queue_mpool = NULL;  /* 清除全局指针 */
    
    /* 销毁内存池 */
    vox_mpool_destroy(mpool);
    
    printf("\n所有测试完成！\n");
    return 0;
}
