/*
 * vox_fs.h - 文件异步操作
 * 提供类似 libuv 的文件异步接口
 */

#ifndef VOX_FS_H
#define VOX_FS_H

#include "vox_handle.h"
#include "vox_file.h"
#include "vox_os.h"
#include <stdint.h>
#include <stdbool.h>
#ifndef VOX_OS_WINDOWS
    #include <sys/types.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
typedef struct vox_fs vox_fs_t;

/* 文件打开回调函数类型 */
typedef void (*vox_fs_open_cb)(vox_fs_t* fs, int status, void* user_data);

/* 文件读取回调函数类型 */
typedef void (*vox_fs_read_cb)(vox_fs_t* fs, ssize_t nread, const void* buf, void* user_data);

/* 文件写入回调函数类型 */
typedef void (*vox_fs_write_cb)(vox_fs_t* fs, int status, void* user_data);

/* 文件关闭回调函数类型 */
typedef void (*vox_fs_close_cb)(vox_fs_t* fs, int status, void* user_data);

/* 文件信息回调函数类型 */
typedef void (*vox_fs_stat_cb)(vox_fs_t* fs, int status, const vox_file_info_t* info, void* user_data);

/* 文件句柄结构 */
struct vox_fs {
    /* 句柄基类（必须作为第一个成员） */
    vox_handle_t handle;
    
    /* 文件句柄（同步API） */
    vox_file_t* file;
    
    /* 文件路径 */
    char* path;
    
    /* 回调函数 */
    vox_fs_open_cb open_cb;
    vox_fs_read_cb read_cb;
    vox_fs_write_cb write_cb;
    vox_fs_close_cb close_cb;
    vox_fs_stat_cb stat_cb;
    
    /* 状态 */
    bool opened;                    /* 是否已打开 */
    
    /* 内部状态 */
    void* read_buf;                 /* 读取缓冲区 */
    size_t read_buf_size;           /* 读取缓冲区大小 */
    void* write_buf;                 /* 写入缓冲区 */
    size_t write_buf_size;          /* 写入缓冲区大小 */
    
    /* 平台特定数据 */
    void* platform_data;            /* 平台特定的数据（io_uring请求等） */
};

/**
 * 初始化文件句柄
 * @param fs 文件句柄指针
 * @param loop 事件循环指针
 * @return 成功返回0，失败返回-1
 */
int vox_fs_init(vox_fs_t* fs, vox_loop_t* loop);

/**
 * 使用内存池创建文件句柄
 * @param loop 事件循环指针
 * @return 成功返回文件句柄指针，失败返回 NULL
 */
vox_fs_t* vox_fs_create(vox_loop_t* loop);

/**
 * 销毁文件句柄
 * @param fs 文件句柄指针
 */
void vox_fs_destroy(vox_fs_t* fs);

/**
 * 异步打开文件
 * @param fs 文件句柄指针
 * @param path 文件路径
 * @param mode 打开模式
 * @param cb 打开完成回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_fs_open(vox_fs_t* fs, const char* path, vox_file_mode_t mode, vox_fs_open_cb cb);

/**
 * 异步读取文件
 * @param fs 文件句柄指针
 * @param buf 数据缓冲区
 * @param len 要读取的字节数
 * @param offset 文件偏移量（-1表示从当前位置读取）
 * @param cb 读取完成回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_fs_read(vox_fs_t* fs, void* buf, size_t len, int64_t offset, vox_fs_read_cb cb);

/**
 * 异步写入文件
 * @param fs 文件句柄指针
 * @param buf 数据缓冲区
 * @param len 要写入的字节数
 * @param offset 文件偏移量（-1表示从当前位置写入）
 * @param cb 写入完成回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_fs_write(vox_fs_t* fs, const void* buf, size_t len, int64_t offset, vox_fs_write_cb cb);

/**
 * 异步关闭文件
 * @param fs 文件句柄指针
 * @param cb 关闭完成回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_fs_close(vox_fs_t* fs, vox_fs_close_cb cb);

/**
 * 异步获取文件信息
 * @param fs 文件句柄指针
 * @param path 文件路径（可以为NULL，使用fs->path）
 * @param cb 获取信息完成回调函数
 * @return 成功返回0，失败返回-1
 */
int vox_fs_stat(vox_fs_t* fs, const char* path, vox_fs_stat_cb cb);

/* ===== 便捷函数 ===== */

/**
 * 便捷函数：异步打开并读取整个文件
 * @param loop 事件循环指针
 * @param path 文件路径
 * @param mode 打开模式（通常为 VOX_FILE_MODE_READ）
 * @param cb 读取完成回调函数（nread为读取的字节数，buf为数据缓冲区）
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 * 
 * @note 此函数自动创建文件句柄，读取完成后自动关闭并销毁句柄
 * @note 缓冲区在回调函数中有效，回调返回后可能被释放
 * @note 如果需要在回调外使用数据，需要复制缓冲区
 */
int vox_fs_read_file(vox_loop_t* loop,
                     const char* path,
                     vox_file_mode_t mode,
                     vox_fs_read_cb cb,
                     void* user_data);

/**
 * 便捷函数：异步写入整个缓冲区到文件
 * @param loop 事件循环指针
 * @param path 文件路径
 * @param mode 打开模式（通常为 VOX_FILE_MODE_WRITE 或 VOX_FILE_MODE_CREATE）
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @param cb 写入完成回调函数
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 * 
 * @note 此函数自动创建文件句柄，写入完成后自动关闭并销毁句柄
 */
int vox_fs_write_file(vox_loop_t* loop,
                      const char* path,
                      vox_file_mode_t mode,
                      const void* buf,
                      size_t len,
                      vox_fs_write_cb cb,
                      void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* VOX_FS_H */
