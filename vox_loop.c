/*
 * vox_loop.c - 事件循环核心实现
 */

#include "vox_loop.h"
#include "vox_backend.h"
#include "vox_handle.h"
#include "vox_tpool.h"
#include "vox_os.h"
#include "vox_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* 前向声明 TCP/UDP 回调函数 */
#ifdef __cplusplus
extern "C" {
#endif
void vox_tcp_backend_event_cb(vox_backend_t* backend, int fd, uint32_t events, void* user_data, void* overlapped, size_t bytes_transferred);
void vox_udp_backend_event_cb(vox_backend_t* backend, int fd, uint32_t events, void* user_data, void* overlapped, size_t bytes_transferred);
#ifdef __cplusplus
}
#endif

/* 回调工作项 */
typedef struct vox_work_item {
    vox_loop_cb callback;
    void* user_data;
} vox_work_item_t;

/* 前向声明 */
static int calculate_poll_timeout(vox_loop_t* loop);
extern void vox_timer_process_expired(vox_loop_t* loop);
extern int vox_timer_get_next_timeout(vox_loop_t* loop);

/* 事件回调函数（用于 backend poll） */
static void handle_backend_event(vox_backend_t* backend, int fd,
                                 uint32_t events, void* user_data,
                                 void* overlapped, size_t bytes_transferred);
static void handle_backend_event(vox_backend_t* backend, int fd,
                                 uint32_t events, void* user_data,
                                 void* overlapped, size_t bytes_transferred) {
    if (!user_data) {
        return;
    }

    /* user_data 是指向句柄相关内部数据的指针 */
    /* 对于 TCP，它是 vox_tcp_internal_data_t*，其中包含 tcp 指针 */
    /* 从 tcp 指针可以获取 handle（第一个成员），然后获取类型 */

    /* 尝试识别 TCP 句柄：检查 user_data 结构 */
    /* TCP 内部数据结构：{ vox_tcp_t* tcp; void* user_data; } */
    /* 我们可以通过检查 tcp 指针的第一个成员（handle）来判断类型 */

    /* 假设 user_data 是内部数据结构，包含句柄指针 */
    /* TCP/UDP 内部数据结构：{ handle_type* handle; void* user_data; } */
    typedef struct {
        void* handle_ptr;  /* vox_tcp_t* 或 vox_udp_t* */
        void* user_data;
    } handle_internal_data_t;

    handle_internal_data_t* internal_data = (handle_internal_data_t*)user_data;

    if (internal_data && internal_data->handle_ptr) {
        /* 获取句柄（TCP/UDP 结构的第一个成员都是 handle） */
        /* 注意：handle_ptr 实际上指向 vox_tcp_t* 或 vox_udp_t*，其第一个成员是 handle */
        vox_handle_t* handle = (vox_handle_t*)internal_data->handle_ptr;

        /* 根据句柄类型分发事件 */
        if (handle && handle->type == VOX_HANDLE_TCP) {
            /* 调用 TCP 的事件回调 */
            vox_tcp_backend_event_cb(backend, fd, events, user_data, overlapped, bytes_transferred);
            return;
        } else if (handle && handle->type == VOX_HANDLE_UDP) {
            /* 调用 UDP 的事件回调 */
            vox_udp_backend_event_cb(backend, fd, events, user_data, overlapped, bytes_transferred);
            return;
        }
    }

    /* 未来可以在这里添加其他句柄类型的事件处理 */
    /* 例如：文件、管道等 */
}

/* 事件循环结构 */
struct vox_loop {
    vox_mpool_t* mpool;              /* 内存池 */
    bool own_mpool;                   /* 是否拥有内存池 */
    
    /* 平台抽象层 backend */
    vox_backend_t* backend;
    
    /* 事件队列 */
    vox_queue_t* pending_events;     /* 待处理事件 */
    vox_queue_t* pending_callbacks;  /* 待执行回调 */
    
    /* 定时器管理（最小堆） */
    vox_mheap_t* timers;
    
    /* 活跃句柄列表 */
    vox_list_t active_handles;
    
    /* 关闭句柄列表 */
    vox_list_t closing_handles;
    
