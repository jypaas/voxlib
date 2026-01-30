/*
 * vox_coroutine_context.h - 协程上下文切换抽象层
 * 提供跨平台的高性能上下文切换接口
 *
 * 平台实现:
 * - Windows: Fiber API (vox_coroutine_context_win.c)
 * - Linux/macOS x86_64: 汇编实现 (vox_coroutine_context_x64.S)
 * - Linux/macOS ARM64: 汇编实现 (vox_coroutine_context_arm64.S)
 * - 其他Unix: ucontext 回退 (vox_coroutine_context_ucontext.c)
 */

#ifndef VOX_COROUTINE_CONTEXT_H
#define VOX_COROUTINE_CONTEXT_H

#include "../vox_os.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 平台检测和上下文类型选择 ===== */

/*
 * 上下文实现类型:
 * VOX_CORO_CTX_FIBER   - Windows Fiber API
 * VOX_CORO_CTX_ASM     - 汇编实现 (x86_64/ARM64)
 * VOX_CORO_CTX_UCONTEXT - ucontext 回退
 */
#ifdef VOX_OS_WINDOWS
    #define VOX_CORO_CTX_FIBER 1
#elif defined(VOX_ARCH_X86_64) || defined(VOX_ARCH_ARM64)
    #define VOX_CORO_CTX_ASM 1
#else
    #define VOX_CORO_CTX_UCONTEXT 1
#endif

/* ===== 上下文结构定义 ===== */

#ifdef VOX_CORO_CTX_FIBER
/* Windows Fiber 上下文 */
typedef struct vox_coro_ctx {
    void* fiber;           /* Fiber 句柄 */
    void* caller_fiber;    /* 调用者 Fiber */
} vox_coro_ctx_t;

#elif defined(VOX_CORO_CTX_ASM)

#ifdef VOX_ARCH_X86_64
/* x86_64 上下文结构 (System V ABI / Windows x64 ABI) */
typedef struct vox_coro_ctx {
    uint64_t rsp;          /* 0x00: 栈指针 */
    uint64_t rbp;          /* 0x08: 帧指针 */
    uint64_t rbx;          /* 0x10: 被调用者保存寄存器 */
    uint64_t r12;          /* 0x18: 被调用者保存寄存器 */
    uint64_t r13;          /* 0x20: 被调用者保存寄存器 */
    uint64_t r14;          /* 0x28: 被调用者保存寄存器 */
    uint64_t r15;          /* 0x30: 被调用者保存寄存器 */
    uint64_t rip;          /* 0x38: 返回地址/入口点 */
#ifdef VOX_OS_WINDOWS
    /* Windows x64 ABI 额外保存 XMM6-XMM15 */
    uint64_t xmm6[2];      /* 0x40 */
    uint64_t xmm7[2];      /* 0x50 */
    uint64_t xmm8[2];      /* 0x60 */
    uint64_t xmm9[2];      /* 0x70 */
    uint64_t xmm10[2];     /* 0x80 */
    uint64_t xmm11[2];     /* 0x90 */
    uint64_t xmm12[2];     /* 0xA0 */
    uint64_t xmm13[2];     /* 0xB0 */
    uint64_t xmm14[2];     /* 0xC0 */
    uint64_t xmm15[2];     /* 0xD0 */
    uint64_t fiber_data;   /* 0xE0: TEB fiber data */
    uint64_t stack_base;   /* 0xE8: TEB stack base */
    uint64_t stack_limit;  /* 0xF0: TEB stack limit */
#endif
} vox_coro_ctx_t;

