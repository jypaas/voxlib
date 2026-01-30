/*
 * vox_coroutine_context_asm.c - 汇编上下文切换 C 包装
 * 为 x86_64 和 ARM64 汇编实现提供 C 接口
 */

#include "vox_coroutine_context.h"

#ifdef VOX_CORO_CTX_ASM

#include "../vox_log.h"
#include <string.h>

/* 协程入口包装数据 */
typedef struct {
    vox_coro_entry_fn entry;
    void* arg;
} coro_wrapper_data_t;

/* 前向声明跳板函数 */
#ifdef VOX_ARCH_X86_64
void coro_trampoline_x64(void);
#endif
#ifdef VOX_ARCH_ARM64
void coro_trampoline_arm64(void);
#endif

/* 协程入口包装函数 (从汇编调用) */
/* 注意：此函数当前未使用，保留用于将来可能的汇编实现 */
VOX_UNUSED_FUNC static void coro_entry_wrapper(void) {
    /* 从栈上获取包装数据 */
    /* 注意: 这个函数由汇编代码调用，参数通过特定方式传递 */
    /* 实际实现中，我们将包装数据存储在栈顶 */
}

/* 初始化上下文结构 */
void vox_coro_ctx_init(vox_coro_ctx_t* ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(vox_coro_ctx_t));
}

/* 创建协程上下文 */
int vox_coro_ctx_make(vox_coro_ctx_t* ctx,
                      void* stack,
                      size_t stack_size,
                      vox_coro_entry_fn entry,
                      void* arg) {
    if (!ctx || !stack || stack_size < VOX_CORO_MIN_STACK_SIZE || !entry) {
        return -1;
    }

    memset(ctx, 0, sizeof(vox_coro_ctx_t));

    /* 计算栈顶 (栈向下增长) */
    uintptr_t stack_top = (uintptr_t)stack + stack_size;

    /* 16字节对齐 */
    stack_top &= ~(uintptr_t)0xF;

    /* 为包装数据预留空间 */
    stack_top -= sizeof(coro_wrapper_data_t);
    coro_wrapper_data_t* wrapper = (coro_wrapper_data_t*)stack_top;
    wrapper->entry = entry;
    wrapper->arg = arg;

    /* 再次对齐 */
    stack_top &= ~(uintptr_t)0xF;

    /* 预留返回地址空间 (模拟 call 指令) */
    stack_top -= sizeof(void*);

#ifdef VOX_ARCH_X86_64
    /* x86_64: 设置初始上下文 */
    /* 栈顶放置入口包装函数地址 */
    /* 注意：函数指针到对象指针的转换在 C 标准中未定义，但在此实现中是必要的 */
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
    *(void**)stack_top = (void*)(uintptr_t)coro_trampoline_x64;
    #pragma GCC diagnostic pop

    ctx->rsp = stack_top;
    ctx->rbp = 0;
    ctx->rbx = (uint64_t)wrapper;  /* 通过 rbx 传递包装数据 */
    ctx->r12 = 0;
    ctx->r13 = 0;
    ctx->r14 = 0;
    ctx->r15 = 0;
    /* 注意：函数指针到整数类型的转换在 C 标准中未定义，但在此实现中是必要的 */
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
    ctx->rip = (uint64_t)(uintptr_t)coro_trampoline_x64;
    #pragma GCC diagnostic pop

#elif defined(VOX_ARCH_ARM64)
    /* ARM64: 设置初始上下文 */
    ctx->sp = stack_top;
    ctx->x29 = 0;  /* fp */
    ctx->x30 = (uint64_t)coro_trampoline_arm64;  /* lr */
    ctx->x19 = (uint64_t)wrapper;  /* 通过 x19 传递包装数据 */
#endif

    return 0;
}

/* 切换上下文 - 调用汇编实现 */
void vox_coro_ctx_swap(vox_coro_ctx_t* from, vox_coro_ctx_t* to) {
    if (!from || !to) return;
    vox_coro_ctx_swap_asm(from, to);
}

/* 跳转到上下文 - 调用汇编实现 */
void vox_coro_ctx_jump(vox_coro_ctx_t* ctx) {
    if (!ctx) return;
    vox_coro_ctx_jump_asm(ctx);
}

/* 销毁上下文 */
void vox_coro_ctx_destroy(vox_coro_ctx_t* ctx) {
    if (!ctx) return;
    /* 汇编实现不需要特殊清理 */
    memset(ctx, 0, sizeof(vox_coro_ctx_t));
}

/* ===== 跳板函数 (从汇编调用) ===== */

#ifdef VOX_ARCH_X86_64
/* x86_64 跳板函数 */
void coro_trampoline_x64(void) {
    /* rbx 包含包装数据指针 */
    coro_wrapper_data_t* wrapper;
    __asm__ volatile("movq %%rbx, %0" : "=r"(wrapper));

    if (wrapper && wrapper->entry) {
        wrapper->entry(wrapper->arg);
    }

    /* 协程完成，不应返回 */
    /* 调用者应该在入口函数中处理返回 */
    for (;;) {
        /* 死循环，防止返回到无效地址 */
    }
}
#endif

#ifdef VOX_ARCH_ARM64
/* ARM64 跳板函数 */
void coro_trampoline_arm64(void) {
    /* x19 包含包装数据指针 */
    coro_wrapper_data_t* wrapper;
    __asm__ volatile("mov %0, x19" : "=r"(wrapper));

    if (wrapper && wrapper->entry) {
        wrapper->entry(wrapper->arg);
    }

    /* 协程完成，不应返回 */
    for (;;) {
        /* 死循环 */
    }
}
#endif

#endif /* VOX_CORO_CTX_ASM */
