/*
 * vox_coroutine_promise.h - 协程Promise机制
 * 用于协程等待异步操作完成
 */

#ifndef VOX_COROUTINE_PROMISE_H
#define VOX_COROUTINE_PROMISE_H

#include "../vox_os.h"
#include "../vox_loop.h"
#include "../vox_mutex.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_coroutine_promise vox_coroutine_promise_t;

/* Promise结构（在.c文件中定义） */
struct vox_coroutine_promise {
    /* 所属事件循环 */
    vox_loop_t* loop;
    
    /* 状态 */
    bool completed;
    int status;              /* 状态码（0=成功，非0=错误） */
    void* result;            /* 结果数据 */
    
    /* 同步原语 */
    vox_mutex_t mutex;       /* 保护状态 */
    vox_event_t event;       /* 用于等待完成 */
    
    /* 等待此Promise的协程 */
    void* waiting_coroutine;  /* vox_coroutine_t*，避免循环依赖 */
};

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

#ifdef __cplusplus
}
#endif

#endif /* VOX_COROUTINE_PROMISE_H */