    /* 线程池（用于阻塞操作） */
    void* thread_pool;  /* vox_tpool_t*，暂时用void*避免循环依赖 */
    
    /* 状态 */
    bool stop_flag;
    bool running;
    uint64_t loop_time;              /* 当前循环时间（微秒） */
    size_t active_handles_count;
    size_t ref_count;                /* 引用计数：协程 await 等未完成时 >0，防止 loop 提前退出 */
};

/* 定时器比较函数（用于最小堆） */
static int timer_cmp(const void* a, const void* b) {
    /* 实际实现在 vox_timer.c 中 */
    /* 这里提供一个默认实现，但应该使用 vox_timer.c 中的版本 */
    typedef struct { uint64_t timeout; } timer_struct_t;
    const timer_struct_t* timer_a = (const timer_struct_t*)a;
    const timer_struct_t* timer_b = (const timer_struct_t*)b;
    
    if (timer_a->timeout < timer_b->timeout) return -1;
    if (timer_a->timeout > timer_b->timeout) return 1;
    return 0;
}

/* 创建事件循环 */
vox_loop_t* vox_loop_create(void) {
    return vox_loop_create_with_config(NULL);
}

/* 使用配置创建事件循环 */
vox_loop_t* vox_loop_create_with_config(const vox_loop_config_t* config) {
    vox_mpool_t* pool = NULL;
    bool own_mpool = false;
    
    /* 处理内存池配置 */
    if (config && config->mpool) {
        /* 使用提供的内存池 */
        pool = config->mpool;
        own_mpool = false;
    } else {
        /* 创建新的内存池（必须是线程安全的，因为 vox_loop_queue_work 支持跨线程调用） */
        vox_mpool_config_t mpool_cfg = {
            .thread_safe = 1,
            .initial_block_count = 1024  /* 预分配更多块以支持高并发 */
        };
        
        if (config && config->mpool_config) {
            /* 使用配置中的内存池配置，但确保线程安全 */
            mpool_cfg = *config->mpool_config;
            mpool_cfg.thread_safe = 1;  /* 强制线程安全 */
        }
        
        pool = vox_mpool_create_with_config(&mpool_cfg);
        if (!pool) {
            return NULL;
        }
        own_mpool = true;
    }
    
    /* 分配事件循环结构 */
    vox_loop_t* loop = (vox_loop_t*)vox_mpool_alloc(pool, sizeof(vox_loop_t));
    if (!loop) {
        if (own_mpool) {
            vox_mpool_destroy(pool);
        }
        return NULL;
    }
    
    memset(loop, 0, sizeof(vox_loop_t));
    loop->mpool = pool;
    loop->own_mpool = own_mpool;
    loop->stop_flag = false;
    loop->running = false;
    loop->loop_time = vox_time_monotonic();
    loop->active_handles_count = 0;
    loop->ref_count = 0;
    
    /* 初始化活跃句柄列表 */
    vox_list_init(&loop->active_handles);
    
    /* 初始化关闭句柄列表 */
    vox_list_init(&loop->closing_handles);
    
    /* 创建事件队列 */
    if (config && config->pending_events_config) {
        loop->pending_events = vox_queue_create_with_config(pool, config->pending_events_config);
    } else {
        loop->pending_events = vox_queue_create(pool);
    }
    if (!loop->pending_events) {
        goto error;
    }
    
    /* 待执行回调使用 MPSC 队列，确保其他线程可以安全提交任务 */
    vox_queue_config_t callback_queue_config = {
        .type = VOX_QUEUE_TYPE_MPSC,
        .initial_capacity = 4096,  /* 初始容量调大以支持高并发 */
        .elem_free = NULL
    };
    
    if (config && config->pending_callbacks_config) {
        /* 使用配置中的回调队列配置，但确保类型是 MPSC */
        callback_queue_config = *config->pending_callbacks_config;
        callback_queue_config.type = VOX_QUEUE_TYPE_MPSC;  /* 强制 MPSC 类型 */
    }
    
    loop->pending_callbacks = vox_queue_create_with_config(pool, &callback_queue_config);
    if (!loop->pending_callbacks) {
        goto error;
    }
    
    /* 创建定时器堆 */
    vox_mheap_config_t timer_config = {
        .initial_capacity = 64,
        .cmp_func = timer_cmp,
        .elem_free = NULL
    };
    loop->timers = vox_mheap_create_with_config(pool, &timer_config);
    if (!loop->timers) {
        goto error;
    }
    
    /* 创建并初始化平台抽象层 backend */
    vox_backend_config_t backend_cfg = {0};
    backend_cfg.mpool = pool;
    backend_cfg.max_events = 0;
    backend_cfg.type = VOX_BACKEND_TYPE_AUTO;  /* 默认自动选择 */
    
    if (config && config->backend_config) {
        /* 使用配置中的 backend 配置 */
        backend_cfg = *config->backend_config;
        backend_cfg.mpool = pool;  /* 确保使用 loop 的内存池 */
    }
    
    loop->backend = vox_backend_create_with_config(&backend_cfg);
    if (!loop->backend) {
        goto error;
    }
    
    if (vox_backend_init(loop->backend) != 0) {
        vox_backend_destroy(loop->backend);
        loop->backend = NULL;
        goto error;
    }
    
    /* 创建线程池（用于阻塞操作，如DNS解析、文件IO等） */
    vox_tpool_config_t tpool_config = {
        .thread_count = 4,  /* 默认4个线程 */
        .queue_capacity = 1024,
        .thread_priority = -1,
        .queue_type = VOX_QUEUE_TYPE_MPSC
    };
    
    /* 如果提供了线程池配置，使用提供的配置 */
    if (config && config->tpool_config) {
        tpool_config = *config->tpool_config;
    }
    
    loop->thread_pool = (void*)vox_tpool_create_with_config(&tpool_config);
    if (!loop->thread_pool) {
        VOX_LOG_ERROR("创建事件循环线程池失败");
        goto error;
    }
    
    return loop;

error:
    if (loop->thread_pool) {
        vox_tpool_shutdown((vox_tpool_t*)loop->thread_pool);
        vox_tpool_destroy((vox_tpool_t*)loop->thread_pool);
        loop->thread_pool = NULL;
    }
    if (loop->backend) {
        vox_backend_destroy(loop->backend);
        loop->backend = NULL;
    }
    if (loop->pending_events) {
        vox_queue_destroy(loop->pending_events);
    }
    if (loop->pending_callbacks) {
        vox_queue_destroy(loop->pending_callbacks);
    }
    if (loop->timers) {
        vox_mheap_destroy(loop->timers);
    }
    vox_mpool_free(pool, loop);
    if (own_mpool) {
        vox_mpool_destroy(pool);
    }
    return NULL;
}

