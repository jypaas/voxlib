/*
 * vox_thread.h - 跨平台线程和线程本地存储抽象API
 * 提供统一的线程创建、管理和线程本地存储接口
 */

#ifndef VOX_THREAD_H
#define VOX_THREAD_H

#include "vox_os.h"
#include "vox_mpool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 线程相关 ===== */

/* 线程不透明类型 */
typedef struct vox_thread vox_thread_t;

/* 线程ID类型 */
typedef uint64_t vox_thread_id_t;

/* 线程入口函数类型 */
typedef int (*vox_thread_func_t)(void* user_data);

/**
 * 创建并启动新线程
 * @param mpool 内存池指针，必须非NULL
 * @param func 线程入口函数
 * @param user_data 传递给线程函数的用户数据
 * @return 成功返回线程指针，失败返回NULL
 */
vox_thread_t* vox_thread_create(vox_mpool_t* mpool, vox_thread_func_t func, void* user_data);

/**
 * 等待线程结束并获取返回值
 * @param thread 线程指针
 * @param exit_code 输出线程退出码，可为NULL
 * @return 成功返回0，失败返回-1
 */
int vox_thread_join(vox_thread_t* thread, int* exit_code);

/**
 * 分离线程（线程结束时自动清理资源）
 * @param thread 线程指针
 * @return 成功返回0，失败返回-1
 */
int vox_thread_detach(vox_thread_t* thread);

/**
 * 获取线程ID
 * @param thread 线程指针
 * @return 返回线程ID，失败返回0
 */
vox_thread_id_t vox_thread_id(const vox_thread_t* thread);

/**
 * 获取当前线程ID
 * @return 返回当前线程ID
 */
vox_thread_id_t vox_thread_self(void);

/**
 * 比较两个线程ID是否相等
 * @param id1 线程ID1
 * @param id2 线程ID2
 * @return 相等返回true，否则返回false
 */
bool vox_thread_id_equal(vox_thread_id_t id1, vox_thread_id_t id2);

/**
 * 让出CPU时间片
 */
void vox_thread_yield(void);

/**
 * 休眠指定毫秒数
 * @param ms 毫秒数
 */
void vox_thread_sleep(uint32_t ms);

/* ===== 线程优先级 ===== */

/* 线程优先级级别 */
typedef enum {
    VOX_THREAD_PRIORITY_LOWEST = 0,      /* 最低优先级 */
    VOX_THREAD_PRIORITY_BELOW_NORMAL,    /* 低于正常 */
    VOX_THREAD_PRIORITY_NORMAL,          /* 正常优先级（默认） */
    VOX_THREAD_PRIORITY_ABOVE_NORMAL,    /* 高于正常 */
    VOX_THREAD_PRIORITY_HIGHEST,         /* 最高优先级 */
    VOX_THREAD_PRIORITY_TIME_CRITICAL    /* 时间关键（实时） */
} vox_thread_priority_t;

/**
 * 设置线程优先级
 * @param thread 线程指针，为NULL时设置当前线程
 * @param priority 优先级级别
 * @return 成功返回0，失败返回-1
 */
int vox_thread_set_priority(vox_thread_t* thread, vox_thread_priority_t priority);

/**
 * 获取线程优先级
 * @param thread 线程指针，为NULL时获取当前线程
 * @param priority 输出优先级级别，可为NULL
 * @return 成功返回0，失败返回-1
 */
int vox_thread_get_priority(vox_thread_t* thread, vox_thread_priority_t* priority);

/* ===== CPU亲和力 ===== */

/**
 * 设置线程CPU亲和力（绑定到指定CPU核心）
 * @param thread 线程指针，为NULL时设置当前线程
 * @param cpu_mask CPU掩码，每一位代表一个CPU核心（0表示CPU 0，1表示CPU 1，以此类推）
 *                 例如：0x1表示只绑定到CPU 0，0x3表示绑定到CPU 0和1
 * @return 成功返回0，失败返回-1
 */
int vox_thread_set_affinity(vox_thread_t* thread, uint64_t cpu_mask);

/**
 * 获取线程CPU亲和力
 * @param thread 线程指针，为NULL时获取当前线程
 * @param cpu_mask 输出CPU掩码，可为NULL
 * @return 成功返回0，失败返回-1
 */
int vox_thread_get_affinity(vox_thread_t* thread, uint64_t* cpu_mask);

/* ===== 线程本地存储 (TLS) ===== */

/* TLS 键不透明类型（注意：与 vox_tls.h 的 vox_tls_t 即 TLS 连接句柄不同） */
typedef struct vox_tls_key vox_tls_key_t;

/**
 * 创建线程本地存储键
 * @param mpool 内存池指针，必须非NULL
 * @param destructor 析构函数，当线程退出时自动调用，可为NULL
 * @return 成功返回TLS键指针，失败返回NULL
 */
vox_tls_key_t* vox_tls_key_create(vox_mpool_t* mpool, void (*destructor)(void*));

/**
 * 设置线程本地存储值
 * @param key TLS键指针
 * @param value 要设置的值
 * @return 成功返回0，失败返回-1
 */
int vox_tls_set(vox_tls_key_t* key, void* value);

/**
 * 获取线程本地存储值
 * @param key TLS键指针
 * @return 返回值指针，未设置返回NULL
 */
void* vox_tls_get(vox_tls_key_t* key);

/**
 * 销毁线程本地存储键
 * @param key TLS键指针
 */
void vox_tls_key_destroy(vox_tls_key_t* key);

#ifdef __cplusplus
}
#endif

#endif /* VOX_THREAD_H */
