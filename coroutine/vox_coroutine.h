/*
 * vox_coroutine.h - 协程系统
 * 提供async/await风格的协程API，避免回调地狱
 */

#ifndef VOX_COROUTINE_H
#define VOX_COROUTINE_H

#include "../vox_handle.h"
#include "../vox_mpool.h"
#include "vox_coroutine_promise.h"
#include "vox_coroutine_pool.h"
#include "vox_coroutine_scheduler.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
#ifndef VOX_COROUTINE_T_DEFINED
#define VOX_COROUTINE_T_DEFINED
typedef struct vox_coroutine vox_coroutine_t;
#endif

/* 协程状态 */
typedef enum {
    VOX_COROUTINE_READY = 0,      /* 就绪 */
    VOX_COROUTINE_RUNNING,        /* 运行中 */
    VOX_COROUTINE_SUSPENDED,      /* 挂起（等待异步操作） */
    VOX_COROUTINE_COMPLETED,      /* 完成 */
    VOX_COROUTINE_ERROR           /* 错误 */
} vox_coroutine_state_t;

/* 协程入口函数类型 */
typedef void (*vox_coroutine_entry_fn)(vox_coroutine_t* co, void* user_data);

/* ===== 扩展配置 ===== */

typedef struct vox_coroutine_config {
    size_t stack_size;             /* 栈大小 (默认: 64KB) */
    bool use_pool;                 /* 使用协程池 (默认: false) */
    vox_coroutine_pool_t* pool;    /* 协程池 (use_pool=true 时必须) */
} vox_coroutine_config_t;

/**
 * 创建协程
 * @param loop 事件循环指针
 * @param entry 协程入口函数
 * @param user_data 用户数据
 * @param stack_size 栈大小（字节），0表示使用默认值（64KB）
 * @return 成功返回协程指针，失败返回NULL
 */
vox_coroutine_t* vox_coroutine_create(vox_loop_t* loop,
                                      vox_coroutine_entry_fn entry,
                                      void* user_data,
                                      size_t stack_size);

/**
 * 销毁协程
 * @param co 协程指针
 */
void vox_coroutine_destroy(vox_coroutine_t* co);

/**
 * 恢复协程执行
 * @param co 协程指针
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_resume(vox_coroutine_t* co);

/**
 * 挂起当前协程（让出执行权）
 * @param co 协程指针
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_yield(vox_coroutine_t* co);

/**
 * 等待Promise完成（挂起协程直到Promise完成）
 * @param co 协程指针
 * @param promise Promise指针
 * @return Promise的状态码（0=成功，非0=错误）
 */
int vox_coroutine_await(vox_coroutine_t* co, vox_coroutine_promise_t* promise);

/**
 * 获取协程状态
 * @param co 协程指针
 * @return 返回协程状态
 */
vox_coroutine_state_t vox_coroutine_get_state(const vox_coroutine_t* co);

/**
 * 获取协程所属的事件循环
 * @param co 协程指针
 * @return 返回事件循环指针
 */
vox_loop_t* vox_coroutine_get_loop(const vox_coroutine_t* co);

/**
 * 获取协程的用户数据
 * @param co 协程指针
 * @return 返回用户数据
 */
void* vox_coroutine_get_user_data(const vox_coroutine_t* co);

/* ===== 扩展 API (高并发优化) ===== */

/**
 * 获取默认配置
 * @param config 配置结构指针
 */
void vox_coroutine_config_default(vox_coroutine_config_t* config);

/**
 * 使用扩展配置创建协程
 * @param loop 事件循环指针
 * @param entry 协程入口函数
 * @param user_data 用户数据
 * @param config 配置 (NULL 使用默认配置)
 * @return 成功返回协程指针，失败返回NULL
 */
vox_coroutine_t* vox_coroutine_create_ex(vox_loop_t* loop,
                                          vox_coroutine_entry_fn entry,
                                          void* user_data,
                                          const vox_coroutine_config_t* config);

