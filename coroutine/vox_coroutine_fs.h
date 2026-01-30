/*
 * vox_coroutine_fs.h - 文件系统协程适配器
 * 提供async/await风格的协程API，避免回调地狱
 */

#ifndef VOX_COROUTINE_FS_H
#define VOX_COROUTINE_FS_H

#include "../vox_fs.h"
#include "vox_coroutine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 协程适配接口 ===== */

/**
 * 在协程中异步打开文件
 * @param co 协程指针
 * @param fs 文件句柄指针
 * @param path 文件路径
 * @param mode 打开模式
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_fs_open_await(vox_coroutine_t* co,
                                 vox_fs_t* fs,
                                 const char* path,
                                 vox_file_mode_t mode);

/**
 * 在协程中异步读取文件
 * @param co 协程指针
 * @param fs 文件句柄指针
 * @param buf 数据缓冲区
 * @param len 要读取的字节数
 * @param offset 文件偏移量（-1表示从当前位置读取）
 * @param out_nread 输出实际读取的字节数
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_fs_read_await(vox_coroutine_t* co,
                                 vox_fs_t* fs,
                                 void* buf,
                                 size_t len,
                                 int64_t offset,
                                 ssize_t* out_nread);

/**
 * 在协程中异步写入文件
 * @param co 协程指针
 * @param fs 文件句柄指针
 * @param buf 数据缓冲区
 * @param len 要写入的字节数
 * @param offset 文件偏移量（-1表示从当前位置写入）
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_fs_write_await(vox_coroutine_t* co,
                                  vox_fs_t* fs,
                                  const void* buf,
                                  size_t len,
                                  int64_t offset);

/**
 * 在协程中异步关闭文件
 * @param co 协程指针
 * @param fs 文件句柄指针
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_fs_close_await(vox_coroutine_t* co,
                                  vox_fs_t* fs);

/**
 * 在协程中异步获取文件信息
 * @param co 协程指针
 * @param fs 文件句柄指针
 * @param path 文件路径（可以为NULL，使用fs->path）
 * @param out_info 输出文件信息（需要调用者分配空间）
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_fs_stat_await(vox_coroutine_t* co,
                                 vox_fs_t* fs,
                                 const char* path,
                                 vox_file_info_t* out_info);

/* ===== 便捷函数 ===== */

/**
 * 在协程中读取整个文件
 * @param co 协程指针
 * @param path 文件路径
 * @param out_data 输出数据缓冲区指针（使用内存池分配，需要调用者通过vox_coroutine_fs_free_file_data释放）
 * @param out_size 输出数据大小
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_fs_read_file_await(vox_coroutine_t* co,
                                      const char* path,
                                      void** out_data,
                                      size_t* out_size);

/**
 * 释放通过vox_coroutine_fs_read_file_await分配的文件数据
 * @param co 协程指针（用于获取loop和内存池）
 * @param data 要释放的数据指针
 */
void vox_coroutine_fs_free_file_data(vox_coroutine_t* co, void* data);

/**
 * 在协程中写入整个文件
 * @param co 协程指针
 * @param path 文件路径
 * @param data 数据缓冲区
 * @param size 数据大小
 * @return 成功返回0，失败返回-1
 */
int vox_coroutine_fs_write_file_await(vox_coroutine_t* co,
                                       const char* path,
                                       const void* data,
                                       size_t size);

#ifdef __cplusplus
}
#endif

#endif /* VOX_COROUTINE_FS_H */
