/*
 * vox_coroutine.c - 协程系统实现
 */

#ifdef VOX_OS_MACOS
#define _XOPEN_SOURCE 700
#endif

#include "vox_coroutine.h"

#ifdef VOX_OS_MACOS
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "vox_coroutine_promise.h"
#include "../vox_loop.h"
#include "../vox_mpool.h"
#include "../vox_mutex.h"   
#include "../vox_log.h"
#include "../vox_os.h"
#include <string.h>
#include <stdlib.h>

/* 平台特定的上下文切换实现 */
#ifdef VOX_OS_WINDOWS
    #include <windows.h>
    typedef LPVOID vox_coroutine_context_t;
#else
    /* Unix平台：使用ucontext或汇编实现 */
    #include <ucontext.h>
    typedef ucontext_t vox_coroutine_context_t;
#endif

/* 协程结构 */
struct vox_coroutine {
    /* 句柄基类（必须作为第一个成员） */
    vox_handle_t handle;
    
    /* 协程状态 */
    vox_coroutine_state_t state;
    
    /* 协程栈 */
    void* stack;
    size_t stack_size;
    
    /* 平台特定的上下文 */
    vox_coroutine_context_t* context;
#ifdef VOX_OS_WINDOWS
    LPVOID caller_context;  /* 调用者Fiber（用于恢复） */
#else
    ucontext_t caller_context_storage;  /* 调用者上下文存储 */
    ucontext_t* caller_context;  /* 调用者上下文指针（指向storage或NULL） */
#endif
    
    /* 协程入口函数 */
    vox_coroutine_entry_fn entry;
    void* user_data;
    
    /* 当前等待的Promise */
    vox_coroutine_promise_t* waiting_promise;

    /* 池化支持 */
    bool is_pooled;                    /* 是否来自池 */
    vox_coroutine_pool_t* pool;        /* 所属池 */
    vox_coroutine_slot_t* slot;        /* 池槽 */
};

/* 主线程上下文（仅Windows需要） */
#ifdef VOX_OS_WINDOWS
static LPVOID s_main_fiber = NULL;
#endif

/* 当前运行的协程（线程局部，用于yield） */
#ifdef VOX_OS_WINDOWS
static __declspec(thread) vox_coroutine_t* s_current_coroutine = NULL;
#else
static __thread vox_coroutine_t* s_current_coroutine = NULL;
#endif

/* 协程入口包装函数（平台特定） */
#ifdef VOX_OS_WINDOWS
static void WINAPI coroutine_entry_wrapper(LPVOID lpParameter) {
    vox_coroutine_t* co = (vox_coroutine_t*)lpParameter;
    s_current_coroutine = co;
    co->state = VOX_COROUTINE_RUNNING;
    
    /* 调用用户入口函数 */
    if (co->entry) {
        co->entry(co, co->user_data);
    }
    
    /* 协程完成 */
    co->state = VOX_COROUTINE_COMPLETED;
    s_current_coroutine = NULL;
    
    /* 切换回主上下文 */
    SwitchToFiber(s_main_fiber);
}
#else
static void coroutine_entry_wrapper(uint32_t low32, uint32_t hi32) {
    uintptr_t ptr = (uintptr_t)(uint64_t)low32 | ((uint64_t)hi32 << 32);
    vox_coroutine_t* co = (vox_coroutine_t*)ptr;
    s_current_coroutine = co;
    co->state = VOX_COROUTINE_RUNNING;

    /* 调用用户入口函数 */
    if (co->entry) {
        co->entry(co, co->user_data);
    }

    /* 协程完成 */
    co->state = VOX_COROUTINE_COMPLETED;
    s_current_coroutine = NULL;

    /* 切换回调用者上下文 */
    if (co->caller_context) {
        setcontext(co->caller_context);
    }
    /* 备用：如果没有caller_context，uc_link会处理 */
}
#endif

