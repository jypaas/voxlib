/*
 * vox_atomic.c - 跨平台原子操作实现
 */

#include "vox_atomic.h"
#include "vox_os.h"
#include "vox_mpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef VOX_OS_WINDOWS
    #include <stdatomic.h>
#endif

/* ===== 原子整数 ===== */

#ifdef VOX_OS_WINDOWS
struct vox_atomic_int {
    vox_mpool_t* mpool;  /* 内存池指针 */
    volatile LONG value;  /* Windows使用LONG（32位） */
};
#else
struct vox_atomic_int {
    vox_mpool_t* mpool;  /* 内存池指针 */
    atomic_int value;    /* C11原子类型 */
};
#endif

vox_atomic_int_t* vox_atomic_int_create(vox_mpool_t* mpool, int32_t initial_value) {
    if (!mpool) return NULL;
    
    vox_atomic_int_t* atomic = (vox_atomic_int_t*)vox_mpool_alloc(mpool, sizeof(vox_atomic_int_t));
    if (!atomic) return NULL;
    
    atomic->mpool = mpool;
    
#ifdef VOX_OS_WINDOWS
    atomic->value = (LONG)initial_value;
#else
    atomic_init(&atomic->value, initial_value);
#endif
    
    return atomic;
}

void vox_atomic_int_destroy(vox_atomic_int_t* atomic) {
    if (!atomic) return;
    
    vox_mpool_t* mpool = atomic->mpool;
    vox_mpool_free(mpool, atomic);
}

int32_t vox_atomic_int_load(const vox_atomic_int_t* atomic) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    /* 使用volatile读取，在x86/x64上对对齐的32位整数是原子的 */
    return (int32_t)((vox_atomic_int_t*)atomic)->value;
#else
    return atomic_load(&atomic->value);
#endif
}

void vox_atomic_int_store(vox_atomic_int_t* atomic, int32_t value) {
    if (!atomic) return;
    
#ifdef VOX_OS_WINDOWS
    InterlockedExchange(&atomic->value, (LONG)value);
#else
    atomic_store(&atomic->value, value);
#endif
}

int32_t vox_atomic_int_exchange(vox_atomic_int_t* atomic, int32_t value) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int32_t)InterlockedExchange(&atomic->value, (LONG)value);
#else
    return atomic_exchange(&atomic->value, value);
#endif
}

bool vox_atomic_int_compare_exchange(vox_atomic_int_t* atomic, int32_t* expected, int32_t desired) {
    if (!atomic || !expected) return false;
    
#ifdef VOX_OS_WINDOWS
    LONG old = (LONG)*expected;
    LONG result = InterlockedCompareExchange(&atomic->value, (LONG)desired, old);
    *expected = (int32_t)result;
    return (result == old);
#else
    return atomic_compare_exchange_strong(&atomic->value, expected, desired);
#endif
}

int32_t vox_atomic_int_add(vox_atomic_int_t* atomic, int32_t value) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int32_t)InterlockedExchangeAdd(&atomic->value, (LONG)value);
#else
    return atomic_fetch_add(&atomic->value, value);
#endif
}

int32_t vox_atomic_int_sub(vox_atomic_int_t* atomic, int32_t value) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int32_t)InterlockedExchangeAdd(&atomic->value, -(LONG)value);
#else
    return atomic_fetch_sub(&atomic->value, value);
#endif
}

int32_t vox_atomic_int_increment(vox_atomic_int_t* atomic) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int32_t)InterlockedIncrement(&atomic->value);
#else
    return atomic_fetch_add(&atomic->value, 1) + 1;
#endif
}

int32_t vox_atomic_int_decrement(vox_atomic_int_t* atomic) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int32_t)InterlockedDecrement(&atomic->value);
#else
    return atomic_fetch_sub(&atomic->value, 1) - 1;
#endif
}

int32_t vox_atomic_int_and(vox_atomic_int_t* atomic, int32_t value) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int32_t)InterlockedAnd(&atomic->value, (LONG)value);
#else
    return atomic_fetch_and(&atomic->value, value);
#endif
}

