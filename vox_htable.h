/*
 * vox_htable.h - 高性能哈希表
 * 使用 wyhash 哈希函数，支持开放寻址法
 */

#ifndef VOX_HTABLE_H
#define VOX_HTABLE_H

#include "vox_mpool.h"
#include "vox_kv_types.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 哈希表不透明类型 */
typedef struct vox_htable vox_htable_t;

/* 哈希函数类型 */
typedef uint64_t (*vox_hash_func_t)(const void* key, size_t key_len);

/* 哈希表配置 */
typedef struct {
    size_t initial_capacity;        /* 初始容量，0表示使用默认值 */
    double load_factor;              /* 负载因子阈值（0.0-1.0），0表示使用默认值0.75 */
    vox_hash_func_t hash_func;      /* 自定义哈希函数，NULL表示使用默认wyhash */
    vox_key_cmp_func_t key_cmp;     /* 键比较函数，NULL表示使用memcmp */
    vox_key_free_func_t key_free;   /* 键释放函数，NULL表示不释放 */
    vox_value_free_func_t value_free; /* 值释放函数，NULL表示不释放 */
} vox_htable_config_t;

/**
 * 使用默认配置创建哈希表
 * @param mpool 内存池指针，必须非NULL
 * @return 成功返回哈希表指针，失败返回NULL
 */
vox_htable_t* vox_htable_create(vox_mpool_t* mpool);

/**
 * 使用自定义配置创建哈希表
 * @param mpool 内存池指针，必须非NULL
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回哈希表指针，失败返回NULL
 */
vox_htable_t* vox_htable_create_with_config(vox_mpool_t* mpool, const vox_htable_config_t* config);

/**
 * 插入或更新键值对
 * @param htable 哈希表指针
 * @param key 键指针
 * @param key_len 键长度（字节）
 * @param value 值指针
 * @return 成功返回0，失败返回-1
 */
int vox_htable_set(vox_htable_t* htable, const void* key, size_t key_len, void* value);

/**
 * 获取值
 * @param htable 哈希表指针
 * @param key 键指针
 * @param key_len 键长度（字节）
 * @return 成功返回值指针，不存在返回NULL
 */
void* vox_htable_get(const vox_htable_t* htable, const void* key, size_t key_len);

/**
 * 删除键值对
 * @param htable 哈希表指针
 * @param key 键指针
 * @param key_len 键长度（字节）
 * @return 成功返回0，不存在返回-1
 */
int vox_htable_delete(vox_htable_t* htable, const void* key, size_t key_len);

/**
 * 检查键是否存在
 * @param htable 哈希表指针
 * @param key 键指针
 * @param key_len 键长度（字节）
 * @return 存在返回true，不存在返回false
 */
bool vox_htable_contains(const vox_htable_t* htable, const void* key, size_t key_len);

/**
 * 获取哈希表中的元素数量
 * @param htable 哈希表指针
 * @return 返回元素数量
 */
size_t vox_htable_size(const vox_htable_t* htable);

/**
 * 检查哈希表是否为空
 * @param htable 哈希表指针
 * @return 为空返回true，否则返回false
 */
bool vox_htable_empty(const vox_htable_t* htable);

/**
 * 清空哈希表（保留容量）
 * @param htable 哈希表指针
 */
void vox_htable_clear(vox_htable_t* htable);

/**
 * 销毁哈希表并释放所有资源
 * @param htable 哈希表指针
 */
void vox_htable_destroy(vox_htable_t* htable);

/**
 * 遍历哈希表中的所有键值对
 * @param htable 哈希表指针
 * @param callback 回调函数，参数为(key, key_len, value, user_data)
 * @param user_data 用户数据指针
 * @return 返回遍历的元素数量
 */
size_t vox_htable_foreach(vox_htable_t* htable, 
                          void (*callback)(const void* key, size_t key_len, void* value, void* user_data),
                          void* user_data);

/**
 * 获取哈希表统计信息（用于调试）
 * @param htable 哈希表指针
 * @param capacity 输出容量
 * @param size 输出元素数量
 * @param load_factor 输出当前负载因子
 */
void vox_htable_stats(const vox_htable_t* htable, size_t* capacity, size_t* size, double* load_factor);

#ifdef __cplusplus
}
#endif

#endif /* VOX_HTABLE_H */
