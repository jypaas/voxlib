/*
 * vox_uring.h - Linux io_uring backend 实现
 * 提供基于 io_uring 的高性能异步 IO
 */

#ifndef VOX_URING_H
#define VOX_URING_H

#include "vox_os.h"
#include "vox_mpool.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef VOX_OS_LINUX
#ifdef VOX_USE_IOURING

#ifdef __cplusplus
extern "C" {
#endif

/* io_uring backend 不透明类型 */
typedef struct vox_uring vox_uring_t;

/* IO 事件回调函数类型 */
typedef void (*vox_uring_event_cb)(vox_uring_t* uring, int fd, uint32_t events, void* user_data);

/* io_uring 配置 */
typedef struct {
    vox_mpool_t* mpool;         /* 内存池，如果为NULL则内部创建 */
    size_t max_events;          /* 每次 io_uring_wait_cqe 的最大事件数，0表示使用默认值256 */
    unsigned int sq_entries;    /* 提交队列大小，0表示使用默认值256 */
} vox_uring_config_t;

/**
 * 创建 io_uring backend
 * @param config 配置结构体，NULL表示使用默认配置
 * @return 成功返回 uring 指针，失败返回 NULL
 */
vox_uring_t* vox_uring_create(const vox_uring_config_t* config);

/**
 * 初始化 io_uring
 * @param uring uring 指针
 * @return 成功返回0，失败返回-1
 */
int vox_uring_init(vox_uring_t* uring);

/**
 * 销毁 io_uring
 * @param uring uring 指针
 */
void vox_uring_destroy(vox_uring_t* uring);

/**
 * 添加文件描述符
 * @param uring uring 指针
 * @param fd 文件描述符
 * @param events 关注的事件
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int vox_uring_add(vox_uring_t* uring, int fd, uint32_t events, void* user_data);

/**
 * 修改文件描述符
 * @param uring uring 指针
 * @param fd 文件描述符
 * @param events 新的事件
 * @return 成功返回0，失败返回-1
 */
int vox_uring_modify(vox_uring_t* uring, int fd, uint32_t events);

/**
 * 移除文件描述符
 * @param uring uring 指针
 * @param fd 文件描述符
 * @return 成功返回0，失败返回-1
 */
int vox_uring_remove(vox_uring_t* uring, int fd);

/**
 * 等待 IO 事件
 * @param uring uring 指针
 * @param timeout_ms 超时时间（毫秒）
 * @param event_cb 事件回调函数
 * @return 成功返回处理的事件数量，失败返回-1
 */
int vox_uring_poll(vox_uring_t* uring, int timeout_ms, vox_uring_event_cb event_cb);

/**
 * 唤醒 io_uring（用于中断 io_uring_wait_cqe）
 * @param uring uring 指针
 * @return 成功返回0，失败返回-1
 */
int vox_uring_wakeup(vox_uring_t* uring);

#ifdef __cplusplus
}
#endif

#endif /* VOX_USE_IOURING */
#endif /* VOX_OS_LINUX */

#endif /* VOX_URING_H */
