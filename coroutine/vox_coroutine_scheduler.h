/*
 * vox_coroutine_scheduler.h - 协程调度器
 * 提供简单的就绪队列调度，集成到事件循环
 */

#ifndef VOX_COROUTINE_SCHEDULER_H
#define VOX_COROUTINE_SCHEDULER_H

#include "../vox_os.h"
#include "../vox_queue.h"
#include "../vox_list.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_coroutine_scheduler vox_coroutine_scheduler_t;

/* 前向声明（避免循环依赖） */
/* 注意：vox_loop_t 和 vox_coroutine_t 的完整定义在包含此头文件的文件中提供 */
struct vox_loop;
struct vox_coroutine;

/* 条件编译：只有在未定义时才定义这些类型 */
/* 这样可以避免在 vox_coroutine.h 包含此头文件时重复定义 */
#ifndef VOX_LOOP_T_DEFINED
#define VOX_LOOP_T_DEFINED
typedef struct vox_loop vox_loop_t;
#endif

#ifndef VOX_COROUTINE_T_DEFINED
#define VOX_COROUTINE_T_DEFINED
typedef struct vox_coroutine vox_coroutine_t;
#endif

/* ===== 调度器配置 ===== */

typedef struct vox_coroutine_scheduler_config {
    size_t ready_queue_capacity;   /* 就绪队列容量 (默认: 4096) */
    size_t max_resume_per_tick;    /* 每 tick 最大恢复数 (默认: 64) */
    bool use_mpsc_queue;           /* 使用 MPSC 队列 (默认: true) */
} vox_coroutine_scheduler_config_t;

/* ===== 调度器统计 ===== */

typedef struct vox_coroutine_scheduler_stats {
    size_t total_scheduled;        /* 总调度次数 */
    size_t total_resumed;          /* 总恢复次数 */
    size_t current_ready;          /* 当前就绪数 */
    size_t peak_ready;             /* 峰值就绪数 */
    size_t ticks;                  /* tick 次数 */
} vox_coroutine_scheduler_stats_t;

/* ===== API 函数 ===== */

/**
 * 获取默认配置
 * @param config 配置结构指针
 */
void vox_coroutine_scheduler_config_default(
    vox_coroutine_scheduler_config_t* config);

/**
 * 创建调度器
 * @param loop 事件循环指针
 * @param config 配置 (NULL 使用默认配置)
 * @return 成功返回调度器指针，失败返回 NULL
 */
vox_coroutine_scheduler_t* vox_coroutine_scheduler_create(
    vox_loop_t* loop,
    const vox_coroutine_scheduler_config_t* config);

/**
 * 销毁调度器
 * @param sched 调度器指针
 */
void vox_coroutine_scheduler_destroy(vox_coroutine_scheduler_t* sched);

/**
 * 调度协程 (加入就绪队列)
 * @param sched 调度器指针
 * @param co 协程指针
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_schedule(vox_coroutine_scheduler_t* sched,
                           vox_coroutine_t* co);

/**
 * 执行一次调度 tick
 * 从就绪队列中取出协程并恢复执行
 * @param sched 调度器指针
 * @return 恢复的协程数量
 */
size_t vox_coroutine_scheduler_tick(vox_coroutine_scheduler_t* sched);

/**
 * 获取就绪队列中的协程数量
 * @param sched 调度器指针
 * @return 就绪协程数量
 */
size_t vox_coroutine_scheduler_ready_count(
    const vox_coroutine_scheduler_t* sched);

/**
 * 检查调度器是否为空
 * @param sched 调度器指针
 * @return 为空返回 true
 */
bool vox_coroutine_scheduler_empty(const vox_coroutine_scheduler_t* sched);

/**
 * 获取统计信息
 * @param sched 调度器指针
 * @param stats 统计结构指针
 */
void vox_coroutine_scheduler_get_stats(
    const vox_coroutine_scheduler_t* sched,
    vox_coroutine_scheduler_stats_t* stats);

#ifdef __cplusplus
}
#endif

#endif /* VOX_COROUTINE_SCHEDULER_H */
