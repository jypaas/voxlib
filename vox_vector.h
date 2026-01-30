/*
 * vox_vector.h - 高性能动态数组
 * 使用 vox_mpool 内存池管理所有内存分配
 */

#ifndef VOX_VECTOR_H
#define VOX_VECTOR_H

#include "vox_mpool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 动态数组不透明类型 */
typedef struct vox_vector vox_vector_t;

/* 元素释放函数类型（可选） */
typedef void (*vox_vector_free_func_t)(void* elem);

/* 遍历回调函数类型 */
typedef void (*vox_vector_visit_func_t)(void* elem, size_t index, void* user_data);

/* 动态数组配置 */
typedef struct {
    size_t initial_capacity;        /* 初始容量，0表示使用默认值 */
    vox_vector_free_func_t elem_free; /* 元素释放函数，NULL表示不释放 */
} vox_vector_config_t;

/**
 * 使用默认配置创建动态数组
 * @param mpool 内存池指针，必须非NULL
 * @return 成功返回动态数组指针，失败返回NULL
 */
vox_vector_t* vox_vector_create(vox_mpool_t* mpool);

/**
 * 使用自定义配置创建动态数组
 * @param mpool 内存池指针，必须非NULL
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回动态数组指针，失败返回NULL
 */
vox_vector_t* vox_vector_create_with_config(vox_mpool_t* mpool, const vox_vector_config_t* config);

/**
 * 在数组末尾添加元素
 * @param vec 动态数组指针
 * @param elem 元素指针
 * @return 成功返回0，失败返回-1
 */
int vox_vector_push(vox_vector_t* vec, void* elem);

/**
 * 移除并返回数组末尾的元素
 * @param vec 动态数组指针
 * @return 成功返回元素指针，数组为空返回NULL
 */
void* vox_vector_pop(vox_vector_t* vec);

/**
 * 在指定位置插入元素
 * @param vec 动态数组指针
 * @param index 插入位置索引
 * @param elem 元素指针
 * @return 成功返回0，失败返回-1
 */
int vox_vector_insert(vox_vector_t* vec, size_t index, void* elem);

/**
 * 移除指定位置的元素
 * @param vec 动态数组指针
 * @param index 要移除的位置索引
 * @return 成功返回被移除的元素指针，失败返回NULL
 */
void* vox_vector_remove(vox_vector_t* vec, size_t index);

/**
 * 获取指定位置的元素（不移除）
 * @param vec 动态数组指针
 * @param index 位置索引
 * @return 成功返回元素指针，失败返回NULL
 */
void* vox_vector_get(const vox_vector_t* vec, size_t index);

/**
 * 设置指定位置的元素
 * @param vec 动态数组指针
 * @param index 位置索引
 * @param elem 新元素指针
 * @return 成功返回0，失败返回-1
 */
int vox_vector_set(vox_vector_t* vec, size_t index, void* elem);

/**
 * 获取数组中的元素数量
 * @param vec 动态数组指针
 * @return 返回元素数量
 */
size_t vox_vector_size(const vox_vector_t* vec);

/**
 * 获取数组的容量
 * @param vec 动态数组指针
 * @return 返回容量
 */
size_t vox_vector_capacity(const vox_vector_t* vec);

/**
 * 检查数组是否为空
 * @param vec 动态数组指针
 * @return 为空返回true，否则返回false
 */
bool vox_vector_empty(const vox_vector_t* vec);

/**
 * 清空数组（保留容量）
 * @param vec 动态数组指针
 */
void vox_vector_clear(vox_vector_t* vec);

/**
 * 调整数组大小
 * @param vec 动态数组指针
 * @param new_size 新大小
 * @return 成功返回0，失败返回-1
 */
int vox_vector_resize(vox_vector_t* vec, size_t new_size);

/**
 * 预留容量
 * @param vec 动态数组指针
 * @param capacity 目标容量
 * @return 成功返回0，失败返回-1
 */
int vox_vector_reserve(vox_vector_t* vec, size_t capacity);

/**
 * 遍历数组中的所有元素
 * @param vec 动态数组指针
 * @param visit 访问函数，参数为(elem, index, user_data)
 * @param user_data 用户数据指针
 * @return 返回遍历的元素数量
 */
size_t vox_vector_foreach(vox_vector_t* vec, vox_vector_visit_func_t visit, void* user_data);

/**
 * 销毁动态数组并释放所有资源
 * @param vec 动态数组指针
 */
void vox_vector_destroy(vox_vector_t* vec);

#ifdef __cplusplus
}
#endif

#endif /* VOX_VECTOR_H */
