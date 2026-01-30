/*
 * vox_atomic.h - 跨平台原子操作抽象API
 * 提供统一的原子操作接口，支持整数和指针类型
 */

#ifndef VOX_ATOMIC_H
#define VOX_ATOMIC_H

#include "vox_os.h"
#include "vox_mpool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 原子整数类型 ===== */

/* 原子整数不透明类型 */
typedef struct vox_atomic_int vox_atomic_int_t;

/**
 * 创建原子整数
 * @param mpool 内存池指针，必须非NULL
 * @param initial_value 初始值
 * @return 成功返回原子整数指针，失败返回NULL
 */
vox_atomic_int_t* vox_atomic_int_create(vox_mpool_t* mpool, int32_t initial_value);

/**
 * 销毁原子整数
 * @param atomic 原子整数指针
 */
void vox_atomic_int_destroy(vox_atomic_int_t* atomic);

/**
 * 加载原子整数的值
 * @param atomic 原子整数指针
 * @return 返回当前值
 */
int32_t vox_atomic_int_load(const vox_atomic_int_t* atomic);

/**
 * 存储值到原子整数
 * @param atomic 原子整数指针
 * @param value 要存储的值
 */
void vox_atomic_int_store(vox_atomic_int_t* atomic, int32_t value);

/**
 * 交换原子整数的值
 * @param atomic 原子整数指针
 * @param value 新值
 * @return 返回旧值
 */
int32_t vox_atomic_int_exchange(vox_atomic_int_t* atomic, int32_t value);

/**
 * 比较并交换（CAS）
 * @param atomic 原子整数指针
 * @param expected 期望的旧值（输入输出参数）
 * @param desired 期望的新值
 * @return 成功返回true，失败返回false（expected会被更新为实际值）
 */
bool vox_atomic_int_compare_exchange(vox_atomic_int_t* atomic, int32_t* expected, int32_t desired);

/**
 * 原子加法
 * @param atomic 原子整数指针
 * @param value 要加的值
 * @return 返回操作前的值
 */
int32_t vox_atomic_int_add(vox_atomic_int_t* atomic, int32_t value);

/**
 * 原子减法
 * @param atomic 原子整数指针
 * @param value 要减的值
 * @return 返回操作前的值
 */
int32_t vox_atomic_int_sub(vox_atomic_int_t* atomic, int32_t value);

/**
 * 原子递增
 * @param atomic 原子整数指针
 * @return 返回递增后的值
 */
int32_t vox_atomic_int_increment(vox_atomic_int_t* atomic);

/**
 * 原子递减
 * @param atomic 原子整数指针
 * @return 返回递减后的值
 */
int32_t vox_atomic_int_decrement(vox_atomic_int_t* atomic);

/**
 * 原子按位与
 * @param atomic 原子整数指针
 * @param value 要按位与的值
 * @return 返回操作前的值
 */
int32_t vox_atomic_int_and(vox_atomic_int_t* atomic, int32_t value);

/**
 * 原子按位或
 * @param atomic 原子整数指针
 * @param value 要按位或的值
 * @return 返回操作前的值
 */
int32_t vox_atomic_int_or(vox_atomic_int_t* atomic, int32_t value);

/**
 * 原子按位异或
 * @param atomic 原子整数指针
 * @param value 要按位异或的值
 * @return 返回操作前的值
 */
int32_t vox_atomic_int_xor(vox_atomic_int_t* atomic, int32_t value);

/* ===== 原子长整数类型 ===== */

/* 原子长整数不透明类型 */
typedef struct vox_atomic_long vox_atomic_long_t;

/**
 * 创建原子长整数
 * @param mpool 内存池指针，必须非NULL
 * @param initial_value 初始值
 * @return 成功返回原子长整数指针，失败返回NULL
 */
vox_atomic_long_t* vox_atomic_long_create(vox_mpool_t* mpool, int64_t initial_value);

/**
 * 销毁原子长整数
 * @param atomic 原子长整数指针
 */
void vox_atomic_long_destroy(vox_atomic_long_t* atomic);

/**
 * 加载原子长整数的值
 * @param atomic 原子长整数指针
 * @return 返回当前值
 */
