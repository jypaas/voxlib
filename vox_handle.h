/*
 * vox_handle.h - 句柄系统
 * 提供异步操作句柄的基类和生命周期管理
 */

#ifndef VOX_HANDLE_H
#define VOX_HANDLE_H

#include "vox_loop.h"
#include "vox_list.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
#ifndef VOX_HANDLE_T_DEFINED
#define VOX_HANDLE_T_DEFINED
typedef struct vox_handle vox_handle_t;
#endif

/* 句柄类型 */
typedef enum {
    VOX_HANDLE_UNKNOWN = 0,
    VOX_HANDLE_TIMER,      /* 定时器句柄 */
    VOX_HANDLE_TCP,        /* TCP 句柄 */
    VOX_HANDLE_UDP,        /* UDP 句柄 */
    VOX_HANDLE_TLS,        /* TLS 句柄 */
    VOX_HANDLE_DTLS,       /* DTLS 句柄 */
    VOX_HANDLE_PIPE,       /* 管道句柄 */
    VOX_HANDLE_FILE,       /* 文件句柄 */
    VOX_HANDLE_PROCESS,    /* 进程句柄 */
    VOX_HANDLE_IDLE,       /* 空闲句柄 */
    VOX_HANDLE_PREPARE,    /* 准备句柄 */
    VOX_HANDLE_CHECK,      /* 检查句柄 */
    VOX_HANDLE_ASYNC,      /* 异步句柄 */
    VOX_HANDLE_POLL,       /* Poll 句柄 */
    VOX_HANDLE_SIGNAL,     /* 信号句柄 */
    VOX_HANDLE_FS_EVENT,   /* 文件系统事件句柄 */
    VOX_HANDLE_FS_POLL,    /* 文件系统轮询句柄 */
    VOX_HANDLE_DNS,        /* DNS 解析句柄 */
    VOX_HANDLE_COROUTINE,  /* 协程句柄 */
    VOX_HANDLE_MAX
} vox_handle_type_t;

/* 关闭回调函数类型 */
typedef void (*vox_handle_close_cb)(vox_handle_t* handle);

/* 句柄基类结构 */
struct vox_handle {
    /* 句柄类型 */
    vox_handle_type_t type;
    
    /* 所属事件循环 */
    vox_loop_t* loop;
    
    /* 用户数据 */
    void* data;
    
    /* 关闭回调 */
    vox_handle_close_cb close_cb;
    
    /* 生命周期管理 */
    uint32_t ref_count;        /* 引用计数 */
    bool closing;              /* 是否正在关闭 */
    bool active;               /* 是否活跃（在活跃句柄列表中） */
    
    /* 链表节点（用于活跃句柄列表） */
    vox_list_node_t node;
    
    /* 内部标志 */
    uint32_t flags;            /* 内部标志位 */
};

/**
 * 初始化句柄
 * @param handle 句柄指针
 * @param type 句柄类型
 * @param loop 事件循环指针
 * @return 成功返回0，失败返回-1
 */
int vox_handle_init(vox_handle_t* handle, vox_handle_type_t type, vox_loop_t* loop);

/**
 * 增加句柄引用计数
 * @param handle 句柄指针
 * @return 返回新的引用计数
 */
uint32_t vox_handle_ref(vox_handle_t* handle);

/**
 * 减少句柄引用计数
 * @param handle 句柄指针
 * @return 返回新的引用计数
 */
uint32_t vox_handle_unref(vox_handle_t* handle);

/**
 * 检查句柄是否活跃
 * @param handle 句柄指针
 * @return 活跃返回true，否则返回false
 */
bool vox_handle_is_active(const vox_handle_t* handle);

/**
 * 检查句柄是否正在关闭
 * @param handle 句柄指针
 * @return 正在关闭返回true，否则返回false
 */
bool vox_handle_is_closing(const vox_handle_t* handle);

/**
 * 关闭句柄（延迟关闭，在回调中真正关闭）
 * @param handle 句柄指针
 * @param close_cb 关闭回调函数，可以为NULL
 * @return 成功返回0，失败返回-1
 */
int vox_handle_close(vox_handle_t* handle, vox_handle_close_cb close_cb);

/**
 * 将句柄添加到活跃句柄列表
 * @param handle 句柄指针
 * @return 成功返回0，失败返回-1
 */
int vox_handle_activate(vox_handle_t* handle);

/**
 * 将句柄从活跃句柄列表移除
 * @param handle 句柄指针
 * @return 成功返回0，失败返回-1
 */
int vox_handle_deactivate(vox_handle_t* handle);

/**
 * 获取句柄类型
 * @param handle 句柄指针
 * @return 返回句柄类型
 */
vox_handle_type_t vox_handle_get_type(const vox_handle_t* handle);

/**
 * 获取句柄所属的事件循环
 * @param handle 句柄指针
 * @return 返回事件循环指针
 */
vox_loop_t* vox_handle_get_loop(const vox_handle_t* handle);

/**
 * 设置句柄的用户数据
 * @param handle 句柄指针
 * @param data 用户数据
 */
void vox_handle_set_data(vox_handle_t* handle, void* data);

/**
 * 获取句柄的用户数据
 * @param handle 句柄指针
 * @return 返回用户数据
 */
void* vox_handle_get_data(const vox_handle_t* handle);

/**
 * 获取句柄的引用计数
 * @param handle 句柄指针
 * @return 返回引用计数
 */
uint32_t vox_handle_get_ref_count(const vox_handle_t* handle);

/* ===== 内部函数（供其他模块使用） ===== */

/**
 * 处理关闭的句柄（在事件循环迭代结束时调用）
 * @param loop 事件循环指针
 */
void vox_handle_process_closing(vox_loop_t* loop);

#ifdef __cplusplus
}
#endif

#endif /* VOX_HANDLE_H */
