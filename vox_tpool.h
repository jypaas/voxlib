/*
 * vox_tpool.h - 高性能线程池
 * 基于 vox_mpool 和 vox_queue 实现
 */

#ifndef VOX_TPOOL_H
#define VOX_TPOOL_H

#include "vox_mpool.h"
#include "vox_queue.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 线程池不透明类型 */
#ifndef VOX_TPOOL_T_DEFINED
#define VOX_TPOOL_T_DEFINED
typedef struct vox_tpool vox_tpool_t;
#endif

/* 任务函数类型 */
typedef void (*vox_tpool_task_func_t)(void* user_data);

/* 任务完成回调函数类型（可选） */
typedef void (*vox_tpool_complete_func_t)(void* user_data, int result);

/* 线程池配置 */
typedef struct {
    size_t thread_count;        /* 线程数量，0表示使用CPU核心数 */
    size_t queue_capacity;      /* 任务队列容量，0表示使用默认值1024 */
    int thread_priority;        /* 线程优先级（使用vox_thread_priority_t），-1表示使用默认 */
    vox_queue_type_t queue_type; /* 队列类型，VOX_QUEUE_TYPE_MPSC（默认）或VOX_QUEUE_TYPE_NORMAL */
} vox_tpool_config_t;

/**
 * 使用默认配置创建线程池
 * @return 成功返回线程池指针，失败返回NULL
 */
vox_tpool_t* vox_tpool_create(void);

/**
 * 使用自定义配置创建线程池
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回线程池指针，失败返回NULL
 */
vox_tpool_t* vox_tpool_create_with_config(const vox_tpool_config_t* config);

/**
 * 提交任务到线程池
 * @param tpool 线程池指针
 * @param task_func 任务函数，必须非NULL
 * @param user_data 传递给任务函数的用户数据，可为NULL
 * @param complete_func 任务完成回调函数，可为NULL
 * @return 成功返回0，失败返回-1
 */
int vox_tpool_submit(vox_tpool_t* tpool, vox_tpool_task_func_t task_func, 
                     void* user_data, vox_tpool_complete_func_t complete_func);

/**
 * 等待所有任务完成
 * @param tpool 线程池指针
 * @return 成功返回0，失败返回-1
 */
int vox_tpool_wait(vox_tpool_t* tpool);

/**
 * 关闭线程池（停止接受新任务，等待所有任务完成）
 * @param tpool 线程池指针
 * @return 成功返回0，失败返回-1
 */
int vox_tpool_shutdown(vox_tpool_t* tpool);

/**
 * 强制关闭线程池（立即停止，不等待任务完成）
 * @param tpool 线程池指针
 */
void vox_tpool_force_shutdown(vox_tpool_t* tpool);

/**
 * 获取线程池中当前待处理的任务数量
 * @param tpool 线程池指针
 * @return 返回任务数量
 */
size_t vox_tpool_pending_tasks(const vox_tpool_t* tpool);

/**
 * 获取线程池中正在执行的任务数量
 * @param tpool 线程池指针
 * @return 返回正在执行的任务数量
 */
size_t vox_tpool_running_tasks(const vox_tpool_t* tpool);

/**
 * 获取线程池统计信息
 * @param tpool 线程池指针
 * @param total_tasks 输出总任务数，可为NULL
 * @param completed_tasks 输出已完成任务数，可为NULL
 * @param failed_tasks 输出失败任务数，可为NULL
 */
void vox_tpool_stats(const vox_tpool_t* tpool, 
                     size_t* total_tasks, 
                     size_t* completed_tasks, 
                     size_t* failed_tasks);

/**
 * 销毁线程池并释放所有资源
 * @param tpool 线程池指针
 */
void vox_tpool_destroy(vox_tpool_t* tpool);

#ifdef __cplusplus
}
#endif

#endif /* VOX_TPOOL_H */