int32_t vox_atomic_int_or(vox_atomic_int_t* atomic, int32_t value) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int32_t)InterlockedOr(&atomic->value, (LONG)value);
#else
    return atomic_fetch_or(&atomic->value, value);
#endif
}

int32_t vox_atomic_int_xor(vox_atomic_int_t* atomic, int32_t value) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int32_t)InterlockedXor(&atomic->value, (LONG)value);
#else
    return atomic_fetch_xor(&atomic->value, value);
#endif
}

/* ===== 原子长整数 ===== */

#ifdef VOX_OS_WINDOWS
struct vox_atomic_long {
    vox_mpool_t* mpool;  /* 内存池指针 */
    volatile LONGLONG value;  /* Windows使用LONGLONG（64位） */
};
#else
struct vox_atomic_long {
    vox_mpool_t* mpool;  /* 内存池指针 */
    atomic_llong value;  /* C11原子类型 */
};
#endif

vox_atomic_long_t* vox_atomic_long_create(vox_mpool_t* mpool, int64_t initial_value) {
    if (!mpool) return NULL;
    
    vox_atomic_long_t* atomic = (vox_atomic_long_t*)vox_mpool_alloc(mpool, sizeof(vox_atomic_long_t));
    if (!atomic) return NULL;
    
    atomic->mpool = mpool;
    
#ifdef VOX_OS_WINDOWS
    atomic->value = (LONGLONG)initial_value;
#else
    atomic_init(&atomic->value, initial_value);
#endif
    
    return atomic;
}

void vox_atomic_long_destroy(vox_atomic_long_t* atomic) {
    if (!atomic) return;
    
    vox_mpool_t* mpool = atomic->mpool;
    vox_mpool_free(mpool, atomic);
}

int64_t vox_atomic_long_load(const vox_atomic_long_t* atomic) {
    if (!atomic) return 0;

#ifdef VOX_OS_WINDOWS
    /* 使用 InterlockedOr64 提供原子读取和适当的内存屏障 */
    /* 在 Windows 上使用 seq_cst 语义 */
    return (int64_t)InterlockedOr64((LONGLONG*)&((vox_atomic_long_t*)atomic)->value, 0);
#else
    return atomic_load(&atomic->value);
#endif
}

int64_t vox_atomic_long_load_acquire(const vox_atomic_long_t* atomic) {
    if (!atomic) return 0;

#ifdef VOX_OS_WINDOWS
    /* Windows: 使用 InterlockedCompareExchange64 提供 acquire 语义 */
    /* CAS 操作隐含 acquire 语义（读取时） */
    return (int64_t)InterlockedCompareExchange64(
        (LONGLONG*)&((vox_atomic_long_t*)atomic)->value, 0, 0);
#else
    return atomic_load_explicit(&atomic->value, memory_order_acquire);
#endif
}

void vox_atomic_long_store(vox_atomic_long_t* atomic, int64_t value) {
    if (!atomic) return;

#ifdef VOX_OS_WINDOWS
    InterlockedExchange64(&atomic->value, (LONGLONG)value);
#else
    atomic_store(&atomic->value, value);
#endif
}

void vox_atomic_long_store_release(vox_atomic_long_t* atomic, int64_t value) {
    if (!atomic) return;

#ifdef VOX_OS_WINDOWS
    /* Windows: InterlockedExchange64 提供 release 语义 */
    /* Exchange 操作隐含 release 语义（写入时） */
    InterlockedExchange64(&atomic->value, (LONGLONG)value);
#else
    atomic_store_explicit(&atomic->value, value, memory_order_release);
#endif
}

int64_t vox_atomic_long_exchange(vox_atomic_long_t* atomic, int64_t value) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int64_t)InterlockedExchange64(&atomic->value, (LONGLONG)value);
#else
    return atomic_exchange(&atomic->value, value);
#endif
}

bool vox_atomic_long_compare_exchange(vox_atomic_long_t* atomic, int64_t* expected, int64_t desired) {
    if (!atomic || !expected) return false;
    
#ifdef VOX_OS_WINDOWS
    LONGLONG old = (LONGLONG)*expected;
    LONGLONG result = InterlockedCompareExchange64(&atomic->value, (LONGLONG)desired, old);
    *expected = (int64_t)result;
    return (result == old);
#else
    return atomic_compare_exchange_strong(&atomic->value, expected, desired);
#endif
}