int64_t vox_atomic_long_load(const vox_atomic_long_t* atomic);

/**
 * 加载原子长整数的值（acquire语义）
 * @param atomic 原子长整数指针
 * @return 返回当前值
 * @note acquire语义确保此操作之后的读写不会被重排到此操作之前
 */
int64_t vox_atomic_long_load_acquire(const vox_atomic_long_t* atomic);

/**
 * 存储值到原子长整数
 * @param atomic 原子长整数指针
 * @param value 要存储的值
 */
void vox_atomic_long_store(vox_atomic_long_t* atomic, int64_t value);

/**
 * 存储值到原子长整数（release语义）
 * @param atomic 原子长整数指针
 * @param value 要存储的值
 * @note release语义确保此操作之前的读写不会被重排到此操作之后
 */
void vox_atomic_long_store_release(vox_atomic_long_t* atomic, int64_t value);

/**
 * 交换原子长整数的值
 * @param atomic 原子长整数指针
 * @param value 新值
 * @return 返回旧值
 */
int64_t vox_atomic_long_exchange(vox_atomic_long_t* atomic, int64_t value);

/**
 * 比较并交换（CAS）
 * @param atomic 原子长整数指针
 * @param expected 期望的旧值（输入输出参数）
 * @param desired 期望的新值
 * @return 成功返回true，失败返回false（expected会被更新为实际值）
 */
bool vox_atomic_long_compare_exchange(vox_atomic_long_t* atomic, int64_t* expected, int64_t desired);

/**
 * 原子加法
 * @param atomic 原子长整数指针
 * @param value 要加的值
 * @return 返回操作前的值
 */
int64_t vox_atomic_long_add(vox_atomic_long_t* atomic, int64_t value);

/**
 * 原子减法
 * @param atomic 原子长整数指针
 * @param value 要减的值
 * @return 返回操作前的值
 */
int64_t vox_atomic_long_sub(vox_atomic_long_t* atomic, int64_t value);

/**
 * 原子递增
 * @param atomic 原子长整数指针
 * @return 返回递增后的值
 */
int64_t vox_atomic_long_increment(vox_atomic_long_t* atomic);

/**
 * 原子递减
 * @param atomic 原子长整数指针
 * @return 返回递减后的值
 */
int64_t vox_atomic_long_decrement(vox_atomic_long_t* atomic);

/* ===== 原子指针类型 ===== */

/* 原子指针不透明类型 */
typedef struct vox_atomic_ptr vox_atomic_ptr_t;

/**
 * 创建原子指针
 * @param mpool 内存池指针，必须非NULL
 * @param initial_value 初始值（可为NULL）
 * @return 成功返回原子指针指针，失败返回NULL
 */
vox_atomic_ptr_t* vox_atomic_ptr_create(vox_mpool_t* mpool, void* initial_value);

/**
 * 销毁原子指针
 * @param atomic 原子指针指针
 */
void vox_atomic_ptr_destroy(vox_atomic_ptr_t* atomic);

/**
 * 加载原子指针的值
 * @param atomic 原子指针指针
 * @return 返回当前值
 */
void* vox_atomic_ptr_load(const vox_atomic_ptr_t* atomic);

/**
 * 存储值到原子指针
 * @param atomic 原子指针指针
 * @param value 要存储的值（可为NULL）
 */
void vox_atomic_ptr_store(vox_atomic_ptr_t* atomic, void* value);

/**
 * 交换原子指针的值
 * @param atomic 原子指针指针
 * @param value 新值
 * @return 返回旧值
 */
void* vox_atomic_ptr_exchange(vox_atomic_ptr_t* atomic, void* value);

/**
 * 比较并交换（CAS）
 * @param atomic 原子指针指针
 * @param expected 期望的旧值（输入输出参数）
 * @param desired 期望的新值
 * @return 成功返回true，失败返回false（expected会被更新为实际值）
 */
bool vox_atomic_ptr_compare_exchange(vox_atomic_ptr_t* atomic, void** expected, void* desired);

#ifdef __cplusplus
}
#endif

#endif /* VOX_ATOMIC_H */
