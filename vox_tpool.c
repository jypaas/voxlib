/*
 * vox_tpool.c - 高性能线程池实现
 * 基于 vox_mpool 和 vox_queue 实现
 */

#include "vox_tpool.h"
#include "vox_queue.h"
#include "vox_thread.h"
#include "vox_mutex.h"
#include "vox_atomic.h"
#include "vox_os.h"
#include <string.h>
#include <stdint.h>

#ifndef VOX_OS_WINDOWS
    #include <unistd.h>
#endif

extern uint32_t vox_get_cpu_count(void);

/* 默认配置 */
#define VOX_TPOOL_DEFAULT_THREAD_COUNT 0  /* 0表示使用CPU核心数 */
#define VOX_TPOOL_DEFAULT_QUEUE_CAPACITY 1024

/* 线程池状态 */
typedef enum {
    VOX_TPOOL_STATE_RUNNING = 0,  /* 运行中 */
    VOX_TPOOL_STATE_SHUTTING_DOWN, /* 正在关闭 */
    VOX_TPOOL_STATE_SHUTDOWN       /* 已关闭 */
} vox_tpool_state_t;

/* 任务结构 */
typedef struct {
    vox_tpool_task_func_t task_func;      /* 任务函数 */
    void* user_data;                       /* 用户数据 */
    vox_tpool_complete_func_t complete_func; /* 完成回调 */
} vox_tpool_task_t;

/* 线程池结构 */
struct vox_tpool {
    vox_mpool_t* mpool;                   /* 内存池 */
    vox_queue_t* task_queue;              /* 任务队列 */
    vox_queue_type_t queue_type;          /* 队列类型 */
    vox_thread_t** threads;               /* 工作线程数组 */
    size_t thread_count;                  /* 线程数量 */
    bool use_queue_mutex;                 /* 是否使用mutex保护队列（NORMAL类型且多线程时） */
    bool mutex_initialized;                /* mutex是否已初始化 */
    bool semaphore_initialized;            /* semaphore是否已初始化 */
    
    /* 同步原语 */
    vox_mutex_t mutex;                    /* 互斥锁（用于保护NORMAL类型队列） */
    vox_semaphore_t semaphore;            /* 信号量（用于唤醒工作线程） */
    
    /* 状态跟踪（使用原子变量） */
    vox_atomic_int_t* state;              /* 线程池状态 */
    vox_atomic_int_t* running_tasks;      /* 正在执行的任务数 */
    vox_atomic_int_t* total_tasks;        /* 总任务数 */
    vox_atomic_int_t* completed_tasks;    /* 已完成任务数 */
    vox_atomic_int_t* failed_tasks;       /* 失败任务数 */
};

/* 清理线程池资源（不释放线程池结构和内存池） */
static void cleanup_tpool_resources(vox_tpool_t* tpool) {
    if (!tpool) return;
    
    /* 释放线程数组 */
    if (tpool->threads) {
        vox_mpool_free(tpool->mpool, tpool->threads);
        tpool->threads = NULL;
    }
    
    /* 销毁原子变量 */
    if (tpool->state) {
        vox_atomic_int_destroy(tpool->state);
        tpool->state = NULL;
    }
    if (tpool->running_tasks) {
        vox_atomic_int_destroy(tpool->running_tasks);
        tpool->running_tasks = NULL;
    }
    if (tpool->total_tasks) {
        vox_atomic_int_destroy(tpool->total_tasks);
        tpool->total_tasks = NULL;
    }
    if (tpool->completed_tasks) {
        vox_atomic_int_destroy(tpool->completed_tasks);
        tpool->completed_tasks = NULL;
    }
    if (tpool->failed_tasks) {
        vox_atomic_int_destroy(tpool->failed_tasks);
        tpool->failed_tasks = NULL;
    }
    
    /* 销毁同步原语 */
    if (tpool->semaphore_initialized) {
        vox_semaphore_destroy(&tpool->semaphore);
        tpool->semaphore_initialized = false;
    }
    if (tpool->use_queue_mutex && tpool->mutex_initialized) {
        vox_mutex_destroy(&tpool->mutex);
        tpool->mutex_initialized = false;
    }
    
    /* 销毁队列 */
    if (tpool->task_queue) {
        vox_queue_destroy(tpool->task_queue);
        tpool->task_queue = NULL;
    }
}

