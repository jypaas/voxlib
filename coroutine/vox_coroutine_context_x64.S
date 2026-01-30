/*
 * vox_coroutine_context_x64.S - x86_64 汇编上下文切换
 * System V AMD64 ABI (Linux/macOS/BSD)
 *
 * 上下文结构布局 (vox_coro_ctx_t):
 *   0x00: rsp  - 栈指针
 *   0x08: rbp  - 帧指针
 *   0x10: rbx  - 被调用者保存
 *   0x18: r12  - 被调用者保存
 *   0x20: r13  - 被调用者保存
 *   0x28: r14  - 被调用者保存
 *   0x30: r15  - 被调用者保存
 *   0x38: rip  - 返回地址
 */

#if defined(__x86_64__) && !defined(_WIN32)

.text

/* ===== vox_coro_ctx_swap_asm ===== */
/* void vox_coro_ctx_swap_asm(vox_coro_ctx_t* from, vox_coro_ctx_t* to) */
/* 参数: rdi = from, rsi = to */

.globl vox_coro_ctx_swap_asm
#if !defined(__APPLE__)
.type vox_coro_ctx_swap_asm, @function
#endif
.align 16
vox_coro_ctx_swap_asm:
    /* 保存被调用者保存寄存器到 from 上下文 */
    movq %rsp, 0x00(%rdi)    /* 保存 rsp */
    movq %rbp, 0x08(%rdi)    /* 保存 rbp */
    movq %rbx, 0x10(%rdi)    /* 保存 rbx */
    movq %r12, 0x18(%rdi)    /* 保存 r12 */
    movq %r13, 0x20(%rdi)    /* 保存 r13 */
    movq %r14, 0x28(%rdi)    /* 保存 r14 */
    movq %r15, 0x30(%rdi)    /* 保存 r15 */

    /* 保存返回地址 (栈顶) */
    movq (%rsp), %rax
    movq %rax, 0x38(%rdi)    /* 保存 rip */

    /* 从 to 上下文恢复寄存器 */
    movq 0x10(%rsi), %rbx    /* 恢复 rbx */
    movq 0x18(%rsi), %r12    /* 恢复 r12 */
    movq 0x20(%rsi), %r13    /* 恢复 r13 */
    movq 0x28(%rsi), %r14    /* 恢复 r14 */
    movq 0x30(%rsi), %r15    /* 恢复 r15 */
    movq 0x08(%rsi), %rbp    /* 恢复 rbp */
    movq 0x00(%rsi), %rsp    /* 恢复 rsp */

    /* 跳转到目标地址 */
    movq 0x38(%rsi), %rax    /* 加载 rip */
    jmpq *%rax               /* 跳转 */

#if !defined(__APPLE__)
.size vox_coro_ctx_swap_asm, .-vox_coro_ctx_swap_asm
#endif


/* ===== vox_coro_ctx_jump_asm ===== */
/* void vox_coro_ctx_jump_asm(vox_coro_ctx_t* ctx) */
/* 参数: rdi = ctx */

.globl vox_coro_ctx_jump_asm
#if !defined(__APPLE__)
.type vox_coro_ctx_jump_asm, @function
#endif
.align 16
vox_coro_ctx_jump_asm:
    /* 从上下文恢复寄存器 */
    movq 0x10(%rdi), %rbx    /* 恢复 rbx */
    movq 0x18(%rdi), %r12    /* 恢复 r12 */
    movq 0x20(%rdi), %r13    /* 恢复 r13 */
    movq 0x28(%rdi), %r14    /* 恢复 r14 */
    movq 0x30(%rdi), %r15    /* 恢复 r15 */
    movq 0x08(%rdi), %rbp    /* 恢复 rbp */
    movq 0x00(%rdi), %rsp    /* 恢复 rsp */

    /* 跳转到目标地址 */
    movq 0x38(%rdi), %rax    /* 加载 rip */
    jmpq *%rax               /* 跳转 */

#if !defined(__APPLE__)
.size vox_coro_ctx_jump_asm, .-vox_coro_ctx_jump_asm
#endif

#endif /* __x86_64__ && !_WIN32 */
