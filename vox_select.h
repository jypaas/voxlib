/*
 * vox_select.h - select backend 实现（跨平台兜底方案）
 * 提供基于 select 的异步 IO，作为其他 backend 不可用时的兜底方案
 */

#ifndef VOX_SELECT_H
#define VOX_SELECT_H

#include "vox_os.h"
#include "vox_mpool.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* select backend 不透明类型 */
typedef struct vox_select vox_select_t;

/* IO 事件回调函数类型 */
typedef void (*vox_select_event_cb)(vox_select_t* select, int fd, uint32_t events, void* user_data);

/* select 配置 */
typedef struct {
    vox_mpool_t* mpool;         /* 内存池，如果为NULL则内部创建 */
    size_t max_events;          /* 每次 select 的最大事件数，0表示使用默认值（受 FD_SETSIZE 限制） */
} vox_select_config_t;

/**
 * 创建 select backend
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回 select 指针，失败返回 NULL
 */
vox_select_t* vox_select_create(const vox_select_config_t* config);

/**
 * 初始化 select
 * @param select select 指针
 * @return 成功返回0，失败返回-1
 */
int vox_select_init(vox_select_t* select);

/**
 * 销毁 select
 * @param select select 指针
 */
void vox_select_destroy(vox_select_t* select);

/**
 * 添加文件描述符
 * @param select select 指针
 * @param fd 文件描述符
 * @param events 关注的事件
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_select_add(vox_select_t* select, int fd, uint32_t events, void* user_data);

/**
 * 修改文件描述符
 * @param select select 指针
 * @param fd 文件描述符
 * @param events 新的事件
 * @return 成功返回0，失败返回-1
 */
int vox_select_modify(vox_select_t* select, int fd, uint32_t events);

/**
 * 移除文件描述符
 * @param select select 指针
 * @param fd 文件描述符
 * @return 成功返回0，失败返回-1
 */
int vox_select_remove(vox_select_t* select, int fd);

/**
 * 等待 IO 事件
 * @param select select 指针
 * @param timeout_ms 超时时间（毫秒）
 * @param event_cb 事件回调函数
 * @return 成功返回处理的事件数量，失败返回-1
 */
int vox_select_poll(vox_select_t* select, int timeout_ms, vox_select_event_cb event_cb);

/**
 * 唤醒 select（用于中断 select 等待）
 * @param select select 指针
 * @return 成功返回0，失败返回-1
 */
int vox_select_wakeup(vox_select_t* select);

#ifdef __cplusplus
}
#endif

#endif /* VOX_SELECT_H */