/* 工作线程函数 */
static int worker_thread_func(void* user_data) {
    vox_tpool_t* tpool = (vox_tpool_t*)user_data;
    if (!tpool) return -1;
    
    while (true) {
        /* 等待信号量（有新任务时会被唤醒） */
        vox_semaphore_wait(&tpool->semaphore);
        
        /* 检查线程池状态 */
        int32_t state = vox_atomic_int_load(tpool->state);
        if (state == VOX_TPOOL_STATE_SHUTDOWN) {
            /* 线程池已关闭，退出 */
            break;
        }
        
        /* 从队列中取出任务 */
        vox_tpool_task_t* task = NULL;
        if (tpool->use_queue_mutex && tpool->mutex_initialized) {
            /* 使用NORMAL类型且多线程，需要加锁 */
            vox_mutex_lock(&tpool->mutex);
            task = (vox_tpool_task_t*)vox_queue_dequeue(tpool->task_queue);
            vox_mutex_unlock(&tpool->mutex);
        } else {
            /* MPSC类型或无锁，直接出队 */
            task = (vox_tpool_task_t*)vox_queue_dequeue(tpool->task_queue);
        }
        
        if (!task) {
            /* 队列为空，检查是否正在关闭 */
            state = vox_atomic_int_load(tpool->state);
            if (state == VOX_TPOOL_STATE_SHUTTING_DOWN || state == VOX_TPOOL_STATE_SHUTDOWN) {
                /* 再次检查队列是否为空且没有正在执行的任务 */
                bool queue_empty;
                if (tpool->use_queue_mutex && tpool->mutex_initialized) {
                    vox_mutex_lock(&tpool->mutex);
                    queue_empty = vox_queue_empty(tpool->task_queue);
                    vox_mutex_unlock(&tpool->mutex);
                } else {
                    queue_empty = vox_queue_empty(tpool->task_queue);
                }
                
                if (queue_empty && vox_atomic_int_load(tpool->running_tasks) == 0) {
                    /* 队列为空且没有正在执行的任务，退出 */
                    break;
                }
            }
            /* 队列为空但可能还有其他线程正在处理任务，继续等待 */
            continue;
        }
        
        /* 更新正在执行的任务数 */
        vox_atomic_int_increment(tpool->running_tasks);
        
        /* 执行任务 */
        int result = 0;
        if (task->task_func) {
            /* 执行任务函数 */
            task->task_func(task->user_data);
        } else {
            result = -1;  /* 任务函数为空 */
        }
        
        /* 调用完成回调 */
        if (task->complete_func) {
            task->complete_func(task->user_data, result);
        }
        
        /* 更新统计信息 */
        if (result == 0) {
            vox_atomic_int_increment(tpool->completed_tasks);
        } else {
            vox_atomic_int_increment(tpool->failed_tasks);
        }
        
        /* 更新正在执行的任务数 */
        vox_atomic_int_decrement(tpool->running_tasks);
        
        /* 释放任务结构（使用内存池） */
        vox_mpool_free(tpool->mpool, task);
    }
    
    return 0;
}

/* 创建线程池 */
vox_tpool_t* vox_tpool_create(void) {
    return vox_tpool_create_with_config(NULL);
}