#elif defined(VOX_ARCH_ARM64)
/* ARM64 上下文结构 (AAPCS64) */
typedef struct vox_coro_ctx {
    uint64_t x19;          /* 0x00: 被调用者保存寄存器 */
    uint64_t x20;          /* 0x08 */
    uint64_t x21;          /* 0x10 */
    uint64_t x22;          /* 0x18 */
    uint64_t x23;          /* 0x20 */
    uint64_t x24;          /* 0x28 */
    uint64_t x25;          /* 0x30 */
    uint64_t x26;          /* 0x38 */
    uint64_t x27;          /* 0x40 */
    uint64_t x28;          /* 0x48 */
    uint64_t x29;          /* 0x50: 帧指针 (fp) */
    uint64_t x30;          /* 0x58: 链接寄存器 (lr) */
    uint64_t sp;           /* 0x60: 栈指针 */
    uint64_t d8;           /* 0x68: 浮点被调用者保存 */
    uint64_t d9;           /* 0x70 */
    uint64_t d10;          /* 0x78 */
    uint64_t d11;          /* 0x80 */
    uint64_t d12;          /* 0x88 */
    uint64_t d13;          /* 0x90 */
    uint64_t d14;          /* 0x98 */
    uint64_t d15;          /* 0xA0 */
} vox_coro_ctx_t;
#endif /* VOX_ARCH_* */

#else /* VOX_CORO_CTX_UCONTEXT */
/* ucontext 回退实现 */
#include <ucontext.h>
typedef struct vox_coro_ctx {
    ucontext_t uc;
    ucontext_t* caller_uc;
} vox_coro_ctx_t;
#endif

/* ===== 协程入口函数类型 ===== */

/* 协程入口函数 (由 vox_coro_ctx_make 设置) */
typedef void (*vox_coro_entry_fn)(void* arg);

/* ===== 上下文操作 API ===== */

/**
 * 初始化上下文结构
 * @param ctx 上下文指针
 */
void vox_coro_ctx_init(vox_coro_ctx_t* ctx);

/**
 * 创建协程上下文
 * @param ctx 上下文指针
 * @param stack 栈内存指针
 * @param stack_size 栈大小
 * @param entry 入口函数
 * @param arg 入口函数参数
 * @return 成功返回0，失败返回-1
 */
int vox_coro_ctx_make(vox_coro_ctx_t* ctx,
                      void* stack,
                      size_t stack_size,
                      vox_coro_entry_fn entry,
                      void* arg);

/**
 * 切换上下文
 * 保存当前上下文到 from，切换到 to
 * @param from 保存当前上下文的指针
 * @param to 要切换到的上下文指针
 */
void vox_coro_ctx_swap(vox_coro_ctx_t* from, vox_coro_ctx_t* to);

/**
 * 跳转到上下文 (不保存当前上下文)
 * @param ctx 要跳转到的上下文指针
 */
void vox_coro_ctx_jump(vox_coro_ctx_t* ctx);

/**
 * 销毁上下文 (释放平台特定资源)
 * @param ctx 上下文指针
 */
void vox_coro_ctx_destroy(vox_coro_ctx_t* ctx);

/* ===== 汇编函数声明 (仅 ASM 实现) ===== */

#ifdef VOX_CORO_CTX_ASM
/**
 * 汇编实现的上下文切换
 * @param from 保存当前上下文的指针
 * @param to 要切换到的上下文指针
 */
extern void vox_coro_ctx_swap_asm(vox_coro_ctx_t* from, vox_coro_ctx_t* to);

/**
 * 汇编实现的上下文跳转
 * @param ctx 要跳转到的上下文指针
 */
extern void vox_coro_ctx_jump_asm(vox_coro_ctx_t* ctx);
#endif

/* ===== 辅助宏 ===== */

/* 栈对齐 (16字节对齐，满足 ABI 要求) */
#define VOX_CORO_STACK_ALIGN 16
#define VOX_CORO_STACK_ALIGN_DOWN(ptr) \
    ((void*)(((uintptr_t)(ptr)) & ~(VOX_CORO_STACK_ALIGN - 1)))

/* 默认栈大小 */
#define VOX_CORO_DEFAULT_STACK_SIZE (64 * 1024)  /* 64KB */
#define VOX_CORO_MIN_STACK_SIZE     (4 * 1024)   /* 4KB */
#define VOX_CORO_MAX_STACK_SIZE     (8 * 1024 * 1024)  /* 8MB */

#ifdef __cplusplus
}
#endif

#endif /* VOX_COROUTINE_CONTEXT_H */
