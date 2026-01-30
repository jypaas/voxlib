/*
 * vox_mheap.h - 高性能最小堆
 * 使用 vox_mpool 内存池管理所有内存分配
 */

#ifndef VOX_MHEAP_H
#define VOX_MHEAP_H

#include "vox_mpool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 最小堆不透明类型 */
typedef struct vox_mheap vox_mheap_t;

/* 元素比较函数类型 */
typedef int (*vox_mheap_cmp_func_t)(const void* elem1, const void* elem2);

/* 元素释放函数类型（可选） */
typedef void (*vox_mheap_free_func_t)(void* elem);

/* 最小堆配置 */
typedef struct {
    size_t initial_capacity;        /* 初始容量，0表示使用默认值 */
    vox_mheap_cmp_func_t cmp_func;  /* 元素比较函数，NULL表示使用指针比较 */
    vox_mheap_free_func_t elem_free; /* 元素释放函数，NULL表示不释放 */
} vox_mheap_config_t;

/**
 * 使用默认配置创建最小堆
 * @param mpool 内存池指针，必须非NULL
 * @return 成功返回最小堆指针，失败返回NULL
 */
vox_mheap_t* vox_mheap_create(vox_mpool_t* mpool);

/**
 * 使用自定义配置创建最小堆
 * @param mpool 内存池指针，必须非NULL
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回最小堆指针，失败返回NULL
 */
vox_mheap_t* vox_mheap_create_with_config(vox_mpool_t* mpool, const vox_mheap_config_t* config);

/**
 * 插入元素
 * @param heap 最小堆指针
 * @param elem 元素指针
 * @return 成功返回0，失败返回-1
 */
int vox_mheap_push(vox_mheap_t* heap, void* elem);

/**
 * 获取最小元素（不移除）
 * @param heap 最小堆指针
 * @return 成功返回最小元素指针，堆为空返回NULL
 */
void* vox_mheap_peek(const vox_mheap_t* heap);

/**
 * 移除并返回最小元素
 * @param heap 最小堆指针
 * @return 成功返回最小元素指针，堆为空返回NULL
 */
void* vox_mheap_pop(vox_mheap_t* heap);

/**
 * 获取堆中的元素数量
 * @param heap 最小堆指针
 * @return 返回元素数量
 */
size_t vox_mheap_size(const vox_mheap_t* heap);

/**
 * 检查堆是否为空
 * @param heap 最小堆指针
 * @return 为空返回true，否则返回false
 */
bool vox_mheap_empty(const vox_mheap_t* heap);

/**
 * 清空堆（保留容量）
 * @param heap 最小堆指针
 */
void vox_mheap_clear(vox_mheap_t* heap);

/**
 * 销毁堆并释放所有资源
 * @param heap 最小堆指针
 */
void vox_mheap_destroy(vox_mheap_t* heap);

/**
 * 遍历堆中的所有元素
 * @param heap 最小堆指针
 * @param visit 访问函数，参数为(elem, user_data)
 * @param user_data 用户数据指针
 * @return 返回遍历的元素数量
 */
size_t vox_mheap_foreach(vox_mheap_t* heap,
                         void (*visit)(void* elem, void* user_data),
                         void* user_data);

/**
 * 从堆中移除指定元素
 * @param heap 最小堆指针
 * @param elem 要移除的元素指针
 * @return 成功返回0，元素不存在返回-1
 */
int vox_mheap_remove(vox_mheap_t* heap, void* elem);

#ifdef __cplusplus
}
#endif

#endif /* VOX_MHEAP_H */