/* 创建协程 */
vox_coroutine_t* vox_coroutine_create(vox_loop_t* loop,
                                      vox_coroutine_entry_fn entry,
                                      void* user_data,
                                      size_t stack_size) {
    if (!loop || !entry) {
        VOX_LOG_ERROR("Invalid loop or entry function");
        return NULL;
    }
    
    /* 默认栈大小：64KB */
    if (stack_size == 0) {
        stack_size = 64 * 1024;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) {
        VOX_LOG_ERROR("Failed to get loop memory pool");
        return NULL;
    }
    
    /* 分配协程结构 */
    vox_coroutine_t* co = (vox_coroutine_t*)vox_mpool_alloc(mpool, sizeof(vox_coroutine_t));
    if (!co) {
        VOX_LOG_ERROR("Failed to allocate coroutine structure");
        return NULL;
    }
    
    memset(co, 0, sizeof(vox_coroutine_t));
    
    /* 初始化句柄 */
    if (vox_handle_init(&co->handle, VOX_HANDLE_COROUTINE, loop) != 0) {
        vox_mpool_free(mpool, co);
        return NULL;
    }
    
    /* 分配栈 */
    co->stack = vox_mpool_alloc(mpool, stack_size);
    if (!co->stack) {
        VOX_LOG_ERROR("Failed to allocate coroutine stack");
        vox_mpool_free(mpool, co);
        return NULL;
    }
    
    co->stack_size = stack_size;
    co->entry = entry;
    co->user_data = user_data;
    co->state = VOX_COROUTINE_READY;
    co->waiting_promise = NULL;
#ifdef VOX_OS_WINDOWS
    co->caller_context = NULL;
#else
    co->caller_context = NULL;  /* 首次启动时不需要调用者上下文 */
#endif
    
    /* 初始化平台特定的上下文 */
#ifdef VOX_OS_WINDOWS
    /* 初始化主Fiber（如果还没有） */
    if (!s_main_fiber) {
        s_main_fiber = ConvertThreadToFiber(NULL);
        if (!s_main_fiber) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_FIBER) {
                VOX_LOG_ERROR("Failed to convert thread to fiber: %lu", err);
                vox_mpool_free(mpool, co->stack);
                vox_mpool_free(mpool, co);
                return NULL;
            }
            /* 已经是Fiber，获取当前Fiber */
            s_main_fiber = GetCurrentFiber();
        }
    }
    
    /* 创建协程Fiber */
    co->context = CreateFiber(stack_size, coroutine_entry_wrapper, co);
    if (!co->context) {
        VOX_LOG_ERROR("Failed to create fiber");
        vox_mpool_free(mpool, co->stack);
        vox_mpool_free(mpool, co);
        return NULL;
    }
#else
    /* Unix平台：使用ucontext（注意：已废弃但可用） */
    co->context = (ucontext_t*)vox_mpool_alloc(mpool, sizeof(ucontext_t));
    if (!co->context) {
        VOX_LOG_ERROR("Failed to allocate context");
        vox_mpool_free(mpool, co->stack);
        vox_mpool_free(mpool, co);
        return NULL;
    }
    
    if (getcontext(co->context) != 0) {
        VOX_LOG_ERROR("Failed to get context");
        vox_mpool_free(mpool, co->context);
        vox_mpool_free(mpool, co->stack);
        vox_mpool_free(mpool, co);
        return NULL;
    }
    
    /* 设置栈 */
    co->context->uc_stack.ss_sp = co->stack;
    co->context->uc_stack.ss_size = stack_size;
    co->context->uc_link = NULL;  /* 协程完成时通过setcontext显式返回 */
    
    /* 设置入口函数 */
    /* 注意：makecontext的参数传递方式因平台而异，这里使用指针传递 */
    makecontext(co->context, (void(*)())coroutine_entry_wrapper, 2,
                (uint32_t)(uintptr_t)co, (uint32_t)((uintptr_t)co >> 32));
#endif
    
    return co;
}

/* 销毁协程 */
void vox_coroutine_destroy(vox_coroutine_t* co) {
    if (!co) {
        return;
    }

    vox_loop_t* loop = co->handle.loop;
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);

#ifdef VOX_OS_WINDOWS
    if (co->context) {
        DeleteFiber(co->context);
    }
#else
    if (co->context) {
        vox_mpool_free(mpool, co->context);
    }
#endif

    /* 处理池化协程 */
    if (co->is_pooled && co->pool && co->slot) {
        vox_coroutine_pool_release(co->pool, co->slot);
    } else if (co->stack) {
        vox_mpool_free(mpool, co->stack);
    }

    vox_mpool_free(mpool, co);
}

