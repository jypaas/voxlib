/*
 * vox_coroutine_scheduler.c - 协程调度器实现
 */

#include "vox_coroutine_scheduler.h"
#include "vox_coroutine.h"
#include "../vox_log.h"
#include <string.h>

/* 调度器结构 */
struct vox_coroutine_scheduler {
    vox_loop_t* loop;
    vox_mpool_t* mpool;
    vox_coroutine_scheduler_config_t config;

    /* 就绪队列 */
    vox_queue_t* ready_queue;

    /* 统计 */
    size_t total_scheduled;
    size_t total_resumed;
    size_t current_ready;
    size_t peak_ready;
    size_t ticks;
};

/* 默认配置 */
#define DEFAULT_READY_QUEUE_CAPACITY  4096
#define DEFAULT_MAX_RESUME_PER_TICK   64
#define DEFAULT_USE_MPSC_QUEUE        true

/* 获取默认配置 */
void vox_coroutine_scheduler_config_default(
    vox_coroutine_scheduler_config_t* config) {
    if (!config) return;
    config->ready_queue_capacity = DEFAULT_READY_QUEUE_CAPACITY;
    config->max_resume_per_tick = DEFAULT_MAX_RESUME_PER_TICK;
    config->use_mpsc_queue = DEFAULT_USE_MPSC_QUEUE;
}

/* 创建调度器 */
vox_coroutine_scheduler_t* vox_coroutine_scheduler_create(
    vox_loop_t* loop,
    const vox_coroutine_scheduler_config_t* config) {

    if (!loop) {
        return NULL;
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) {
        return NULL;
    }

    vox_coroutine_scheduler_t* sched = (vox_coroutine_scheduler_t*)vox_mpool_alloc(
        mpool, sizeof(vox_coroutine_scheduler_t));
    if (!sched) {
        return NULL;
    }

    memset(sched, 0, sizeof(vox_coroutine_scheduler_t));
    sched->loop = loop;
    sched->mpool = mpool;

    /* 应用配置 */
    if (config) {
        sched->config = *config;
    } else {
        vox_coroutine_scheduler_config_default(&sched->config);
    }

    /* 创建就绪队列 */
    vox_queue_config_t queue_config;
    queue_config.initial_capacity = sched->config.ready_queue_capacity;
    queue_config.type = sched->config.use_mpsc_queue ?
                        VOX_QUEUE_TYPE_MPSC : VOX_QUEUE_TYPE_NORMAL;
    queue_config.elem_free = NULL;

    sched->ready_queue = vox_queue_create_with_config(mpool, &queue_config);
    if (!sched->ready_queue) {
        vox_mpool_free(mpool, sched);
        return NULL;
    }

    return sched;
}

/* 销毁调度器 */
void vox_coroutine_scheduler_destroy(vox_coroutine_scheduler_t* sched) {
    if (!sched) return;

    if (sched->ready_queue) {
        vox_queue_destroy(sched->ready_queue);
    }

    vox_mpool_free(sched->mpool, sched);
}

/* 调度协程 */
int vox_coroutine_schedule(vox_coroutine_scheduler_t* sched,
                           vox_coroutine_t* co) {
    if (!sched || !co) {
        return -1;
    }

    if (vox_queue_enqueue(sched->ready_queue, co) != 0) {
        VOX_LOG_WARN("Failed to enqueue coroutine to ready queue");
        return -1;
    }

    sched->total_scheduled++;
    sched->current_ready++;
    if (sched->current_ready > sched->peak_ready) {
        sched->peak_ready = sched->current_ready;
    }

    return 0;
}

/* 执行一次调度 tick */
size_t vox_coroutine_scheduler_tick(vox_coroutine_scheduler_t* sched) {
    if (!sched) return 0;

    sched->ticks++;
    size_t resumed = 0;
    size_t max_resume = sched->config.max_resume_per_tick;

    while (resumed < max_resume && !vox_queue_empty(sched->ready_queue)) {
        vox_coroutine_t* co = (vox_coroutine_t*)vox_queue_dequeue(sched->ready_queue);
        if (!co) break;

        sched->current_ready--;

        /* 恢复协程执行 */
        if (vox_coroutine_resume(co) == 0) {
            resumed++;
            sched->total_resumed++;
        }
    }

    return resumed;
}

/* 获取就绪队列中的协程数量 */
size_t vox_coroutine_scheduler_ready_count(
    const vox_coroutine_scheduler_t* sched) {
    if (!sched) return 0;
    return sched->current_ready;
}

/* 检查调度器是否为空 */
bool vox_coroutine_scheduler_empty(const vox_coroutine_scheduler_t* sched) {
    if (!sched) return true;
    return sched->current_ready == 0;
}

/* 获取统计信息 */
void vox_coroutine_scheduler_get_stats(
    const vox_coroutine_scheduler_t* sched,
    vox_coroutine_scheduler_stats_t* stats) {
    if (!sched || !stats) return;

    stats->total_scheduled = sched->total_scheduled;
    stats->total_resumed = sched->total_resumed;
    stats->current_ready = sched->current_ready;
    stats->peak_ready = sched->peak_ready;
    stats->ticks = sched->ticks;
}
