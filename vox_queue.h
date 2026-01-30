/*
 * vox_queue.h - 高性能队列
 * 使用循环数组实现，O(1) 入队和出队操作
 * 使用 vox_mpool 内存池管理所有内存分配
 */

#ifndef VOX_QUEUE_H
#define VOX_QUEUE_H

#include "vox_mpool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 队列类型 */
typedef enum {
    VOX_QUEUE_TYPE_NORMAL = 0,  /* 普通队列（线程不安全，支持动态扩容） */
    VOX_QUEUE_TYPE_SPSC,        /* 单生产者单消费者无锁队列（固定容量） */
    VOX_QUEUE_TYPE_MPSC         /* 多生产者单消费者无锁队列（固定容量） */
} vox_queue_type_t;

/* 队列不透明类型 */
typedef struct vox_queue vox_queue_t;

/* 元素释放函数类型（可选） */
typedef void (*vox_queue_free_func_t)(void* elem);

/* 遍历回调函数类型 */
typedef void (*vox_queue_visit_func_t)(void* elem, size_t index, void* user_data);

/* 队列配置 */
typedef struct {
    vox_queue_type_t type;          /* 队列类型，默认NORMAL */
    size_t initial_capacity;        /* 初始容量，0表示使用默认值（SPSC/MPSC必须指定且固定） */
    vox_queue_free_func_t elem_free; /* 元素释放函数，NULL表示不释放 */
} vox_queue_config_t;

/**
 * 使用默认配置创建队列
 * @param mpool 内存池指针，必须非NULL
 * @return 成功返回队列指针，失败返回NULL
 */
vox_queue_t* vox_queue_create(vox_mpool_t* mpool);

/**
 * 使用自定义配置创建队列
 * @param mpool 内存池指针，必须非NULL
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回队列指针，失败返回NULL
 */
vox_queue_t* vox_queue_create_with_config(vox_mpool_t* mpool, const vox_queue_config_t* config);

/**
 * 在队列末尾添加元素（入队）
 * @param queue 队列指针
 * @param elem 元素指针
 * @return 成功返回0，失败返回-1
 */
int vox_queue_enqueue(vox_queue_t* queue, void* elem);

/**
 * 移除并返回队列首部的元素（出队）
 * @param queue 队列指针
 * @return 成功返回元素指针，队列为空返回NULL
 */
void* vox_queue_dequeue(vox_queue_t* queue);

/**
 * 获取队列首部元素（不移除）
 * @param queue 队列指针
 * @return 成功返回元素指针，队列为空返回NULL
 */
void* vox_queue_peek(const vox_queue_t* queue);

/**
 * 获取队列中的元素数量
 * @param queue 队列指针
 * @return 返回元素数量
 */
size_t vox_queue_size(const vox_queue_t* queue);

/**
 * 获取队列的容量
 * @param queue 队列指针
 * @return 返回容量
 */
size_t vox_queue_capacity(const vox_queue_t* queue);

/**
 * 检查队列是否为空
 * @param queue 队列指针
 * @return 为空返回true，否则返回false
 */
bool vox_queue_empty(const vox_queue_t* queue);

/**
 * 检查队列是否已满
 * @param queue 队列指针
 * @return 已满返回true，否则返回false
 */
bool vox_queue_full(const vox_queue_t* queue);

/**
 * 清空队列（保留容量）
 * @param queue 队列指针
 */
void vox_queue_clear(vox_queue_t* queue);

/**
 * 遍历队列中的所有元素（从队首到队尾）
 * @param queue 队列指针
 * @param visit 访问函数，参数为(elem, index, user_data)
 * @param user_data 用户数据指针
 * @return 返回遍历的元素数量
 */
size_t vox_queue_foreach(vox_queue_t* queue, vox_queue_visit_func_t visit, void* user_data);

/**
 * 销毁队列并释放所有资源
 * @param queue 队列指针
 */
void vox_queue_destroy(vox_queue_t* queue);

#ifdef __cplusplus
}
#endif

#endif /* VOX_QUEUE_H */