/* 运行事件循环 */
int vox_loop_run(vox_loop_t* loop, vox_run_mode_t mode) {
    if (!loop) {
        return -1;
    }
    
    if (loop->running) {
        return -1;  /* 已经在运行 */
    }
    
    loop->running = true;
    loop->stop_flag = false;

    while (!loop->stop_flag) {
        
        /* 更新循环时间 */
        vox_loop_update_time(loop);
        
        /* 处理到期的定时器 */
        vox_timer_process_expired(loop);
        
        /* 处理待执行的回调 - 限制单次循环处理数量，避免阻塞事件循环 */
        #ifndef VOX_LOOP_MAX_CALLBACKS_PER_ITERATION
        #define VOX_LOOP_MAX_CALLBACKS_PER_ITERATION 8192  /* 调大默认值以支持高并发 */
        #endif
        int callback_count = 0;
        while (!vox_queue_empty(loop->pending_callbacks) && 
               callback_count < VOX_LOOP_MAX_CALLBACKS_PER_ITERATION) {
            vox_work_item_t* item = (vox_work_item_t*)vox_queue_dequeue(loop->pending_callbacks);
            if (item) {
                if (item->callback) {
                    item->callback(loop, item->user_data);
                }
                /* 处理完后释放工作项内存 */
                vox_mpool_free(loop->mpool, item);
            }
            callback_count++;
        }
        
        /* 平台特定的 poll 操作 */
        int timeout = calculate_poll_timeout(loop);
        
        /* 根据运行模式调整超时 */
        if (mode == VOX_RUN_NOWAIT) {
            timeout = 0;
        } else if (mode == VOX_RUN_ONCE) {
            /* ONCE 模式：如果没有待处理的回调和定时器，使用非阻塞模式 */
            if (vox_queue_empty(loop->pending_callbacks)) {
                vox_mheap_t* timers = vox_loop_get_timers(loop);
                if (timers == NULL || vox_mheap_empty(timers)) {
                    timeout = 0;  /* 非阻塞，立即返回 */
                }
            }
        }
        
        /* 使用 backend 进行 poll */
        if (loop->backend) {
            vox_backend_poll(loop->backend, timeout, handle_backend_event);
        } else {
            /* 如果没有 backend，短暂休眠避免CPU空转 */
            if (mode != VOX_RUN_NOWAIT && timeout > 0) {
                vox_time_sleep_ms(timeout > 100 ? 100 : timeout);
            } else if (mode == VOX_RUN_NOWAIT) {
                /* NOWAIT 模式不等待 */
            }
        }
        
        /* poll 返回后若已请求停止则立即退出（回调中调用了 vox_loop_stop 时） */
        if (loop->stop_flag) {
            break;
        }
        
        /* 如果是一次性运行，退出 */
        if (mode == VOX_RUN_ONCE || mode == VOX_RUN_NOWAIT) {
            break;
        }
        
        /* 处理关闭的句柄 */
        vox_handle_process_closing(loop);

        /* 如果没有活跃句柄、无待处理回调、无定时器且无引用（如协程在 await），则退出 */
        vox_mheap_t* timers = vox_loop_get_timers(loop);

        if (loop->active_handles_count == 0 && 
            loop->ref_count == 0 &&
            vox_queue_empty(loop->pending_callbacks) &&
            (timers == NULL || vox_mheap_empty(timers))) {
            break;
        }
    }
    
    loop->running = false;
    return 0;
}

