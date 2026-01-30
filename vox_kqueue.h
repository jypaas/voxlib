/*
 * vox_kqueue.h - macOS/BSD kqueue backend 实现
 * 提供基于 kqueue 的高性能异步 IO
 */

#ifndef VOX_KQUEUE_H
#define VOX_KQUEUE_H

#include "vox_os.h"
#include "vox_mpool.h"
#include <stdint.h>
#include <stdbool.h>

#if defined(VOX_OS_MACOS) || defined(VOX_OS_BSD)

#ifdef __cplusplus
extern "C" {
#endif

/* kqueue backend 不透明类型 */
typedef struct vox_kqueue vox_kqueue_t;

/* IO 事件回调函数类型 */
typedef void (*vox_kqueue_event_cb)(vox_kqueue_t* kqueue, int fd, uint32_t events, void* user_data);

/* kqueue 配置 */
typedef struct {
    vox_mpool_t* mpool;         /* 内存池，如果为NULL则内部创建 */
    size_t max_events;          /* 每次 kevent 的最大事件数，0表示使用默认值256 */
} vox_kqueue_config_t;

/**
 * 创建 kqueue backend
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回 kqueue 指针，失败返回 NULL
 */
vox_kqueue_t* vox_kqueue_create(const vox_kqueue_config_t* config);

/**
 * 初始化 kqueue
 * @param kqueue kqueue 指针
 * @return 成功返回0，失败返回-1
 */
int vox_kqueue_init(vox_kqueue_t* kqueue);

/**
 * 销毁 kqueue
 * @param kqueue kqueue 指针
 */
void vox_kqueue_destroy(vox_kqueue_t* kqueue);

/**
 * 添加文件描述符
 * @param kqueue kqueue 指针
 * @param fd 文件描述符
 * @param events 关注的事件
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_kqueue_add(vox_kqueue_t* kqueue, int fd, uint32_t events, void* user_data);

/**
 * 修改文件描述符
 * @param kqueue kqueue 指针
 * @param fd 文件描述符
 * @param events 新的事件
 * @return 成功返回0，失败返回-1
 */
int vox_kqueue_modify(vox_kqueue_t* kqueue, int fd, uint32_t events);

/**
 * 移除文件描述符
 * @param kqueue kqueue 指针
 * @param fd 文件描述符
 * @return 成功返回0，失败返回-1
 */
int vox_kqueue_remove(vox_kqueue_t* kqueue, int fd);

/**
 * 等待 IO 事件
 * @param kqueue kqueue 指针
 * @param timeout_ms 超时时间（毫秒）
 * @param event_cb 事件回调函数
 * @return 成功返回处理的事件数量，失败返回-1
 */
int vox_kqueue_poll(vox_kqueue_t* kqueue, int timeout_ms, vox_kqueue_event_cb event_cb);

/**
 * 唤醒 kqueue（用于中断 kevent）
 * @param kqueue kqueue 指针
 * @return 成功返回0，失败返回-1
 */
int vox_kqueue_wakeup(vox_kqueue_t* kqueue);

#ifdef __cplusplus
}
#endif

#endif /* VOX_OS_MACOS || VOX_OS_BSD */

#endif /* VOX_KQUEUE_H */
