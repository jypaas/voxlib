/*
 * thread_example.c - 线程和线程本地存储示例程序
 * 演示 vox_thread 的基本用法
 */

#include "../vox_thread.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <string.h>

/* 线程数据结构 */
typedef struct {
    int thread_num;
    int iterations;
} thread_data_t;

/* 全局TLS键和内存池 */
static vox_tls_key_t* g_tls_key = NULL;
static vox_mpool_t* g_tls_mpool = NULL;  /* TLS数据使用的内存池（线程安全） */

/* TLS数据析构函数 */
static void tls_destructor(void* value) {
    if (value && g_tls_mpool) {
        printf("  [TLS析构] 线程 %lu 的TLS数据被释放: %s\n", 
               (unsigned long)vox_thread_self(), (char*)value);
        vox_mpool_free(g_tls_mpool, value);
    }
}

/* 工作线程函数 */
static int worker_thread(void* user_data) {
    thread_data_t* data = (thread_data_t*)user_data;
    vox_thread_id_t tid = vox_thread_self();
    
    printf("  线程 %d 启动 (ID: %llu)\n", data->thread_num, (unsigned long long)tid);
    
    /* 设置TLS数据 */
    if (g_tls_key && g_tls_mpool) {
        char* tls_value = (char*)vox_mpool_alloc(g_tls_mpool, 64);
        if (tls_value) {
            snprintf(tls_value, 64, "线程%d的数据", data->thread_num);
            vox_tls_set(g_tls_key, tls_value);
            printf("  线程 %d 设置TLS: %s\n", data->thread_num, tls_value);
        }
    }
    
    /* 执行工作 */
    for (int i = 0; i < data->iterations; i++) {
        /* 获取TLS数据 */
        if (g_tls_key) {
            char* tls_value = (char*)vox_tls_get(g_tls_key);
            if (tls_value) {
                printf("  线程 %d 迭代 %d: TLS值 = %s\n", 
                       data->thread_num, i + 1, tls_value);
            }
        }
        
        vox_thread_sleep(100);  /* 休眠100毫秒 */
    }
    
    printf("  线程 %d 完成 (ID: %llu)\n", data->thread_num, (unsigned long long)tid);
    return data->thread_num * 10;  /* 返回退出码 */
}