/* 计算 poll 超时时间（毫秒） */
static int calculate_poll_timeout(vox_loop_t* loop) {
    if (!loop) {
        return 0;
    }
    
    /* 1. 如果有待执行的回调，立即返回（不等待） */
    if (!vox_queue_empty(loop->pending_callbacks)) {
        return 0;
    }
    
    /* 2. 如果已停止，立即返回 */
    if (loop->stop_flag) {
        return 0;
    }
    
    /* 3. 获取下一个定时器的到期时间 */
    int timer_timeout = vox_timer_get_next_timeout(loop);
    if (timer_timeout < 0) {
        /* 没有定时器，如果没有活跃句柄，应该立即返回 */
        /* 但为了安全，使用一个较短的超时时间（100ms） */
        return 100;
    }
    return timer_timeout;
}

/* 停止事件循环 */
void vox_loop_stop(vox_loop_t* loop) {
    if (loop) {
        loop->stop_flag = true;
        /* 唤醒 backend 以便立即退出阻塞的 poll */
        if (loop->backend) {
            vox_backend_wakeup(loop->backend);
        }
    }
}

/* 检查事件循环是否正在运行 */
bool vox_loop_is_running(const vox_loop_t* loop) {
    return loop && loop->running;
}

/* 检查事件循环是否已停止 */
bool vox_loop_is_stopped(const vox_loop_t* loop) {
    return loop && loop->stop_flag;
}

/* 获取事件循环的活跃句柄数量 */
size_t vox_loop_active_handles(const vox_loop_t* loop) {
    return loop ? loop->active_handles_count : 0;
}

/* 增加事件循环引用（协程 await 时调用） */
void vox_loop_ref(vox_loop_t* loop) {
    if (loop) {
        loop->ref_count++;
    }
}

/* 减少事件循环引用（协程恢复回调中调用） */
void vox_loop_unref(vox_loop_t* loop) {
    if (loop && loop->ref_count > 0) {
        loop->ref_count--;
    }
}

/* 获取事件循环的内存池 */
vox_mpool_t* vox_loop_get_mpool(vox_loop_t* loop) {
    return loop ? loop->mpool : NULL;
}

/* 获取当前循环时间 */
uint64_t vox_loop_now(const vox_loop_t* loop) {
    return loop ? loop->loop_time : 0;
}

/* 更新循环时间 */
void vox_loop_update_time(vox_loop_t* loop) {
    if (loop) {
        loop->loop_time = vox_time_monotonic();
    }
}

