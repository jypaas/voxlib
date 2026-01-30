/*
 * vox_coroutine_context_arm64.S - ARM64 汇编上下文切换
 * AAPCS64 ABI (Linux/macOS)
 *
 * 上下文结构布局 (vox_coro_ctx_t):
 *   0x00: x19  - 被调用者保存
 *   0x08: x20
 *   0x10: x21
 *   0x18: x22
 *   0x20: x23
 *   0x28: x24
 *   0x30: x25
 *   0x38: x26
 *   0x40: x27
 *   0x48: x28
 *   0x50: x29  - 帧指针 (fp)
 *   0x58: x30  - 链接寄存器 (lr)
 *   0x60: sp   - 栈指针
 *   0x68: d8-d15 - 浮点被调用者保存
 */

#if defined(__aarch64__)

.text

/* ===== vox_coro_ctx_swap_asm ===== */
/* void vox_coro_ctx_swap_asm(vox_coro_ctx_t* from, vox_coro_ctx_t* to) */
/* 参数: x0 = from, x1 = to */

.globl vox_coro_ctx_swap_asm
.type vox_coro_ctx_swap_asm, %function
.align 4
vox_coro_ctx_swap_asm:
    /* 保存被调用者保存寄存器到 from 上下文 */
    stp x19, x20, [x0, #0x00]
    stp x21, x22, [x0, #0x10]
    stp x23, x24, [x0, #0x20]
    stp x25, x26, [x0, #0x30]
    stp x27, x28, [x0, #0x40]
    stp x29, x30, [x0, #0x50]
    mov x2, sp
    str x2, [x0, #0x60]

    /* 保存浮点寄存器 */
    stp d8, d9, [x0, #0x68]
    stp d10, d11, [x0, #0x78]
    stp d12, d13, [x0, #0x88]
    stp d14, d15, [x0, #0x98]

    /* 从 to 上下文恢复寄存器 */
    ldp x19, x20, [x1, #0x00]
    ldp x21, x22, [x1, #0x10]
    ldp x23, x24, [x1, #0x20]
    ldp x25, x26, [x1, #0x30]
    ldp x27, x28, [x1, #0x40]
    ldp x29, x30, [x1, #0x50]
    ldr x2, [x1, #0x60]
    mov sp, x2

    /* 恢复浮点寄存器 */
    ldp d8, d9, [x1, #0x68]
    ldp d10, d11, [x1, #0x78]
    ldp d12, d13, [x1, #0x88]
    ldp d14, d15, [x1, #0x98]

    /* 返回 (跳转到 lr) */
    ret

.size vox_coro_ctx_swap_asm, .-vox_coro_ctx_swap_asm


/* ===== vox_coro_ctx_jump_asm ===== */
/* void vox_coro_ctx_jump_asm(vox_coro_ctx_t* ctx) */
/* 参数: x0 = ctx */

.globl vox_coro_ctx_jump_asm
.type vox_coro_ctx_jump_asm, %function
.align 4
vox_coro_ctx_jump_asm:
    /* 从上下文恢复寄存器 */
    ldp x19, x20, [x0, #0x00]
    ldp x21, x22, [x0, #0x10]
    ldp x23, x24, [x0, #0x20]
    ldp x25, x26, [x0, #0x30]
    ldp x27, x28, [x0, #0x40]
    ldp x29, x30, [x0, #0x50]
    ldr x2, [x0, #0x60]
    mov sp, x2

    /* 恢复浮点寄存器 */
    ldp d8, d9, [x0, #0x68]
    ldp d10, d11, [x0, #0x78]
    ldp d12, d13, [x0, #0x88]
    ldp d14, d15, [x0, #0x98]

    /* 返回 (跳转到 lr) */
    ret

.size vox_coro_ctx_jump_asm, .-vox_coro_ctx_jump_asm

#endif /* __aarch64__ */