/* 使用配置创建线程池 */
vox_tpool_t* vox_tpool_create_with_config(const vox_tpool_config_t* config) {
    /* 解析配置 */
    size_t thread_count = VOX_TPOOL_DEFAULT_THREAD_COUNT;
    size_t queue_capacity = VOX_TPOOL_DEFAULT_QUEUE_CAPACITY;
    vox_queue_type_t queue_type = VOX_QUEUE_TYPE_MPSC;  /* 默认使用MPSC */
    int thread_safe = 0;  /* 默认非线程安全 */
    
    if (config) {
        if (config->thread_count > 0) {
            thread_count = config->thread_count;
        }
        if (config->queue_capacity > 0) {
            queue_capacity = config->queue_capacity;
        }
        /* 只有当用户显式设置了 queue_type 时才使用它 */
        /* 注意：VOX_QUEUE_TYPE_NORMAL = 0，所以不能简单地检查是否为0 */
        /* 由于无法区分"未设置"和"设置为NORMAL"，我们采用保守策略： */
        /* 只有当 queue_type 是 MPSC 时才使用配置值，否则使用默认值 MPSC */
        /* 如果用户想使用 NORMAL，必须显式设置（虽然值也是0，但至少是明确的） */
        /* 但是，为了支持用户显式设置 NORMAL，我们需要一个更好的方法 */
        /* 暂时：只有当 queue_type 是 MPSC 时才使用，其他情况（包括未初始化的0）都使用默认值 MPSC */
        if (config->queue_type == VOX_QUEUE_TYPE_MPSC) {
            queue_type = VOX_QUEUE_TYPE_MPSC;
        } else {
            /* 对于 NORMAL 或其他值，都使用默认值 MPSC（更安全） */
            /* 如果用户真的想使用 NORMAL，需要在配置中明确设置，但目前我们无法区分 */
            queue_type = VOX_QUEUE_TYPE_MPSC;
        }
    }
    
    /* 如果线程数为0，使用CPU核心数 */
    if (thread_count == 0) {
        thread_count = (size_t)vox_get_cpu_count();
        thread_safe = 1;
    } else if (thread_count > 1) {
        thread_safe = 1;  /* 多线程需要线程安全 */
    }
    
    /* 创建内部内存池 */
    vox_mpool_config_t mpool_config = {0};
    mpool_config.thread_safe = thread_safe;
    vox_mpool_t* mpool = vox_mpool_create_with_config(&mpool_config);
    if (!mpool) return NULL;
    
    /* 分配线程池结构（从内存池分配） */
    vox_tpool_t* tpool = (vox_tpool_t*)vox_mpool_alloc(mpool, sizeof(vox_tpool_t));
    if (!tpool) {
        vox_mpool_destroy(mpool);
        return NULL;
    }
    
    memset(tpool, 0, sizeof(vox_tpool_t));
    tpool->mpool = mpool;
    tpool->thread_count = thread_count;
    tpool->queue_type = queue_type;
    tpool->mutex_initialized = false;
    tpool->semaphore_initialized = false;
    
    /* 判断是否需要使用mutex保护队列 */
    /* 如果使用NORMAL类型且是多线程，需要mutex保护 */
    tpool->use_queue_mutex = (queue_type == VOX_QUEUE_TYPE_NORMAL && thread_count > 1);
    
    /* 创建任务队列 */
    vox_queue_config_t queue_config = {0};
    queue_config.type = queue_type;
    queue_config.initial_capacity = queue_capacity;
    queue_config.elem_free = NULL;  /* 任务由我们手动释放 */
    
    tpool->task_queue = vox_queue_create_with_config(mpool, &queue_config);
    if (!tpool->task_queue) {
        vox_mpool_free(mpool, tpool);
        vox_mpool_destroy(mpool);
        return NULL;
    }
    
    /* 初始化互斥锁（如果使用NORMAL类型且多线程，需要mutex） */
    if (tpool->use_queue_mutex) {
        int mutex_result = vox_mutex_create(&tpool->mutex);
        if (mutex_result != 0) {
            /* mutex 创建失败，清理资源并返回错误 */
            cleanup_tpool_resources(tpool);
            vox_mpool_free(mpool, tpool);
            vox_mpool_destroy(mpool);
            return NULL;
        }
        /* 只有成功创建后才设置标志 */
        tpool->mutex_initialized = true;
    } else {
        /* 确保 mutex_initialized 为 false（虽然已经初始化了，但明确设置更安全） */
        tpool->mutex_initialized = false;
    }
    
    /* 创建信号量（初始值为0，工作线程等待信号） */
    if (vox_semaphore_create(&tpool->semaphore, 0) != 0) {
        cleanup_tpool_resources(tpool);
        vox_mpool_free(mpool, tpool);
        vox_mpool_destroy(mpool);
        return NULL;
    }
    tpool->semaphore_initialized = true;
    
    /* 创建原子变量 */
    tpool->state = vox_atomic_int_create(mpool, VOX_TPOOL_STATE_RUNNING);
    tpool->running_tasks = vox_atomic_int_create(mpool, 0);
    tpool->total_tasks = vox_atomic_int_create(mpool, 0);
    tpool->completed_tasks = vox_atomic_int_create(mpool, 0);
    tpool->failed_tasks = vox_atomic_int_create(mpool, 0);
    
    if (!tpool->state || !tpool->running_tasks || !tpool->total_tasks || 
        !tpool->completed_tasks || !tpool->failed_tasks) {
        cleanup_tpool_resources(tpool);
        vox_mpool_free(mpool, tpool);
        vox_mpool_destroy(mpool);
        return NULL;
    }
    
    /* 分配线程数组 */
    tpool->threads = (vox_thread_t**)vox_mpool_alloc(mpool, thread_count * sizeof(vox_thread_t*));
    if (!tpool->threads) {
        cleanup_tpool_resources(tpool);
        vox_mpool_free(mpool, tpool);
        vox_mpool_destroy(mpool);
        return NULL;
    }
    
    memset(tpool->threads, 0, thread_count * sizeof(vox_thread_t*));
    
    /* 创建工作线程 */
    for (size_t i = 0; i < thread_count; i++) {
        tpool->threads[i] = vox_thread_create(mpool, worker_thread_func, tpool);
        if (!tpool->threads[i]) {
            /* 创建失败，清理已创建的线程 */
            vox_atomic_int_store(tpool->state, VOX_TPOOL_STATE_SHUTDOWN);
            
            /* 唤醒所有等待的线程 */
            for (size_t j = 0; j < i + 1; j++) {
                vox_semaphore_post(&tpool->semaphore);
            }
            
            /* 等待已创建的线程退出 */
            for (size_t j = 0; j < i; j++) {
                if (tpool->threads[j]) {
                    vox_thread_join(tpool->threads[j], NULL);
                }
            }
            
            /* 清理资源 */
            cleanup_tpool_resources(tpool);
            vox_mpool_free(mpool, tpool);
            vox_mpool_destroy(mpool);
            return NULL;
        }
    }
    
    return tpool;
}