/* 恢复协程执行 */
int vox_coroutine_resume(vox_coroutine_t* co) {
    if (!co) {
        return -1;
    }
    
    if (co->state == VOX_COROUTINE_COMPLETED) {
        VOX_LOG_WARN("Cannot resume completed coroutine");
        return -1;
    }
    
    if (co->state == VOX_COROUTINE_RUNNING) {
        VOX_LOG_WARN("Coroutine is already running");
        return -1;
    }
    
#ifdef VOX_OS_WINDOWS
    /* 保存当前Fiber作为调用者上下文 */
    co->caller_context = GetCurrentFiber();
    
    /* 切换到协程Fiber */
    SwitchToFiber(co->context);
    
    /* 返回时，协程已经yield或完成 */
    return 0;
#else
    /* Unix平台：使用swapcontext进行上下文切换 */
    /* 设置调用者上下文指针 */
    co->caller_context = &co->caller_context_storage;

    if (co->state == VOX_COROUTINE_READY) {
        co->state = VOX_COROUTINE_RUNNING;
    }

    /* 保存当前上下文到caller_context，然后切换到协程上下文 */
    /* 当协程yield或完成时，会通过swapcontext/setcontext返回到这里 */
    swapcontext(&co->caller_context_storage, co->context);

    return 0;
#endif
}

/* 挂起当前协程 */
int vox_coroutine_yield(vox_coroutine_t* co) {
    if (!co || co != s_current_coroutine) {
        VOX_LOG_ERROR("Invalid coroutine or not current coroutine");
        return -1;
    }
    
    co->state = VOX_COROUTINE_SUSPENDED;
    
#ifdef VOX_OS_WINDOWS
    /* 切换回调用者Fiber */
    if (co->caller_context) {
        SwitchToFiber(co->caller_context);
    } else {
        SwitchToFiber(s_main_fiber);
    }
    return 0;
#else
    /* Unix平台：切换回调用者上下文 */
    if (!co->caller_context) {
        VOX_LOG_ERROR("No caller context to return to");
        return -1;
    }
    /* 保存当前上下文到co->context，然后切换回调用者 */
    swapcontext(co->context, co->caller_context);
    return 0;
#endif
}

/* 等待Promise完成 */
int vox_coroutine_await(vox_coroutine_t* co, vox_coroutine_promise_t* promise) {
    if (!co || !promise) {
        return -1;
    }
    
    if (co != s_current_coroutine) {
        VOX_LOG_ERROR("Cannot await from non-current coroutine");
        return -1;
    }
    
    /* 检查Promise是否已完成 */
    if (vox_coroutine_promise_is_completed(promise)) {
        return vox_coroutine_promise_get_status(promise);
    }
    
    /* 设置等待的Promise和协程 */
    co->waiting_promise = promise;
    promise->waiting_coroutine = co;
    co->state = VOX_COROUTINE_SUSPENDED;
    
    /* 引用 loop，防止在异步完成前事件循环退出（避免 wakeup pipe EBADF 竞态） */
    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (loop) {
        vox_loop_ref(loop);
    }
    
    /* 挂起协程 */
    vox_coroutine_yield(co);
    
    /* 恢复后，Promise应该已完成 */
    co->waiting_promise = NULL;
    promise->waiting_coroutine = NULL;
    return vox_coroutine_promise_get_status(promise);
}

/* 获取协程状态 */
vox_coroutine_state_t vox_coroutine_get_state(const vox_coroutine_t* co) {
    return co ? co->state : VOX_COROUTINE_ERROR;
}

/* 获取协程所属的事件循环 */
vox_loop_t* vox_coroutine_get_loop(const vox_coroutine_t* co) {
    return co ? co->handle.loop : NULL;
}

/* 获取协程的用户数据 */
void* vox_coroutine_get_user_data(const vox_coroutine_t* co) {
    return co ? co->user_data : NULL;
}

/* 获取默认配置 */
void vox_coroutine_config_default(vox_coroutine_config_t* config) {
    if (!config) return;
    config->stack_size = 64 * 1024;
    config->use_pool = false;
    config->pool = NULL;
}

