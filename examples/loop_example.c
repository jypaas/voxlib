/*
 * loop_example.c - 事件循环和定时器示例
 */

#include "vox_loop.h"
#include "vox_timer.h"
#include <stdio.h>
#include <stdlib.h>

/* 使用全局变量存储事件循环指针（用于回调中停止循环） */
static vox_loop_t* g_loop = NULL;

/* 定时器回调函数 */
static void timer_callback(vox_timer_t* timer, void* user_data) {
    static int count = 0;
    (void)user_data;  /* 避免未使用警告 */
    
    count++;
    printf("定时器触发: %d\n", count);
    
    /* 5次后停止 */
    if (count >= 5) {
        printf("停止定时器\n");
        vox_timer_stop(timer);
        
        /* 停止事件循环 */
        if (g_loop) {
            vox_loop_stop(g_loop);
        }
    }
}

int main(void) {
    printf("=== 事件循环和定时器示例 ===\n\n");
    
    /* 创建事件循环 */
    vox_loop_t* loop = vox_loop_create();
    if (!loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return 1;
    }
    
    printf("事件循环已创建\n");
    
    /* 保存事件循环指针供回调使用 */
    g_loop = loop;
    
    /* 创建定时器（使用内存池分配） */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) {
        fprintf(stderr, "获取内存池失败\n");
        vox_loop_destroy(loop);
        return 1;
    }
    
    /* 创建定时器 */
    vox_timer_t timer;
    
    /* 初始化定时器 */
    if (vox_timer_init(&timer, loop) != 0) {
        fprintf(stderr, "初始化定时器失败\n");
        vox_loop_destroy(loop);
        return 1;
    }
    
    printf("定时器已初始化\n");
    
    /* 启动定时器：每1秒触发一次 */
    if (vox_timer_start(&timer, 1000, 1000, timer_callback, NULL) != 0) {
        fprintf(stderr, "启动定时器失败\n");
        vox_timer_destroy(&timer);
        vox_loop_destroy(loop);
        return 1;
    }
    
    printf("定时器已启动（每1秒触发一次）\n");
    printf("运行事件循环...\n\n");
    
    /* 运行事件循环 */
    if (vox_loop_run(loop, VOX_RUN_DEFAULT) != 0) {
        fprintf(stderr, "运行事件循环失败\n");
    }
    
    printf("\n事件循环已停止\n");
    
    /* 清理 */
    vox_timer_destroy(&timer);
    vox_loop_destroy(loop);
    
    printf("示例完成\n");
    return 0;
}