int64_t vox_atomic_long_add(vox_atomic_long_t* atomic, int64_t value) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int64_t)InterlockedExchangeAdd64(&atomic->value, (LONGLONG)value);
#else
    return atomic_fetch_add(&atomic->value, value);
#endif
}

int64_t vox_atomic_long_sub(vox_atomic_long_t* atomic, int64_t value) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int64_t)InterlockedExchangeAdd64(&atomic->value, -(LONGLONG)value);
#else
    return atomic_fetch_sub(&atomic->value, value);
#endif
}

int64_t vox_atomic_long_increment(vox_atomic_long_t* atomic) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int64_t)InterlockedIncrement64(&atomic->value);
#else
    return atomic_fetch_add(&atomic->value, 1) + 1;
#endif
}

int64_t vox_atomic_long_decrement(vox_atomic_long_t* atomic) {
    if (!atomic) return 0;
    
#ifdef VOX_OS_WINDOWS
    return (int64_t)InterlockedDecrement64(&atomic->value);
#else
    return atomic_fetch_sub(&atomic->value, 1) - 1;
#endif
}

/* ===== 原子指针 ===== */

#ifdef VOX_OS_WINDOWS
struct vox_atomic_ptr {
    vox_mpool_t* mpool;  /* 内存池指针 */
    volatile PVOID value;  /* Windows使用PVOID */
};
#else
struct vox_atomic_ptr {
    vox_mpool_t* mpool;  /* 内存池指针 */
    atomic_uintptr_t value;  /* C11原子指针类型 */
};
#endif

vox_atomic_ptr_t* vox_atomic_ptr_create(vox_mpool_t* mpool, void* initial_value) {
    if (!mpool) return NULL;
    
    vox_atomic_ptr_t* atomic = (vox_atomic_ptr_t*)vox_mpool_alloc(mpool, sizeof(vox_atomic_ptr_t));
    if (!atomic) return NULL;
    
    atomic->mpool = mpool;
    
#ifdef VOX_OS_WINDOWS
    atomic->value = initial_value;
#else
    atomic_init(&atomic->value, (uintptr_t)initial_value);
#endif
    
    return atomic;
}

void vox_atomic_ptr_destroy(vox_atomic_ptr_t* atomic) {
    if (!atomic) return;
    
    vox_mpool_t* mpool = atomic->mpool;
    vox_mpool_free(mpool, atomic);
}

void* vox_atomic_ptr_load(const vox_atomic_ptr_t* atomic) {
    if (!atomic) return NULL;
    
#ifdef VOX_OS_WINDOWS
    /* 使用volatile读取，在x86/x64上对对齐的指针是原子的 */
    return (void*)((vox_atomic_ptr_t*)atomic)->value;
#else
    return (void*)atomic_load(&atomic->value);
#endif
}

void vox_atomic_ptr_store(vox_atomic_ptr_t* atomic, void* value) {
    if (!atomic) return;
    
#ifdef VOX_OS_WINDOWS
    InterlockedExchangePointer(&atomic->value, value);
#else
    atomic_store(&atomic->value, (uintptr_t)value);
#endif
}

void* vox_atomic_ptr_exchange(vox_atomic_ptr_t* atomic, void* value) {
    if (!atomic) return NULL;
    
#ifdef VOX_OS_WINDOWS
    return InterlockedExchangePointer(&atomic->value, value);
#else
    return (void*)atomic_exchange(&atomic->value, (uintptr_t)value);
#endif
}

bool vox_atomic_ptr_compare_exchange(vox_atomic_ptr_t* atomic, void** expected, void* desired) {
    if (!atomic || !expected) return false;
    
#ifdef VOX_OS_WINDOWS
    PVOID old = *expected;
    PVOID result = InterlockedCompareExchangePointer(&atomic->value, desired, old);
    *expected = result;
    return (result == old);
#else
    uintptr_t exp = (uintptr_t)*expected;
    bool success = atomic_compare_exchange_strong(&atomic->value, &exp, (uintptr_t)desired);
    *expected = (void*)exp;
    return success;
#endif
}