/* 测试基本线程操作 */
static void test_basic_threads(void) {
    printf("\n=== 测试基本线程操作 ===\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    #define NUM_THREADS 3
    vox_thread_t* threads[NUM_THREADS];
    thread_data_t data[NUM_THREADS];
    
    /* 创建多个线程 */
    printf("创建 %d 个线程...\n", NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++) {
        data[i].thread_num = i + 1;
        data[i].iterations = 3;
        
        threads[i] = vox_thread_create(mpool, worker_thread, &data[i]);
        if (!threads[i]) {
            fprintf(stderr, "创建线程 %d 失败\n", i + 1);
            continue;
        }
        
        printf("创建线程 %d，ID: %llu\n", i + 1, 
               (unsigned long long)vox_thread_id(threads[i]));
    }
    
    /* 等待所有线程完成 */
    printf("\n等待所有线程完成...\n");
    for (int i = 0; i < NUM_THREADS; i++) {
        if (threads[i]) {
            int exit_code;
            if (vox_thread_join(threads[i], &exit_code) == 0) {
                printf("线程 %d 已退出，退出码: %d\n", i + 1, exit_code);
            } else {
                printf("等待线程 %d 失败\n", i + 1);
            }
        }
    }
    
    vox_mpool_destroy(mpool);
}

/* 测试线程分离 */
static void test_detached_threads(void) {
    printf("\n=== 测试分离线程 ===\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    thread_data_t data = {99, 2};
    vox_thread_t* thread = vox_thread_create(mpool, worker_thread, &data);
    
    if (thread) {
        printf("创建分离线程，ID: %llu\n", 
               (unsigned long long)vox_thread_id(thread));
        
        if (vox_thread_detach(thread) == 0) {
            printf("线程已分离，将自动清理\n");
        } else {
            printf("分离线程失败\n");
        }
        
        /* 等待一段时间让线程完成 */
        printf("等待分离线程完成...\n");
        vox_thread_sleep(500);
    }
    
    vox_mpool_destroy(mpool);
}

/* 测试线程本地存储 */
static void test_thread_local_storage(void) {
    printf("\n=== 测试线程本地存储 ===\n");
    
    /* 创建内存池（用于线程） */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 创建线程安全的内存池（用于TLS数据，因为析构函数可能在不同线程中调用） */
    vox_mpool_config_t tls_mpool_config = {0};
    tls_mpool_config.thread_safe = 1;  /* TLS析构函数可能在不同线程中调用，需要线程安全 */
    g_tls_mpool = vox_mpool_create_with_config(&tls_mpool_config);
    if (!g_tls_mpool) {
        fprintf(stderr, "创建TLS内存池失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    /* 创建TLS键 */
    g_tls_key = vox_tls_key_create(mpool, tls_destructor);
    if (!g_tls_key) {
        fprintf(stderr, "创建TLS键失败\n");
        vox_mpool_destroy(g_tls_mpool);
        g_tls_mpool = NULL;
        vox_mpool_destroy(mpool);
        return;
    }
    printf("TLS键创建成功\n");
    
    /* 在主线程中设置TLS */
    char* main_tls = (char*)vox_mpool_alloc(g_tls_mpool, 64);
    if (main_tls) {
        strcpy(main_tls, "主线程数据");
        vox_tls_set(g_tls_key, main_tls);
        printf("主线程设置TLS: %s\n", main_tls);
    }
    
    /* 创建多个线程测试TLS */
    #define NUM_TLS_THREADS 2
    vox_thread_t* threads[NUM_TLS_THREADS];
    thread_data_t data[NUM_TLS_THREADS];
    
    printf("\n创建 %d 个线程测试TLS...\n", NUM_TLS_THREADS);
    for (int i = 0; i < NUM_TLS_THREADS; i++) {
        data[i].thread_num = i + 1;
        data[i].iterations = 2;
        threads[i] = vox_thread_create(mpool, worker_thread, &data[i]);
    }
    
    /* 等待线程完成 */
    for (int i = 0; i < NUM_TLS_THREADS; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    /* 检查主线程的TLS是否还在 */
    char* main_tls_check = (char*)vox_tls_get(g_tls_key);
    if (main_tls_check) {
        printf("主线程TLS仍然存在: %s\n", main_tls_check);
    }
    
    /* 清理TLS键 */
    vox_tls_key_destroy(g_tls_key);
    g_tls_key = NULL;
    if (main_tls) {
        vox_mpool_free(g_tls_mpool, main_tls);
    }
    vox_mpool_destroy(g_tls_mpool);
    g_tls_mpool = NULL;
    vox_mpool_destroy(mpool);
}

/* 测试线程ID比较 */
static void test_thread_id(void) {
    printf("\n=== 测试线程ID ===\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    vox_thread_id_t main_id = vox_thread_self();
    printf("主线程ID: %llu\n", (unsigned long long)main_id);
    
    thread_data_t data = {0, 1};
    vox_thread_t* thread = vox_thread_create(mpool, worker_thread, &data);
    
    if (thread) {
        vox_thread_id_t thread_id = vox_thread_id(thread);
        printf("工作线程ID: %llu\n", (unsigned long long)thread_id);
        
        if (vox_thread_id_equal(main_id, thread_id)) {
            printf("线程ID相同（不应该发生）\n");
        } else {
            printf("线程ID不同（正确）\n");
        }
        
        vox_thread_join(thread, NULL);
    }
    
    vox_mpool_destroy(mpool);
}

/* 测试线程让出 */
static void test_thread_yield(void) {
    printf("\n=== 测试线程让出 ===\n");
    
    printf("主线程让出CPU时间片...\n");
    vox_thread_yield();
    printf("继续执行\n");
}

/* 测试线程优先级 */
typedef struct {
    int thread_num;
    vox_thread_priority_t priority;
} priority_test_data_t;

static int priority_worker(void* user_data) {
    priority_test_data_t* data = (priority_test_data_t*)user_data;
    vox_thread_id_t tid = vox_thread_self();
    
    printf("  线程 %d (ID: %llu): 优先级 = ", 
           data->thread_num, (unsigned long long)tid);
    
    switch (data->priority) {
        case VOX_THREAD_PRIORITY_LOWEST:
            printf("最低");
            break;
        case VOX_THREAD_PRIORITY_BELOW_NORMAL:
            printf("低于正常");
            break;
        case VOX_THREAD_PRIORITY_NORMAL:
            printf("正常");
            break;
        case VOX_THREAD_PRIORITY_ABOVE_NORMAL:
            printf("高于正常");
            break;
        case VOX_THREAD_PRIORITY_HIGHEST:
            printf("最高");
            break;
        case VOX_THREAD_PRIORITY_TIME_CRITICAL:
            printf("时间关键");
            break;
    }
    printf("\n");
    
    /* 执行一些工作 */
    for (int i = 0; i < 3; i++) {
        vox_thread_sleep(50);
    }
    
    return 0;
}

static void test_thread_priority(void) {
    printf("\n=== 测试线程优先级 ===\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    #define PRIORITY_THREADS 3
    vox_thread_t* threads[PRIORITY_THREADS];
    priority_test_data_t data[PRIORITY_THREADS];
    vox_thread_priority_t priorities[PRIORITY_THREADS] = {
        VOX_THREAD_PRIORITY_LOWEST,
        VOX_THREAD_PRIORITY_NORMAL,
        VOX_THREAD_PRIORITY_HIGHEST
    };
    
    printf("创建 %d 个不同优先级的线程...\n", PRIORITY_THREADS);
    for (int i = 0; i < PRIORITY_THREADS; i++) {
        data[i].thread_num = i + 1;
        data[i].priority = priorities[i];
        
        threads[i] = vox_thread_create(mpool, priority_worker, &data[i]);
        if (threads[i]) {
            /* 设置线程优先级 */
            if (vox_thread_set_priority(threads[i], priorities[i]) == 0) {
                printf("线程 %d 优先级设置成功\n", i + 1);
            } else {
                printf("线程 %d 优先级设置失败\n", i + 1);
            }
        }
    }
    
    /* 等待所有线程完成 */
    for (int i = 0; i < PRIORITY_THREADS; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    /* 测试获取当前线程优先级 */
    vox_thread_priority_t current_priority;
    if (vox_thread_get_priority(NULL, &current_priority) == 0) {
        printf("当前线程优先级获取成功\n");
    }
    
    vox_mpool_destroy(mpool);
}

/* 测试CPU亲和力 */
typedef struct {
    int thread_num;
    uint64_t cpu_mask;
} affinity_test_data_t;

static int affinity_worker(void* user_data) {
    affinity_test_data_t* data = (affinity_test_data_t*)user_data;
    vox_thread_id_t tid = vox_thread_self();
    
    printf("  线程 %d (ID: %llu): 开始工作\n", 
           data->thread_num, (unsigned long long)tid);
    
    /* 获取当前CPU亲和力 */
    uint64_t current_mask;
    if (vox_thread_get_affinity(NULL, &current_mask) == 0) {
        printf("  线程 %d: 当前CPU亲和力掩码 = 0x%llx\n", 
               data->thread_num, (unsigned long long)current_mask);
    }
    
    /* 执行一些工作 */
    for (int i = 0; i < 3; i++) {
        vox_thread_sleep(50);
    }
    
    return 0;
}

static void test_thread_affinity(void) {
    printf("\n=== 测试CPU亲和力 ===\n");
    
    /* 创建内存池 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 获取当前线程的CPU亲和力 */
    uint64_t main_mask;
    if (vox_thread_get_affinity(NULL, &main_mask) == 0) {
        printf("主线程CPU亲和力掩码: 0x%llx\n", (unsigned long long)main_mask);
    }
    
    #define AFFINITY_THREADS 2
    vox_thread_t* threads[AFFINITY_THREADS];
    affinity_test_data_t data[AFFINITY_THREADS];
    
    /* 设置不同的CPU亲和力 */
    uint64_t cpu_masks[AFFINITY_THREADS] = {
        0x1,  /* 绑定到CPU 0 */
        0x2   /* 绑定到CPU 1 */
    };
    
    printf("创建 %d 个线程，设置不同的CPU亲和力...\n", AFFINITY_THREADS);
    for (int i = 0; i < AFFINITY_THREADS; i++) {
        data[i].thread_num = i + 1;
        data[i].cpu_mask = cpu_masks[i];
        
        threads[i] = vox_thread_create(mpool, affinity_worker, &data[i]);
        if (threads[i]) {
            /* 设置线程CPU亲和力 */
            if (vox_thread_set_affinity(threads[i], cpu_masks[i]) == 0) {
                printf("线程 %d CPU亲和力设置成功 (掩码: 0x%llx)\n", 
                       i + 1, (unsigned long long)cpu_masks[i]);
            } else {
                printf("线程 %d CPU亲和力设置失败（可能CPU核心不存在或无权限）\n", i + 1);
            }
        }
    }
    
    /* 等待所有线程完成 */
    for (int i = 0; i < AFFINITY_THREADS; i++) {
        if (threads[i]) {
            vox_thread_join(threads[i], NULL);
        }
    }
    
    vox_mpool_destroy(mpool);
}

int main(void) {
    printf("=== vox_thread 示例程序 ===\n");
    printf("当前线程ID: %llu\n", (unsigned long long)vox_thread_self());
    
    /* 运行各种测试 */
    test_basic_threads();
    test_detached_threads();
    test_thread_local_storage();
    test_thread_id();
    test_thread_yield();
    test_thread_priority();
    test_thread_affinity();
    
    printf("\n=== 所有测试完成 ===\n");
    return 0;
}