/* 销毁事件循环 */
void vox_loop_destroy(vox_loop_t* loop) {
    if (!loop) {
        return;
    }
    
    /* 停止事件循环 */
    if (loop->running) {
        vox_loop_stop(loop);
    }
    
    /* 清理平台抽象层 backend */
    if (loop->backend) {
        vox_backend_destroy(loop->backend);
        loop->backend = NULL;
    }
    
    /* 先销毁 loop 的队列，再销毁线程池，避免不同 mpool 的大块在堆上相邻时
     * free 与仍在使用中的相邻块合并导致堆损坏（glibc free 合并相邻块）。 */
    if (loop->pending_events) {
        vox_queue_destroy(loop->pending_events);
        loop->pending_events = NULL;
    }
    if (loop->pending_callbacks) {
        vox_queue_destroy(loop->pending_callbacks);
        loop->pending_callbacks = NULL;
    }
    
    /* 销毁线程池（使用独立 mpool，其大块释放顺序与 loop 的队列无关） */
    if (loop->thread_pool) {
        vox_tpool_shutdown((vox_tpool_t*)loop->thread_pool);
        vox_tpool_destroy((vox_tpool_t*)loop->thread_pool);
        loop->thread_pool = NULL;
    }
    
    /* 销毁定时器堆 */
    if (loop->timers) {
        vox_mheap_destroy(loop->timers);
    }
    
    /* 释放内存 */
    bool own_mpool = loop->own_mpool;
    vox_mpool_t* mpool = loop->mpool;
    vox_mpool_free(mpool, loop);
    
    /* 如果拥有内存池，销毁它 */
    if (own_mpool) {
        vox_mpool_destroy(mpool);
    }
}

/* 在事件循环的下一次迭代中执行回调 */
int vox_loop_queue_work(vox_loop_t* loop, vox_loop_cb cb, void* user_data) {
    if (!loop || !cb) {
        return -1;
    }
    
    vox_work_item_t* item = (vox_work_item_t*)vox_mpool_alloc(loop->mpool, sizeof(vox_work_item_t));
    if (!item) {
        return -1;
    }
    
    item->callback = cb;
    item->user_data = user_data;
    
    if (vox_queue_enqueue(loop->pending_callbacks, item) != 0) {
        vox_mpool_free(loop->mpool, item);
        return -1;
    }
    
    /* 唤醒 backend 以便立即处理新任务（类似 libuv 的 uv_async_send） */
    if (loop->backend) {
        vox_backend_wakeup(loop->backend);
    }
    
    return 0;
}

/* 立即执行回调 */
int vox_loop_queue_work_immediate(vox_loop_t* loop, vox_loop_cb cb, void* user_data) {
    if (!loop || !cb) {
        return -1;
    }
    
    cb(loop, user_data);
    return 0;
}

/* 获取定时器堆（内部使用） */
vox_mheap_t* vox_loop_get_timers(vox_loop_t* loop) {
    return loop ? loop->timers : NULL;
}

/* 获取活跃句柄列表（内部使用） */
vox_list_t* vox_loop_get_active_handles(vox_loop_t* loop) {
    return loop ? &loop->active_handles : NULL;
}

/* 获取关闭句柄列表（内部使用） */
vox_list_t* vox_loop_get_closing_handles(vox_loop_t* loop) {
    return loop ? &loop->closing_handles : NULL;
}

/* 增加活跃句柄计数（内部使用） */
void vox_loop_increment_active_handles(vox_loop_t* loop) {
    if (loop) {
        loop->active_handles_count++;
    }
}

/* 减少活跃句柄计数（内部使用） */
void vox_loop_decrement_active_handles(vox_loop_t* loop) {
    if (loop && loop->active_handles_count > 0) {
        loop->active_handles_count--;
    }
}

/* 获取 backend（内部使用） */
vox_backend_t* vox_loop_get_backend(vox_loop_t* loop) {
    return loop ? loop->backend : NULL;
}

/* 获取线程池（内部使用） */
vox_tpool_t* vox_loop_get_thread_pool(vox_loop_t* loop) {
    return loop ? (vox_tpool_t*)loop->thread_pool : NULL;
}