/* 使用扩展配置创建协程 */
vox_coroutine_t* vox_coroutine_create_ex(vox_loop_t* loop,
                                          vox_coroutine_entry_fn entry,
                                          void* user_data,
                                          const vox_coroutine_config_t* config) {
    vox_coroutine_config_t default_config;
    if (!config) {
        vox_coroutine_config_default(&default_config);
        config = &default_config;
    }

    if (config->use_pool && config->pool) {
        return vox_coroutine_create_pooled(loop, config->pool, entry, user_data);
    }

    return vox_coroutine_create(loop, entry, user_data, config->stack_size);
}

/* 从协程池创建协程 */
vox_coroutine_t* vox_coroutine_create_pooled(vox_loop_t* loop,
                                              vox_coroutine_pool_t* pool,
                                              vox_coroutine_entry_fn entry,
                                              void* user_data) {
    if (!loop || !pool || !entry) {
        VOX_LOG_ERROR("Invalid parameters for pooled coroutine");
        return NULL;
    }

    /* 从池获取槽 */
    vox_coroutine_slot_t* slot = vox_coroutine_pool_acquire(pool);
    if (!slot) {
        VOX_LOG_ERROR("Failed to acquire slot from pool");
        return NULL;
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) {
        vox_coroutine_pool_release(pool, slot);
        return NULL;
    }

    /* 分配协程结构 */
    vox_coroutine_t* co = (vox_coroutine_t*)vox_mpool_alloc(mpool, sizeof(vox_coroutine_t));
    if (!co) {
        vox_coroutine_pool_release(pool, slot);
        return NULL;
    }

    memset(co, 0, sizeof(vox_coroutine_t));

    /* 初始化句柄 */
    if (vox_handle_init(&co->handle, VOX_HANDLE_COROUTINE, loop) != 0) {
        vox_mpool_free(mpool, co);
        vox_coroutine_pool_release(pool, slot);
        return NULL;
    }

    /* 使用池槽的栈 */
    co->stack = slot->stack;
    co->stack_size = slot->stack_size;
    co->entry = entry;
    co->user_data = user_data;
    co->state = VOX_COROUTINE_READY;
    co->waiting_promise = NULL;
    co->is_pooled = true;
    co->pool = pool;
    co->slot = slot;

#ifdef VOX_OS_WINDOWS
    co->caller_context = NULL;
    /* 初始化主Fiber */
    if (!s_main_fiber) {
        s_main_fiber = ConvertThreadToFiber(NULL);
        if (!s_main_fiber) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_FIBER) {
                vox_mpool_free(mpool, co);
                vox_coroutine_pool_release(pool, slot);
                return NULL;
            }
            s_main_fiber = GetCurrentFiber();
        }
    }
    co->context = CreateFiber(co->stack_size, coroutine_entry_wrapper, co);
    if (!co->context) {
        vox_mpool_free(mpool, co);
        vox_coroutine_pool_release(pool, slot);
        return NULL;
    }
#else
    co->caller_context = NULL;
    co->context = (ucontext_t*)vox_mpool_alloc(mpool, sizeof(ucontext_t));
    if (!co->context) {
        vox_mpool_free(mpool, co);
        vox_coroutine_pool_release(pool, slot);
        return NULL;
    }
    if (getcontext(co->context) != 0) {
        vox_mpool_free(mpool, co->context);
        vox_mpool_free(mpool, co);
        vox_coroutine_pool_release(pool, slot);
        return NULL;
    }
    co->context->uc_stack.ss_sp = co->stack;
    co->context->uc_stack.ss_size = co->stack_size;
    co->context->uc_link = NULL;
    makecontext(co->context, (void(*)())coroutine_entry_wrapper, 2,
                (uint32_t)(uintptr_t)co, (uint32_t)((uintptr_t)co >> 32));
#endif

    return co;
}

/* 检查协程是否来自池 */
bool vox_coroutine_is_pooled(const vox_coroutine_t* co) {
    return co ? co->is_pooled : false;
}

/* 获取协程的栈大小 */
size_t vox_coroutine_get_stack_size(const vox_coroutine_t* co) {
    return co ? co->stack_size : 0;
}

/* 获取当前正在运行的协程 */
vox_coroutine_t* vox_coroutine_current(void) {
    return s_current_coroutine;
}
