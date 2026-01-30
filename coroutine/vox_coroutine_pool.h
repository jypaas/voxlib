/*
 * vox_coroutine_pool.h - 协程池
 * 提供协程结构和栈的复用，减少内存分配开销
 */

#ifndef VOX_COROUTINE_POOL_H
#define VOX_COROUTINE_POOL_H

#include "../vox_os.h"
#include "../vox_mpool.h"
#include "../vox_list.h"
#include "../vox_mutex.h"
#include "../vox_loop.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_coroutine_pool vox_coroutine_pool_t;

/* ===== 协程池配置 ===== */

typedef struct vox_coroutine_pool_config {
    size_t initial_count;      /* 预分配数量 (默认: 64) */
    size_t max_count;          /* 最大数量 (0=无限制) */
    size_t stack_size;         /* 栈大小 (默认: 64KB) */
    bool use_guard_pages;      /* 启用 guard page (默认: true) */
    bool thread_safe;          /* 线程安全模式 (默认: false) */
} vox_coroutine_pool_config_t;

/* ===== 协程池统计 ===== */

typedef struct vox_coroutine_pool_stats {
    size_t total_created;      /* 总创建数 */
    size_t total_acquired;     /* 总获取数 */
    size_t total_released;     /* 总释放数 */
    size_t current_free;       /* 当前空闲数 */
    size_t current_in_use;     /* 当前使用中 */
    size_t peak_in_use;        /* 峰值使用数 */
    size_t stack_size;         /* 栈大小 */
} vox_coroutine_pool_stats_t;

/* ===== 池化协程槽 ===== */

typedef struct vox_coroutine_slot {
    vox_list_node_t node;      /* 链表节点 (用于空闲列表) */
    void* stack;               /* 栈内存 */
    size_t stack_size;         /* 栈大小 */
    bool in_use;               /* 是否正在使用 */
    void* guard_page;          /* guard page 地址 (如果启用) */
} vox_coroutine_slot_t;

/* ===== API 函数 ===== */

/**
 * 获取默认配置
 * @param config 配置结构指针
 */
void vox_coroutine_pool_config_default(vox_coroutine_pool_config_t* config);

/**
 * 创建协程池
 * @param loop 事件循环指针
 * @param config 配置 (NULL 使用默认配置)
 * @return 成功返回池指针，失败返回 NULL
 */
vox_coroutine_pool_t* vox_coroutine_pool_create(
    vox_loop_t* loop,
    const vox_coroutine_pool_config_t* config);

/**
 * 销毁协程池
 * @param pool 池指针
 */
void vox_coroutine_pool_destroy(vox_coroutine_pool_t* pool);

/**
 * 从池中获取协程槽
 * @param pool 池指针
 * @return 成功返回槽指针，失败返回 NULL
 */
vox_coroutine_slot_t* vox_coroutine_pool_acquire(vox_coroutine_pool_t* pool);

/**
 * 将协程槽归还到池中
 * @param pool 池指针
 * @param slot 槽指针
 */
void vox_coroutine_pool_release(vox_coroutine_pool_t* pool,
                                 vox_coroutine_slot_t* slot);

/**
 * 获取池统计信息
 * @param pool 池指针
 * @param stats 统计结构指针
 */
void vox_coroutine_pool_get_stats(const vox_coroutine_pool_t* pool,
                                   vox_coroutine_pool_stats_t* stats);

/**
 * 预热池 (预分配指定数量的槽)
 * @param pool 池指针
 * @param count 预分配数量
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_pool_warmup(vox_coroutine_pool_t* pool, size_t count);

/**
 * 收缩池 (释放多余的空闲槽)
 * @param pool 池指针
 * @param keep_count 保留的空闲槽数量
 * @return 释放的槽数量
 */
size_t vox_coroutine_pool_shrink(vox_coroutine_pool_t* pool, size_t keep_count);

#ifdef __cplusplus
}
#endif

#endif /* VOX_COROUTINE_POOL_H */
