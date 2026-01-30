/*
 * vox_timer.c - 定时器系统实现
 */

#include "vox_timer.h"
#include "vox_loop.h"
#include "vox_mheap.h"
#include "vox_time.h"
#include "vox_mpool.h"
#include "vox_os.h"
#include <string.h>
#include <stdlib.h>

/* 前向声明 */
vox_mheap_t* vox_loop_get_timers(vox_loop_t* loop);

/* 定时器比较函数（用于最小堆） */
/* 注意：目前未使用，保留以备将来使用 */
VOX_UNUSED_FUNC static int timer_cmp(const void* a, const void* b) {
    const vox_timer_t* timer_a = (const vox_timer_t*)a;
    const vox_timer_t* timer_b = (const vox_timer_t*)b;
    
    if (timer_a->timeout < timer_b->timeout) return -1;
    if (timer_a->timeout > timer_b->timeout) return 1;
    return 0;
}

/* 初始化定时器 */
int vox_timer_init(vox_timer_t* timer, vox_loop_t* loop) {
    if (!timer || !loop) {
        return -1;
    }
    
    memset(timer, 0, sizeof(vox_timer_t));
    timer->loop = loop;
    timer->active = false;
    
    return 0;
}

/* 启动定时器 */
int vox_timer_start(vox_timer_t* timer, uint64_t timeout_ms, 
                    uint64_t repeat_ms, vox_timer_cb cb, void* user_data) {
    if (!timer || !timer->loop || !cb) {
        return -1;
    }
    
    /* 如果已经活跃，先停止 */
    if (timer->active) {
        vox_timer_stop(timer);
    }
    
    /* 设置定时器参数 */
    uint64_t now = vox_loop_now(timer->loop);
    timer->timeout = now + (timeout_ms * 1000);  /* 转换为微秒 */
    timer->repeat = repeat_ms * 1000;             /* 转换为微秒 */
    timer->callback = cb;
    timer->user_data = user_data;
    timer->active = true;
    
    /* 添加到定时器堆 */
    vox_mheap_t* timers = vox_loop_get_timers(timer->loop);
    if (!timers) {
        timer->active = false;
        return -1;
    }
    
    /* 重新配置堆的比较函数（如果还没有配置） */
    /* 注意：这里简化处理，实际应该在使用前配置好 */
    
    if (vox_mheap_push(timers, timer) != 0) {
        timer->active = false;
        return -1;
    }
    
    return 0;
}

/* 停止定时器 */
int vox_timer_stop(vox_timer_t* timer) {
    if (!timer || !timer->active) {
        return -1;
    }

    /* 标记为非活跃 */
    timer->active = false;

    /* 从堆中删除定时器 */
    vox_mheap_t* timers = vox_loop_get_timers(timer->loop);
    if (timers) {
        vox_mheap_remove(timers, timer);
    }

    return 0;
}

/* 重启定时器 */
int vox_timer_again(vox_timer_t* timer) {
    if (!timer || !timer->loop) {
        return -1;
    }

    if (!timer->active) {
        return -1;  /* 定时器未启动 */
    }

    if (timer->repeat == 0) {
        return -1;  /* 定时器不重复 */
    }

    vox_mheap_t* timers = vox_loop_get_timers(timer->loop);
    if (!timers) {
        return -1;
    }

    /* 从堆中移除 */
    vox_mheap_remove(timers, timer);

    /* 重新计算到期时间 */
    uint64_t now = vox_loop_now(timer->loop);
    timer->timeout = now + timer->repeat;

    /* 重新添加到堆 */
    if (vox_mheap_push(timers, timer) != 0) {
        timer->active = false;
        return -1;
    }

    return 0;
}

/* 检查定时器是否活跃 */
bool vox_timer_is_active(const vox_timer_t* timer) {
    return timer && timer->active;
}

/* 获取定时器的重复间隔 */
uint64_t vox_timer_get_repeat(const vox_timer_t* timer) {
    if (!timer) {
        return 0;
    }
    return timer->repeat / 1000;  /* 转换为毫秒 */
}

/* 设置定时器的重复间隔 */
int vox_timer_set_repeat(vox_timer_t* timer, uint64_t repeat_ms) {
    if (!timer) {
        return -1;
    }
    
    timer->repeat = repeat_ms * 1000;  /* 转换为微秒 */
    return 0;
}

/* 销毁定时器 */
void vox_timer_destroy(vox_timer_t* timer) {
    if (!timer) {
        return;
    }
    
    if (timer->active) {
        vox_timer_stop(timer);
    }
    
    memset(timer, 0, sizeof(vox_timer_t));
}

/* 处理到期的定时器（内部函数，由事件循环调用） */
void vox_timer_process_expired(vox_loop_t* loop) {
    if (!loop) {
        return;
    }

    vox_mheap_t* timers = vox_loop_get_timers(loop);
    if (!timers) {
        return;
    }

    uint64_t now = vox_loop_now(loop);

    /* 处理所有到期的定时器 */
    while (!vox_mheap_empty(timers)) {
        vox_timer_t* timer = (vox_timer_t*)vox_mheap_peek(timers);
        if (!timer) {
            break;
        }

        /* 如果定时器已停止，移除并继续检查下一个 */
        if (!timer->active) {
            vox_mheap_pop(timers);
            continue;
        }

        /* 检查是否到期 */
        if (timer->timeout > now) {
            break;  /* 还没有到期 */
        }

        /* 移除定时器 */
        vox_mheap_pop(timers);

        /* 执行回调 */
        if (timer->callback) {
            timer->callback(timer, timer->user_data);
        }

        /* 如果是重复定时器，重新添加 */
        if (timer->repeat > 0 && timer->active) {
            timer->timeout = now + timer->repeat;
            vox_mheap_push(timers, timer);
        } else {
            timer->active = false;
        }
    }
}

/* 获取下一次定时器到期的时间间隔（毫秒） */
int vox_timer_get_next_timeout(vox_loop_t* loop) {
    if (!loop) {
        return -1;
    }

    vox_mheap_t* timers = vox_loop_get_timers(loop);
    if (!timers || vox_mheap_empty(timers)) {
        return -1;  /* 无限等待 */
    }

    /* 跳过非活跃的定时器 */
    vox_timer_t* timer = (vox_timer_t*)vox_mheap_peek(timers);
    while (timer && !timer->active) {
        vox_mheap_pop(timers);
        if (vox_mheap_empty(timers)) {
            return -1;
        }
        timer = (vox_timer_t*)vox_mheap_peek(timers);
    }

    if (!timer) {
        return -1;
    }

    uint64_t now = vox_loop_now(loop);
    if (timer->timeout <= now) {
        return 0;  /* 已到期 */
    }

    uint64_t diff = timer->timeout - now;
    return (int)(diff / 1000);  /* 转换为毫秒 */
}
