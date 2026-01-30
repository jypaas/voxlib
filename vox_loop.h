/*
 * vox_loop.h - 事件循环核心
 * 提供类似 libuv 的事件循环接口
 */

#ifndef VOX_LOOP_H
#define VOX_LOOP_H

#include "vox_os.h"
#include "vox_mpool.h"
#include "vox_queue.h"
#include "vox_mheap.h"
#include "vox_list.h"
#include "vox_time.h"
#include "vox_backend.h"
#include "vox_tpool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 事件循环不透明类型 */
#ifndef VOX_LOOP_T_DEFINED
#define VOX_LOOP_T_DEFINED
typedef struct vox_loop vox_loop_t;
#endif

/* 事件循环运行模式 */
typedef enum {
    VOX_RUN_DEFAULT = 0,    /* 默认模式，运行直到没有活跃句柄 */
    VOX_RUN_ONCE,          /* 运行一次迭代 */
    VOX_RUN_NOWAIT         /* 运行一次迭代，不等待 */
} vox_run_mode_t;

/* 回调函数类型 */
typedef void (*vox_loop_cb)(vox_loop_t* loop, void* user_data);

/* 事件循环配置 */
typedef struct {
    /* 内存池配置 */
    vox_mpool_t* mpool;                    /* 内存池指针，NULL表示内部创建 */
    vox_mpool_config_t* mpool_config;      /* 内存池配置，NULL表示使用默认配置 */
    
    /* 队列配置 */
    vox_queue_config_t* pending_events_config;      /* 待处理事件队列配置，NULL表示使用默认配置 */
    vox_queue_config_t* pending_callbacks_config;   /* 待执行回调队列配置，NULL表示使用默认配置 */
    
    /* Backend 配置 */
    vox_backend_config_t* backend_config;   /* Backend 配置，NULL表示使用默认配置 */
    
    /* 线程池配置 */
    vox_tpool_config_t* tpool_config;       /* 线程池配置，NULL表示使用默认配置 */
} vox_loop_config_t;

/**
 * 创建事件循环
 * @return 成功返回事件循环指针，失败返回NULL
 */
vox_loop_t* vox_loop_create(void);

/**
 * 使用配置创建事件循环
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回事件循环指针，失败返回NULL
 */
vox_loop_t* vox_loop_create_with_config(const vox_loop_config_t* config);

/**
 * 运行事件循环
 * @param loop 事件循环指针
 * @param mode 运行模式
 * @return 成功返回0，失败返回-1
 */
int vox_loop_run(vox_loop_t* loop, vox_run_mode_t mode);

/**
 * 停止事件循环
 * @param loop 事件循环指针
 */
void vox_loop_stop(vox_loop_t* loop);

/**
 * 检查事件循环是否正在运行
 * @param loop 事件循环指针
 * @return 正在运行返回true，否则返回false
 */
bool vox_loop_is_running(const vox_loop_t* loop);

/**
 * 检查事件循环是否已停止
 * @param loop 事件循环指针
 * @return 已停止返回true，否则返回false
 */
bool vox_loop_is_stopped(const vox_loop_t* loop);

/**
 * 获取事件循环的活跃句柄数量
 * @param loop 事件循环指针
 * @return 返回活跃句柄数量
 */
size_t vox_loop_active_handles(const vox_loop_t* loop);

/**
 * 增加事件循环引用（协程 await 时调用，防止 loop 在异步完成前退出）
 * @param loop 事件循环指针
 */
void vox_loop_ref(vox_loop_t* loop);

/**
 * 减少事件循环引用（协程恢复回调中调用）
 * @param loop 事件循环指针
 */
void vox_loop_unref(vox_loop_t* loop);

/**
 * 获取事件循环的内存池
 * @param loop 事件循环指针
 * @return 返回内存池指针
 */
vox_mpool_t* vox_loop_get_mpool(vox_loop_t* loop);

/**
 * 获取当前循环时间（微秒）
 * @param loop 事件循环指针
 * @return 返回当前循环时间
 */
uint64_t vox_loop_now(const vox_loop_t* loop);

/**
 * 更新循环时间（通常在每次迭代开始时调用）
 * @param loop 事件循环指针
 */
void vox_loop_update_time(vox_loop_t* loop);

/**
 * 销毁事件循环
 * @param loop 事件循环指针
 */
void vox_loop_destroy(vox_loop_t* loop);

/* ===== 回调队列 ===== */

/**
 * 在事件循环的下一次迭代中执行回调
 * @param loop 事件循环指针
 * @param cb 回调函数
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_loop_queue_work(vox_loop_t* loop, vox_loop_cb cb, void* user_data);

/**
 * 立即执行回调（在当前迭代中）
 * @param loop 事件循环指针
 * @param cb 回调函数
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_loop_queue_work_immediate(vox_loop_t* loop, vox_loop_cb cb, void* user_data);

/* ===== 内部函数（供其他模块使用） ===== */

/* 获取定时器堆（内部使用） */
vox_mheap_t* vox_loop_get_timers(vox_loop_t* loop);

/* 获取活跃句柄列表（内部使用，供 vox_handle 使用） */
vox_list_t* vox_loop_get_active_handles(vox_loop_t* loop);

/* 获取关闭句柄列表（内部使用，供 vox_handle 使用） */
vox_list_t* vox_loop_get_closing_handles(vox_loop_t* loop);

/* 增加活跃句柄计数（内部使用） */
void vox_loop_increment_active_handles(vox_loop_t* loop);

/* 减少活跃句柄计数（内部使用） */
void vox_loop_decrement_active_handles(vox_loop_t* loop);

/* 获取 backend（内部使用，供 vox_tcp 等使用） */
vox_backend_t* vox_loop_get_backend(vox_loop_t* loop);

/* 获取线程池（内部使用，供 vox_dns、vox_fs 等使用） */
vox_tpool_t* vox_loop_get_thread_pool(vox_loop_t* loop);

#ifdef __cplusplus
}
#endif

#endif /* VOX_LOOP_H */