/* 提交任务 */
int vox_tpool_submit(vox_tpool_t* tpool, vox_tpool_task_func_t task_func, 
                     void* user_data, vox_tpool_complete_func_t complete_func) {
    if (!tpool || !task_func) return -1;
    
    /* 检查线程池状态 */
    int32_t state = vox_atomic_int_load(tpool->state);
    if (state != VOX_TPOOL_STATE_RUNNING) {
        return -1;  /* 线程池已关闭，不接受新任务 */
    }
    
    /* 分配任务结构 */
    vox_tpool_task_t* task = (vox_tpool_task_t*)vox_mpool_alloc(tpool->mpool, sizeof(vox_tpool_task_t));
    if (!task) return -1;
    
    task->task_func = task_func;
    task->user_data = user_data;
    task->complete_func = complete_func;
    
    /* 入队 */
    int enqueue_result;
    if (tpool->use_queue_mutex && tpool->mutex_initialized) {
        /* 使用NORMAL类型且多线程，需要加锁 */
        vox_mutex_lock(&tpool->mutex);
        enqueue_result = vox_queue_enqueue(tpool->task_queue, task);
        vox_mutex_unlock(&tpool->mutex);
    } else {
        /* MPSC类型或无锁，直接入队 */
        enqueue_result = vox_queue_enqueue(tpool->task_queue, task);
    }
    
    if (enqueue_result != 0) {
        /* 队列已满 */
        vox_mpool_free(tpool->mpool, task);
        return -1;
    }
    
    /* 更新总任务数 */
    vox_atomic_int_increment(tpool->total_tasks);
    
    /* 唤醒一个工作线程 */
    vox_semaphore_post(&tpool->semaphore);
    
    return 0;
}

