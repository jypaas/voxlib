/*
 * loop_queue_work_example.c - vox_loop_queue_work 使用示例
 * 演示如何在事件循环中排队执行回调，包括跨线程使用
 */

#include "../vox_loop.h"
#include "../vox_timer.h"
#include "../vox_thread.h"
#include "../vox_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 全局变量 */
static vox_loop_t* g_loop = NULL;
static int g_task_counter = 0;
static int g_thread_task_counter = 0;

/* 任务数据结构 */
typedef struct {
    int task_id;
    const char* task_name;
    uint64_t submit_time;
} task_data_t;

/* 回调函数：处理排队的任务 */
static void task_callback(vox_loop_t* loop, void* user_data) {
    task_data_t* data = (task_data_t*)user_data;
    /* 使用 vox_time_monotonic() 获取实际执行时间，而不是 vox_loop_now(loop)，
     * 因为 loop_time 只在每次迭代开始时更新一次，不能反映任务实际执行时间 */
    uint64_t now = vox_time_monotonic();
    uint64_t delay = (now - data->submit_time) / 1000;  /* 转换为毫秒 */
    
    printf("[任务 #%d] %s - 延迟: %llu 毫秒\n", 
           data->task_id, data->task_name, (unsigned long long)delay);
    
    /* 释放任务数据 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_mpool_free(mpool, data);
}

/* 回调函数：批量任务处理 */
static void batch_task_callback(vox_loop_t* loop, void* user_data) {
    int* count = (int*)user_data;
    printf("[批量任务] 处理第 %d 个批量任务\n", (*count)++);
    
    /* 如果达到10个，停止事件循环 */
    if (*count >= 10) {
        printf("[批量任务] 已完成10个任务，停止事件循环\n");
        vox_loop_stop(loop);
    }
}

/* 回调函数：停止事件循环 */
static void stop_loop_callback(vox_loop_t* loop, void* user_data) {
    (void)user_data;
    printf("[停止任务] 收到停止请求，停止事件循环\n");
    vox_loop_stop(loop);
}

/* 定时器回调：定期提交任务 */
static void timer_callback(vox_timer_t* timer, void* user_data) {
    static int timer_count = 0;
    (void)timer;
    
    timer_count++;
    vox_loop_t* loop = (vox_loop_t*)user_data;
    
    /* 从定时器回调中提交任务 */
    task_data_t* data = (task_data_t*)vox_mpool_alloc(
        vox_loop_get_mpool(loop), sizeof(task_data_t));
    if (data) {
        data->task_id = ++g_task_counter;
        data->task_name = "定时器触发的任务";
        /* 使用 vox_time_monotonic() 确保时间戳准确 */
        data->submit_time = vox_time_monotonic();
        
        if (vox_loop_queue_work(loop, task_callback, data) == 0) {
            printf("[定时器] 已提交任务 #%d\n", data->task_id);
        } else {
            printf("[定时器] 提交任务失败\n");
            vox_mpool_free(vox_loop_get_mpool(loop), data);
        }
    }
    
    /* 5次后停止定时器 */
    if (timer_count >= 5) {
        printf("[定时器] 定时器已触发5次，停止定时器\n");
        vox_timer_stop(timer);
    }
}

/* 工作线程函数：从其他线程提交任务 */
static int worker_thread_func(void* arg) {
    vox_loop_t* loop = (vox_loop_t*)arg;
    
    printf("[工作线程] 线程启动，准备提交任务\n");
    
    /* 等待一小段时间，确保事件循环已启动 */
    vox_time_sleep_ms(500);
    
    /* 从工作线程提交多个任务 */
    for (int i = 0; i < 5; i++) {
        task_data_t* data = (task_data_t*)vox_mpool_alloc(
            vox_loop_get_mpool(loop), sizeof(task_data_t));
        if (data) {
            data->task_id = ++g_thread_task_counter;
            data->task_name = "工作线程提交的任务";
            /* 在工作线程中使用 vox_time_monotonic() 而不是 vox_loop_now()，
             * 因为 loop_time 只在事件循环主线程中更新，线程安全 */
            data->submit_time = vox_time_monotonic();
            
            if (vox_loop_queue_work(loop, task_callback, data) == 0) {
                printf("[工作线程] 已提交任务 #%d\n", data->task_id);
            } else {
                printf("[工作线程] 提交任务失败\n");
                vox_mpool_free(vox_loop_get_mpool(loop), data);
            }
        }
        
        /* 任务之间稍作延迟 */
        vox_time_sleep_ms(200);
    }
    
    printf("[工作线程] 线程完成，准备退出\n");
    
    /* 提交停止任务，停止事件循环 */
    if (vox_loop_queue_work(loop, stop_loop_callback, NULL) == 0) {
        printf("[工作线程] 已提交停止任务\n");
    }
    
    return 0;
}

/* 示例1：基本用法 - 在事件循环中排队任务 */
static void example_basic_usage(void) {
    printf("\n=== 示例1：基本用法 ===\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    printf("事件循环已创建\n");
    
    /* 提交几个任务 */
    for (int i = 0; i < 3; i++) {
        task_data_t* data = (task_data_t*)vox_mpool_alloc(
            vox_loop_get_mpool(loop), sizeof(task_data_t));
        if (data) {
            data->task_id = ++g_task_counter;
            data->task_name = "基本任务";
            /* 使用 vox_time_monotonic() 确保时间戳准确 */
            data->submit_time = vox_time_monotonic();
            
            if (vox_loop_queue_work(loop, task_callback, data) == 0) {
                printf("已提交任务 #%d\n", data->task_id);
            } else {
                vox_mpool_free(vox_loop_get_mpool(loop), data);
            }
        }
    }
    
    /* 提交停止任务 */
    if (vox_loop_queue_work(loop, stop_loop_callback, NULL) == 0) {
        printf("已提交停止任务\n");
    }
    
    printf("运行事件循环...\n");
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    printf("事件循环已停止\n");
    vox_loop_destroy(loop);
}

/* 示例2：与定时器结合使用 */
static void example_with_timer(void) {
    printf("\n=== 示例2：与定时器结合使用 ===\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    g_loop = loop;
    
    /* 创建定时器 */
    vox_timer_t timer;
    
    if (vox_timer_init(&timer, loop) != 0) {
        fprintf(stderr, "初始化定时器失败\n");
        vox_loop_destroy(loop);
        return;
    }
    
    /* 启动定时器：每500毫秒触发一次 */
    if (vox_timer_start(&timer, 500, 500, timer_callback, loop) != 0) {
        fprintf(stderr, "启动定时器失败\n");
        vox_timer_destroy(&timer);
        vox_loop_destroy(loop);
        return;
    }
    
    printf("定时器已启动（每500毫秒触发一次）\n");
    printf("定时器回调会提交任务到事件循环\n");
    printf("运行事件循环...\n\n");
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    printf("\n事件循环已停止\n");
    
    /* 清理 */
    vox_timer_destroy(&timer);
    vox_loop_destroy(loop);
    g_loop = NULL;
}

/* 示例3：跨线程使用 */
static void example_cross_thread(void) {
    printf("\n=== 示例3：跨线程使用 ===\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    printf("事件循环已创建\n");
    printf("创建工作线程...\n");
    
    /* 创建内存池用于线程 */
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        vox_loop_destroy(loop);
        return;
    }
    
    /* 创建工作线程 */
    vox_thread_t* worker_thread = vox_thread_create(mpool, worker_thread_func, loop);
    if (!worker_thread) {
        fprintf(stderr, "创建工作线程失败\n");
        vox_mpool_destroy(mpool);
        vox_loop_destroy(loop);
        return;
    }
    
    printf("工作线程已创建\n");
    printf("运行事件循环（等待工作线程提交任务）...\n\n");
    
    /* 运行事件循环 */
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    printf("\n事件循环已停止，等待工作线程退出...\n");
    
    /* 等待工作线程退出 */
    int exit_code;
    if (vox_thread_join(worker_thread, &exit_code) == 0) {
        printf("工作线程已退出，退出码: %d\n", exit_code);
    } else {
        printf("等待工作线程失败\n");
    }
    
    vox_loop_destroy(loop);
    vox_mpool_destroy(mpool);
}

/* 示例4：批量任务处理 */
static void example_batch_processing(void) {
    printf("\n=== 示例4：批量任务处理 ===\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    printf("事件循环已创建\n");
    
    /* 提交多个批量任务 */
    int batch_count = 0;
    for (int i = 0; i < 10; i++) {
        if (vox_loop_queue_work(loop, batch_task_callback, &batch_count) == 0) {
            printf("已提交批量任务 #%d\n", i + 1);
        }
    }
    
    printf("运行事件循环...\n");
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    
    printf("事件循环已停止\n");
    vox_loop_destroy(loop);
}

/* 示例5：立即执行 vs 延迟执行 */
static void immediate_callback(vox_loop_t* loop, void* user_data) {
    VOX_UNUSED(loop);
    VOX_UNUSED(user_data);
    printf("[立即执行] 任务立即执行\n");
}

static void delayed_callback(vox_loop_t* loop, void* user_data) {
    VOX_UNUSED(loop);
    VOX_UNUSED(user_data);
    printf("[延迟执行] 任务在下次迭代执行\n");
}

static void example_immediate_vs_delayed(void) {
    printf("\n=== 示例5：立即执行 vs 延迟执行 ===\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return;
    }
    
    printf("事件循环已创建\n");
    
    /* 提交延迟执行的任务 */
    printf("提交延迟执行的任务...\n");
    vox_loop_queue_work(loop, delayed_callback, NULL);
    
    /* 立即执行任务 */
    printf("立即执行任务...\n");
    vox_loop_queue_work_immediate(loop, immediate_callback, NULL);
    
    /* 再提交一个延迟执行的任务 */
    printf("再提交一个延迟执行的任务...\n");
    vox_loop_queue_work(loop, delayed_callback, NULL);
    
    /* 提交停止任务 */
    vox_loop_queue_work(loop, stop_loop_callback, NULL);
    
    printf("运行事件循环（单次迭代）...\n");
    vox_loop_run(loop, VOX_RUN_ONCE);
    
    printf("事件循环已停止\n");
    vox_loop_destroy(loop);
}

int main(int argc, char* argv[]) {
    printf("=== vox_loop_queue_work 使用示例 ===\n");
    printf("演示事件循环中的任务排队机制\n\n");
    
    if (argc > 1) {
        int example_num = atoi(argv[1]);
        switch (example_num) {
            case 1:
                example_basic_usage();
                break;
            case 2:
                example_with_timer();
                break;
            case 3:
                example_cross_thread();
                break;
            case 4:
                example_batch_processing();
                break;
            case 5:
                example_immediate_vs_delayed();
                break;
            default:
                fprintf(stderr, "未知示例编号: %d\n", example_num);
                return 1;
        }
    } else {
        /* 运行所有示例 */
        example_basic_usage();
        vox_time_sleep_ms(500);
        
        example_with_timer();
        vox_time_sleep_ms(500);
        
        example_cross_thread();
        vox_time_sleep_ms(500);
        
        example_batch_processing();
        vox_time_sleep_ms(500);
        
        example_immediate_vs_delayed();
    }
    
    printf("\n所有示例完成！\n");
    return 0;
}
