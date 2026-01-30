/*
 * vox_kv_types.h - 键/值回调类型（htable、rbtree 等共用）
 */

#ifndef VOX_KV_TYPES_H
#define VOX_KV_TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 键比较函数类型 */
typedef int (*vox_key_cmp_func_t)(const void* key1, const void* key2, size_t key_len);

/* 键释放函数类型（可选） */
typedef void (*vox_key_free_func_t)(void* key);

/* 值释放函数类型（可选） */
typedef void (*vox_value_free_func_t)(void* value);

#ifdef __cplusplus
}
#endif

#endif /* VOX_KV_TYPES_H */