/* 等待所有任务完成 */
int vox_tpool_wait(vox_tpool_t* tpool) {
    if (!tpool) return -1;
    
    /* 等待队列为空且没有正在执行的任务 */
    /* 需要多次检查以确保任务真正完成（避免竞态条件） */
    int stable_count = 0;
    const int required_stable = 3;  /* 需要连续3次检查都满足条件 */
    
    while (true) {
        size_t pending;
        if (tpool->use_queue_mutex && tpool->mutex_initialized) {
            vox_mutex_lock(&tpool->mutex);
            pending = vox_queue_size(tpool->task_queue);
            vox_mutex_unlock(&tpool->mutex);
        } else {
            pending = vox_queue_size(tpool->task_queue);
        }
        
        int32_t running = vox_atomic_int_load(tpool->running_tasks);
        int32_t total = vox_atomic_int_load(tpool->total_tasks);
        int32_t completed = vox_atomic_int_load(tpool->completed_tasks);
        int32_t failed = vox_atomic_int_load(tpool->failed_tasks);
        
        /* 检查所有任务是否完成 */
        /* 如果没有任务，直接返回 */
        if (total == 0) {
            if (pending == 0 && running == 0) {
                break;
            }
        } else {
            /* 有任务时，需要确保所有任务都完成 */
            if (pending == 0 && running == 0 && (completed + failed) == total) {
                stable_count++;
                if (stable_count >= required_stable) {
                    break;  /* 所有任务已完成 */
                }
            } else {
                stable_count = 0;  /* 重置稳定计数 */
            }
        }
        
        /* 让出CPU时间片 */
        vox_thread_yield();
    }
    
    return 0;
}

/* 关闭线程池 */
int vox_tpool_shutdown(vox_tpool_t* tpool) {
    if (!tpool) return -1;
    
    /* 设置状态为正在关闭 */
    int32_t old_state = vox_atomic_int_exchange(tpool->state, VOX_TPOOL_STATE_SHUTTING_DOWN);
    if (old_state == VOX_TPOOL_STATE_SHUTDOWN) {
        return 0;  /* 已经关闭 */
    }
    
    /* 等待所有任务完成 */
    vox_tpool_wait(tpool);
    
    /* 设置状态为已关闭 */
    vox_atomic_int_store(tpool->state, VOX_TPOOL_STATE_SHUTDOWN);
    
    /* 唤醒所有工作线程（让它们退出） */
    /* 需要唤醒所有线程，因为可能有多个线程在等待信号量 */
    for (size_t i = 0; i < tpool->thread_count; i++) {
        vox_semaphore_post(&tpool->semaphore);
    }
    
    /* 等待所有线程退出 */
    /* 重要：必须等待所有线程退出后才能销毁信号量 */
    for (size_t i = 0; i < tpool->thread_count; i++) {
        if (tpool->threads[i]) {
            vox_thread_join(tpool->threads[i], NULL);
            tpool->threads[i] = NULL;  /* 标记为已处理 */
        }
    }
    
    return 0;
}