/**
 * 从协程池创建协程
 * @param loop 事件循环指针
 * @param pool 协程池指针
 * @param entry 协程入口函数
 * @param user_data 用户数据
 * @return 成功返回协程指针，失败返回NULL
 */
vox_coroutine_t* vox_coroutine_create_pooled(vox_loop_t* loop,
                                              vox_coroutine_pool_t* pool,
                                              vox_coroutine_entry_fn entry,
                                              void* user_data);

/**
 * 检查协程是否来自池
 * @param co 协程指针
 * @return 来自池返回 true
 */
bool vox_coroutine_is_pooled(const vox_coroutine_t* co);

/**
 * 获取协程的栈大小
 * @param co 协程指针
 * @return 栈大小 (字节)
 */
size_t vox_coroutine_get_stack_size(const vox_coroutine_t* co);

/**
 * 获取当前正在运行的协程
 * @return 当前协程指针，如果不在协程中返回 NULL
 */
vox_coroutine_t* vox_coroutine_current(void);

/* ===== Promise机制 ===== */

/**
 * 创建Promise
 * @param loop 事件循环指针
 * @return 成功返回Promise指针，失败返回NULL
 */
vox_coroutine_promise_t* vox_coroutine_promise_create(vox_loop_t* loop);

/**
 * 销毁Promise
 * @param promise Promise指针
 */
void vox_coroutine_promise_destroy(vox_coroutine_promise_t* promise);

/**
 * 完成Promise（设置结果并恢复等待的协程）
 * @param promise Promise指针
 * @param status 状态码（0=成功，非0=错误）
 * @param result 结果数据（可选，类型由具体操作决定）
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_promise_complete(vox_coroutine_promise_t* promise,
                                   int status,
                                   void* result);

/**
 * 检查Promise是否已完成
 * @param promise Promise指针
 * @return 已完成返回true，否则返回false
 */
bool vox_coroutine_promise_is_completed(const vox_coroutine_promise_t* promise);

/**
 * 获取Promise的状态码
 * @param promise Promise指针
 * @return 返回状态码（仅在Promise完成后有效）
 */
int vox_coroutine_promise_get_status(const vox_coroutine_promise_t* promise);

/**
 * 获取Promise的结果数据
 * @param promise Promise指针
 * @return 返回结果数据（仅在Promise完成后有效）
 */
void* vox_coroutine_promise_get_result(const vox_coroutine_promise_t* promise);

/* ===== 宏定义 ===== */

/**
 * 定义协程入口函数
 * @param name 函数名
 * @param user_data_type 用户数据类型
 * 
 * 示例：
 * VOX_COROUTINE_ENTRY(my_coroutine, int* value) {
 *     // 协程代码
 *     int val = *value;
 *     // ...
 * }
 */
#define VOX_COROUTINE_ENTRY(name, user_data_type) \
    static void name(vox_coroutine_t* co, void* user_data)

/**
 * 启动协程
 * @param loop 事件循环
 * @param entry 协程入口函数
 * @param user_data 用户数据
 */
#define VOX_COROUTINE_START(loop, entry, user_data) \
    do { \
        vox_coroutine_t* co = vox_coroutine_create(loop, entry, user_data, 0); \
        if (co) vox_coroutine_resume(co); \
    } while(0)

/**
 * await Promise（在协程中使用）
 * @param co 协程指针（通常从协程入口函数参数获取）
 * @param promise Promise指针
 * @return Promise的状态码（0=成功，非0=错误）
 */
#define VOX_COROUTINE_AWAIT(co, promise) \
    vox_coroutine_await(co, promise)

/**
 * 启动池化协程
 * @param loop 事件循环
 * @param pool 协程池
 * @param entry 协程入口函数
 * @param user_data 用户数据
 */
#define VOX_COROUTINE_START_POOLED(loop, pool, entry, user_data) \
    do { \
        vox_coroutine_t* co = vox_coroutine_create_pooled(loop, pool, entry, user_data); \
        if (co) vox_coroutine_resume(co); \
    } while(0)

#ifdef __cplusplus
}
#endif

#endif /* VOX_COROUTINE_H */
