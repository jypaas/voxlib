/*
 * vox_coroutine_context_win.c - Windows Fiber 上下文实现
 * 使用 Windows Fiber API 实现协程上下文切换
 */

#include "vox_coroutine_context.h"

#ifdef VOX_OS_WINDOWS

#include "../vox_log.h"
#include <windows.h>

/* 主线程 Fiber (线程局部) */
static __declspec(thread) LPVOID s_main_fiber = NULL;
static __declspec(thread) bool s_fiber_initialized = false;

/* 协程包装数据 */
typedef struct {
    vox_coro_entry_fn entry;
    void* arg;
    vox_coro_ctx_t* ctx;
} fiber_wrapper_data_t;

/* Fiber 入口包装函数 */
static void WINAPI fiber_entry_wrapper(LPVOID lpParameter) {
    fiber_wrapper_data_t* data = (fiber_wrapper_data_t*)lpParameter;
    vox_coro_entry_fn entry = data->entry;
    void* arg = data->arg;

    /* 调用用户入口函数 */
    if (entry) {
        entry(arg);
    }

    /* 协程完成，切换回调用者 */
    if (data->ctx && data->ctx->caller_fiber) {
        SwitchToFiber(data->ctx->caller_fiber);
    } else if (s_main_fiber) {
        SwitchToFiber(s_main_fiber);
    }

    /* 不应该到达这里 */
}

/* 确保当前线程已转换为 Fiber */
static int ensure_fiber_mode(void) {
    if (s_fiber_initialized) {
        return 0;
    }

    /* 检查是否已经是 Fiber */
    if (IsThreadAFiber()) {
        s_main_fiber = GetCurrentFiber();
        s_fiber_initialized = true;
        return 0;
    }

    /* 转换当前线程为 Fiber */
    s_main_fiber = ConvertThreadToFiber(NULL);
    if (!s_main_fiber) {
        DWORD err = GetLastError();
        if (err == ERROR_ALREADY_FIBER) {
            s_main_fiber = GetCurrentFiber();
            s_fiber_initialized = true;
            return 0;
        }
        VOX_LOG_ERROR("ConvertThreadToFiber failed: %lu", err);
        return -1;
    }

    s_fiber_initialized = true;
    return 0;
}

/* 初始化上下文结构 */
void vox_coro_ctx_init(vox_coro_ctx_t* ctx) {
    if (!ctx) return;
    ctx->fiber = NULL;
    ctx->caller_fiber = NULL;
}

/* 创建协程上下文 */
int vox_coro_ctx_make(vox_coro_ctx_t* ctx,
                      void* stack,
                      size_t stack_size,
                      vox_coro_entry_fn entry,
                      void* arg) {
    if (!ctx || !entry) {
        return -1;
    }

    /* 确保 Fiber 模式 */
    if (ensure_fiber_mode() != 0) {
        return -1;
    }

    /* 分配包装数据 (嵌入到栈顶) */
    /* 注意: Windows Fiber 管理自己的栈，我们传入的 stack 参数被忽略 */
    /* 但我们需要存储包装数据，使用 HeapAlloc */
    fiber_wrapper_data_t* wrapper = (fiber_wrapper_data_t*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(fiber_wrapper_data_t));
    if (!wrapper) {
        VOX_LOG_ERROR("Failed to allocate fiber wrapper data");
        return -1;
    }

    wrapper->entry = entry;
    wrapper->arg = arg;
    wrapper->ctx = ctx;

    /* 创建 Fiber */
    /* 注意: Windows Fiber 使用自己的栈，stack_size 参数指定大小 */
    ctx->fiber = CreateFiber(stack_size, fiber_entry_wrapper, wrapper);
    if (!ctx->fiber) {
        VOX_LOG_ERROR("CreateFiber failed: %lu", GetLastError());
        HeapFree(GetProcessHeap(), 0, wrapper);
        return -1;
    }

    ctx->caller_fiber = NULL;

    /* 忽略传入的 stack 参数，Windows Fiber 管理自己的栈 */
    (void)stack;

    return 0;
}

/* 切换上下文 */
void vox_coro_ctx_swap(vox_coro_ctx_t* from, vox_coro_ctx_t* to) {
    if (!from || !to || !to->fiber) {
        return;
    }

    /* 保存当前 Fiber 到 from */
    from->fiber = GetCurrentFiber();

    /* 设置 to 的调用者为当前 Fiber */
    to->caller_fiber = from->fiber;

    /* 切换到目标 Fiber */
    SwitchToFiber(to->fiber);
}

/* 跳转到上下文 (不保存当前上下文) */
void vox_coro_ctx_jump(vox_coro_ctx_t* ctx) {
    if (!ctx || !ctx->fiber) {
        return;
    }
    SwitchToFiber(ctx->fiber);
}

/* 销毁上下文 */
void vox_coro_ctx_destroy(vox_coro_ctx_t* ctx) {
    if (!ctx) return;

    if (ctx->fiber && ctx->fiber != s_main_fiber) {
        /* 获取并释放包装数据 */
        /* 注意: Fiber 的参数在创建时传入，无法直接获取 */
        /* 我们在 DeleteFiber 之前无法安全释放包装数据 */
        /* 这里简单地删除 Fiber，包装数据会泄漏 */
        /* TODO: 使用更好的方式管理包装数据生命周期 */
        DeleteFiber(ctx->fiber);
    }

    ctx->fiber = NULL;
    ctx->caller_fiber = NULL;
}

#endif /* VOX_OS_WINDOWS */
