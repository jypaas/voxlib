/*
 * vox_epoll.h - Linux epoll backend 实现
 * 提供基于 epoll 的高性能异步 IO
 */

#ifndef VOX_EPOLL_H
#define VOX_EPOLL_H

#include "vox_os.h"
#include "vox_mpool.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef VOX_OS_LINUX

#ifdef __cplusplus
extern "C" {
#endif

/* epoll backend 不透明类型 */
typedef struct vox_epoll vox_epoll_t;

/* IO 事件回调函数类型 */
typedef void (*vox_epoll_event_cb)(vox_epoll_t* epoll, int fd, uint32_t events, void* user_data);

/* epoll 配置 */
typedef struct {
    vox_mpool_t* mpool;         /* 内存池，如果为NULL则内部创建 */
    size_t max_events;          /* 每次 epoll_wait 的最大事件数，0表示使用默认值256 */
} vox_epoll_config_t;

/**
 * 创建 epoll backend
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回 epoll 指针，失败返回 NULL
 */
vox_epoll_t* vox_epoll_create(const vox_epoll_config_t* config);

/**
 * 初始化 epoll
 * @param epoll epoll 指针
 * @return 成功返回0，失败返回-1
 */
int vox_epoll_init(vox_epoll_t* epoll);

/**
 * 销毁 epoll
 * @param epoll epoll 指针
 */
void vox_epoll_destroy(vox_epoll_t* epoll);

/**
 * 添加文件描述符
 * @param epoll epoll 指针
 * @param fd 文件描述符
 * @param events 关注的事件
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_epoll_add(vox_epoll_t* epoll, int fd, uint32_t events, void* user_data);

/**
 * 修改文件描述符
 * @param epoll epoll 指针
 * @param fd 文件描述符
 * @param events 新的事件
 * @return 成功返回0，失败返回-1
 */
int vox_epoll_modify(vox_epoll_t* epoll, int fd, uint32_t events);

/**
 * 移除文件描述符
 * @param epoll epoll 指针
 * @param fd 文件描述符
 * @return 成功返回0，失败返回-1
 */
int vox_epoll_remove(vox_epoll_t* epoll, int fd);

/**
 * 等待 IO 事件
 * @param epoll epoll 指针
 * @param timeout_ms 超时时间（毫秒）
 * @param event_cb 事件回调函数
 * @return 成功返回处理的事件数量，失败返回-1
 */
int vox_epoll_poll(vox_epoll_t* epoll, int timeout_ms, vox_epoll_event_cb event_cb);

/**
 * 唤醒 epoll（用于中断 epoll_wait）
 * @param epoll epoll 指针
 * @return 成功返回0，失败返回-1
 */
int vox_epoll_wakeup(vox_epoll_t* epoll);

#ifdef __cplusplus
}
#endif

#endif /* VOX_OS_LINUX */

#endif /* VOX_EPOLL_H */