/* 强制关闭线程池 */
void vox_tpool_force_shutdown(vox_tpool_t* tpool) {
    if (!tpool) return;
    
    /* 直接设置状态为已关闭 */
    vox_atomic_int_store(tpool->state, VOX_TPOOL_STATE_SHUTDOWN);
    
    /* 唤醒所有工作线程 */
    /* 需要唤醒所有线程，因为可能有多个线程在等待信号量 */
    for (size_t i = 0; i < tpool->thread_count; i++) {
        vox_semaphore_post(&tpool->semaphore);
    }
    
    /* 等待所有线程退出 */
    /* 重要：必须等待所有线程退出后才能销毁信号量 */
    for (size_t i = 0; i < tpool->thread_count; i++) {
        if (tpool->threads[i]) {
            vox_thread_join(tpool->threads[i], NULL);
            tpool->threads[i] = NULL;  /* 标记为已处理 */
        }
    }
}

/* 获取待处理任务数 */
size_t vox_tpool_pending_tasks(const vox_tpool_t* tpool) {
    if (!tpool) return 0;
    
    /* 注意：这里使用const_cast是因为我们需要修改mutex，但函数是const的 */
    /* 实际上mutex的lock/unlock不会改变tpool的逻辑状态 */
    vox_tpool_t* non_const_tpool = (vox_tpool_t*)tpool;
    
    if (non_const_tpool->use_queue_mutex && non_const_tpool->mutex_initialized) {
        vox_mutex_lock(&non_const_tpool->mutex);
        size_t size = vox_queue_size(non_const_tpool->task_queue);
        vox_mutex_unlock(&non_const_tpool->mutex);
        return size;
    } else {
        return vox_queue_size(non_const_tpool->task_queue);
    }
}

/* 获取正在执行的任务数 */
size_t vox_tpool_running_tasks(const vox_tpool_t* tpool) {
    if (!tpool) return 0;
    return (size_t)vox_atomic_int_load(tpool->running_tasks);
}

/* 获取统计信息 */
void vox_tpool_stats(const vox_tpool_t* tpool, 
                     size_t* total_tasks, 
                     size_t* completed_tasks, 
                     size_t* failed_tasks) {
    if (!tpool) return;
    
    if (total_tasks) {
        *total_tasks = (size_t)vox_atomic_int_load(tpool->total_tasks);
    }
    if (completed_tasks) {
        *completed_tasks = (size_t)vox_atomic_int_load(tpool->completed_tasks);
    }
    if (failed_tasks) {
        *failed_tasks = (size_t)vox_atomic_int_load(tpool->failed_tasks);
    }
}

/* 销毁线程池 */
void vox_tpool_destroy(vox_tpool_t* tpool) {
    if (!tpool) return;
    
    /* 如果还在运行，先关闭 */
    int32_t state = vox_atomic_int_load(tpool->state);
    if (state != VOX_TPOOL_STATE_SHUTDOWN) {
        vox_tpool_shutdown(tpool);
    }
    
    /* 清空任务队列（释放剩余任务） */
    while (true) {
        bool queue_empty;
        if (tpool->use_queue_mutex && tpool->mutex_initialized) {
            vox_mutex_lock(&tpool->mutex);
            queue_empty = vox_queue_empty(tpool->task_queue);
            vox_mutex_unlock(&tpool->mutex);
        } else {
            queue_empty = vox_queue_empty(tpool->task_queue);
        }
        
        if (queue_empty) break;
        
        vox_tpool_task_t* task = NULL;
        if (tpool->use_queue_mutex && tpool->mutex_initialized) {
            vox_mutex_lock(&tpool->mutex);
            task = (vox_tpool_task_t*)vox_queue_dequeue(tpool->task_queue);
            vox_mutex_unlock(&tpool->mutex);
        } else {
            task = (vox_tpool_task_t*)vox_queue_dequeue(tpool->task_queue);
        }
        
        if (task) {
            vox_mpool_free(tpool->mpool, task);
        }
    }
    
    /* 保存内存池指针（用于销毁） */
    vox_mpool_t* mpool = tpool->mpool;
    
    /* 清理所有资源 */
    cleanup_tpool_resources(tpool);
    
    /* 释放线程池结构（从内存池分配） */
    vox_mpool_free(mpool, tpool);
    
    /* 销毁内部内存池 */
    vox_mpool_destroy(mpool);
}
