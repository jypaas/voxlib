/*
 * vox_timer.h - 定时器系统
 * 使用最小堆实现高效的定时器管理
 */

#ifndef VOX_TIMER_H
#define VOX_TIMER_H

#include "vox_loop.h"
#include "vox_os.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 定时器结构前向声明 */
typedef struct vox_timer vox_timer_t;

/* 定时器回调函数类型 */
typedef void (*vox_timer_cb)(vox_timer_t* timer, void* user_data);

/* 定时器结构 */
struct vox_timer {
    vox_loop_t* loop;            /* 所属事件循环 */
    uint64_t timeout;            /* 到期时间（微秒） */
    uint64_t repeat;             /* 重复间隔（微秒），0表示不重复 */
    vox_timer_cb callback;       /* 回调函数 */
    void* user_data;             /* 用户数据 */
    bool active;                 /* 是否活跃 */
} ;

/**
 * 初始化定时器
 * @param timer 定时器指针（外部分配）
 * @param loop 事件循环指针
 * @return 成功返回0，失败返回-1
 */
int vox_timer_init(vox_timer_t* timer, vox_loop_t* loop);

/**
 * 启动定时器
 * @param timer 定时器指针
 * @param timeout_ms 超时时间（毫秒）
 * @param repeat_ms 重复间隔（毫秒），0表示不重复
 * @param cb 回调函数
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_timer_start(vox_timer_t* timer, uint64_t timeout_ms, 
                    uint64_t repeat_ms, vox_timer_cb cb, void* user_data);

/**
 * 停止定时器
 * @param timer 定时器指针
 * @return 成功返回0，失败返回-1
 */
int vox_timer_stop(vox_timer_t* timer);

/**
 * 重启定时器（使用上次的配置）
 * @param timer 定时器指针
 * @return 成功返回0，失败返回-1
 */
int vox_timer_again(vox_timer_t* timer);

/**
 * 检查定时器是否活跃
 * @param timer 定时器指针
 * @return 活跃返回true，否则返回false
 */
bool vox_timer_is_active(const vox_timer_t* timer);

/**
 * 获取定时器的重复间隔
 * @param timer 定时器指针
 * @return 返回重复间隔（毫秒），0表示不重复
 */
uint64_t vox_timer_get_repeat(const vox_timer_t* timer);

/**
 * 设置定时器的重复间隔
 * @param timer 定时器指针
 * @param repeat_ms 重复间隔（毫秒），0表示不重复
 * @return 成功返回0，失败返回-1
 */
int vox_timer_set_repeat(vox_timer_t* timer, uint64_t repeat_ms);

/**
 * 销毁定时器（清理资源，不释放内存）
 * @param timer 定时器指针
 */
void vox_timer_destroy(vox_timer_t* timer);

/* ===== 内部函数（供事件循环使用） ===== */

/**
 * 处理到期的定时器（内部函数，由事件循环调用）
 * @param loop 事件循环指针
 */
void vox_timer_process_expired(vox_loop_t* loop);

#ifdef __cplusplus
}
#endif

#endif /* VOX_TIMER_H */
