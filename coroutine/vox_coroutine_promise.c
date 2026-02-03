/*
 * vox_coroutine_promise.c - Promise机制实现
 */

#include "vox_coroutine_promise.h"
#include "../vox_loop.h"
#include "../vox_mpool.h"
#include "../vox_log.h"
#include <string.h>

/* 前向声明（避免循环依赖） */
struct vox_coroutine;
extern int vox_coroutine_resume(struct vox_coroutine* co);

/* 恢复协程的工作项 */
typedef struct {
    struct vox_coroutine* co;
} resume_coroutine_work_t;

/* 恢复协程的工作函数 */
static void resume_coroutine_work(vox_loop_t* loop, void* user_data) {
    resume_coroutine_work_t* work = (resume_coroutine_work_t*)user_data;
    if (!work || !work->co) {
        return;
    }
    
    /* 恢复协程 */
    vox_coroutine_resume(work->co);
    
    /* 解除 await 时加的 loop 引用 */
    if (loop) {
        vox_loop_unref(loop);
    }
}

/* 创建Promise */
vox_coroutine_promise_t* vox_coroutine_promise_create(vox_loop_t* loop) {
    if (!loop) {
        VOX_LOG_ERROR("Invalid loop pointer");
        return NULL;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) {
        VOX_LOG_ERROR("Failed to get loop memory pool");
        return NULL;
    }
    
    /* 分配Promise结构 */
    vox_coroutine_promise_t* promise = (vox_coroutine_promise_t*)vox_mpool_alloc(mpool, sizeof(vox_coroutine_promise_t));
    if (!promise) {
        VOX_LOG_ERROR("Failed to allocate promise structure");
        return NULL;
    }
    
    memset(promise, 0, sizeof(vox_coroutine_promise_t));
    
    promise->loop = loop;
    promise->completed = false;
    promise->status = 0;
    promise->result = NULL;
    promise->waiting_coroutine = NULL;
    
    /* 初始化互斥锁 */
    if (vox_mutex_create(&promise->mutex) != 0) {
        VOX_LOG_ERROR("Failed to create mutex");
        vox_mpool_free(mpool, promise);
        return NULL;
    }
    
    /* 创建事件（自动重置，初始未触发） */
    if (vox_event_create(&promise->event, false, false) != 0) {
        VOX_LOG_ERROR("Failed to create event");
        vox_mutex_destroy(&promise->mutex);
        vox_mpool_free(mpool, promise);
        return NULL;
    }
    
    return promise;
}

/* 销毁Promise */
void vox_coroutine_promise_destroy(vox_coroutine_promise_t* promise) {
    if (!promise) {
        return;
    }
    
    vox_loop_t* loop = promise->loop;
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    
    vox_event_destroy(&promise->event);
    vox_mutex_destroy(&promise->mutex);
    vox_mpool_free(mpool, promise);
}

/* 完成Promise */
int vox_coroutine_promise_complete(vox_coroutine_promise_t* promise,
                                   int status,
                                   void* result) {
    if (!promise) {
        return -1;
    }
    
    vox_mutex_lock(&promise->mutex);
    
    /* 检查是否已经完成 */
    if (promise->completed) {
        vox_mutex_unlock(&promise->mutex);
        VOX_LOG_WARN("Promise already completed");
        return -1;
    }
    
    /* 设置结果 */
    promise->completed = true;
    promise->status = status;
    promise->result = result;
    
    /* 获取等待的协程 */
    struct vox_coroutine* waiting_co = (struct vox_coroutine*)promise->waiting_coroutine;
    
    vox_mutex_unlock(&promise->mutex);
    
    /* 触发事件 */
    vox_event_set(&promise->event);
    
    /* 如果有等待的协程，恢复它 */
    if (waiting_co) {
        /* 通过loop队列恢复协程（确保在loop线程执行） */
        vox_mpool_t* mpool = vox_loop_get_mpool(promise->loop);
        resume_coroutine_work_t* work = (resume_coroutine_work_t*)vox_mpool_alloc(
            mpool, sizeof(resume_coroutine_work_t));
        if (work) {
            work->co = waiting_co;
            vox_loop_queue_work(promise->loop, resume_coroutine_work, work);
        } else {
            /* OOM：无法入队恢复，需解除 await 时加的 loop 引用，避免泄漏 */
            vox_loop_unref(promise->loop);
        }
    }
    
    return 0;
}

/* 检查Promise是否已完成 */
bool vox_coroutine_promise_is_completed(const vox_coroutine_promise_t* promise) {
    if (!promise) {
        return false;
    }
    
    vox_mutex_lock((vox_mutex_t*)&promise->mutex);
    bool completed = promise->completed;
    vox_mutex_unlock((vox_mutex_t*)&promise->mutex);
    
    return completed;
}

/* 获取Promise的状态码 */
int vox_coroutine_promise_get_status(const vox_coroutine_promise_t* promise) {
    if (!promise) {
        return -1;
    }
    
    vox_mutex_lock((vox_mutex_t*)&promise->mutex);
    int status = promise->status;
    vox_mutex_unlock((vox_mutex_t*)&promise->mutex);
    
    return status;
}

/* 获取Promise的结果数据 */
void* vox_coroutine_promise_get_result(const vox_coroutine_promise_t* promise) {
    if (!promise) {
        return NULL;
    }
    
    vox_mutex_lock((vox_mutex_t*)&promise->mutex);
    void* result = promise->result;
    vox_mutex_unlock((vox_mutex_t*)&promise->mutex);
    
    return result;
}
