/*
 * vox_file.h - 跨平台文件操作抽象API
 * 提供统一的文件读写、文件信息、目录操作等接口
 */

#ifndef VOX_FILE_H
#define VOX_FILE_H

#include "vox_os.h"
#include "vox_mpool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 文件不透明类型 */
typedef struct vox_file vox_file_t;

/* 文件打开模式 */
typedef enum {
    VOX_FILE_MODE_READ = 0,      /* 只读 */
    VOX_FILE_MODE_WRITE,         /* 只写（创建新文件，如果存在则截断） */
    VOX_FILE_MODE_APPEND,        /* 追加（如果不存在则创建） */
    VOX_FILE_MODE_READ_WRITE,    /* 读写（如果不存在则创建） */
    VOX_FILE_MODE_READ_APPEND    /* 读追加（如果不存在则创建） */
} vox_file_mode_t;

/* 文件定位方式 */
typedef enum {
    VOX_FILE_SEEK_SET = 0,  /* 从文件开头 */
    VOX_FILE_SEEK_CUR,      /* 从当前位置 */
    VOX_FILE_SEEK_END       /* 从文件末尾 */
} vox_file_seek_t;

/* 文件信息结构 */
typedef struct {
    bool exists;            /* 文件是否存在 */
    bool is_directory;      /* 是否为目录 */
    bool is_regular_file;   /* 是否为普通文件 */
    int64_t size;          /* 文件大小（字节） */
    int64_t modified_time; /* 修改时间（Unix时间戳，秒） */
    int64_t accessed_time; /* 访问时间（Unix时间戳，秒） */
    int64_t created_time;  /* 创建时间（Unix时间戳，秒） */
} vox_file_info_t;

/* 目录遍历回调函数类型 */
typedef int (*vox_file_walk_callback_t)(const char* path, const vox_file_info_t* info, void* user_data);

/**
 * 打开文件
 * @param mpool 内存池指针，必须非NULL
 * @param path 文件路径
 * @param mode 打开模式
 * @return 成功返回文件指针，失败返回NULL
 */
vox_file_t* vox_file_open(vox_mpool_t* mpool, const char* path, vox_file_mode_t mode);

/**
 * 关闭文件
 * @param file 文件指针
 * @return 成功返回0，失败返回-1
 */
int vox_file_close(vox_file_t* file);

/**
 * 读取文件数据
 * @param file 文件指针
 * @param buffer 缓冲区
 * @param size 要读取的字节数
 * @return 成功返回实际读取的字节数，失败返回-1，到达文件末尾返回0
 */
int64_t vox_file_read(vox_file_t* file, void* buffer, size_t size);

/**
 * 写入文件数据
 * @param file 文件指针
 * @param buffer 数据缓冲区
 * @param size 要写入的字节数
 * @return 成功返回实际写入的字节数，失败返回-1
 */
int64_t vox_file_write(vox_file_t* file, const void* buffer, size_t size);

/**
 * 刷新文件缓冲区
 * @param file 文件指针
 * @return 成功返回0，失败返回-1
 */
int vox_file_flush(vox_file_t* file);

/**
 * 文件定位
 * @param file 文件指针
 * @param offset 偏移量
 * @param whence 定位方式
 * @return 成功返回新的文件位置，失败返回-1
 */
int64_t vox_file_seek(vox_file_t* file, int64_t offset, vox_file_seek_t whence);

/**
 * 获取当前文件位置
 * @param file 文件指针
 * @return 成功返回当前位置，失败返回-1
 */
int64_t vox_file_tell(vox_file_t* file);

/**
 * 获取文件大小
 * @param file 文件指针
 * @return 成功返回文件大小，失败返回-1
 */
int64_t vox_file_size(vox_file_t* file);

/**
 * 检查文件是否存在
 * @param path 文件路径
 * @return 存在返回true，不存在返回false
 */
bool vox_file_exists(const char* path);

/**
 * 获取文件信息
 * @param path 文件路径
 * @param info 输出文件信息结构，可为NULL
 * @return 成功返回0，失败返回-1
 */
int vox_file_stat(const char* path, vox_file_info_t* info);

/**
 * 删除文件
 * @param mpool 内存池指针，必须非NULL
 * @param path 文件路径
 * @return 成功返回0，失败返回-1
 */
int vox_file_remove(vox_mpool_t* mpool, const char* path);

/**
 * 重命名文件
 * @param mpool 内存池指针，必须非NULL
 * @param old_path 旧路径
 * @param new_path 新路径
 * @return 成功返回0，失败返回-1
 */
int vox_file_rename(vox_mpool_t* mpool, const char* old_path, const char* new_path);

/**
 * 复制文件
 * @param mpool 内存池指针，必须非NULL
 * @param src_path 源文件路径
 * @param dst_path 目标文件路径
 * @return 成功返回0，失败返回-1
 */
int vox_file_copy(vox_mpool_t* mpool, const char* src_path, const char* dst_path);

/**
 * 创建目录
 * @param mpool 内存池指针，必须非NULL
 * @param path 目录路径
 * @param recursive 是否递归创建父目录
 * @return 成功返回0，失败返回-1
 */
int vox_file_mkdir(vox_mpool_t* mpool, const char* path, bool recursive);

/**
 * 删除目录
 * @param mpool 内存池指针，必须非NULL
 * @param path 目录路径
 * @param recursive 是否递归删除子目录和文件
 * @return 成功返回0，失败返回-1
 */
int vox_file_rmdir(vox_mpool_t* mpool, const char* path, bool recursive);

/**
 * 遍历目录
 * @param mpool 内存池指针，必须非NULL
 * @param path 目录路径
 * @param callback 回调函数，返回非0值停止遍历
 * @param user_data 用户数据指针
 * @return 成功返回遍历的文件数量，失败返回-1
 */
int vox_file_walk(vox_mpool_t* mpool, const char* path, vox_file_walk_callback_t callback, void* user_data);

/**
 * 读取整个文件到内存
 * @param mpool 内存池指针，必须非NULL
 * @param path 文件路径
 * @param size 输出文件大小，可为NULL
 * @return 成功返回数据指针（需要调用vox_mpool_free释放），失败返回NULL
 */
void* vox_file_read_all(vox_mpool_t* mpool, const char* path, size_t* size);

/**
 * 写入整个缓冲区到文件
 * @param mpool 内存池指针，必须非NULL
 * @param path 文件路径
 * @param data 数据缓冲区
 * @param size 数据大小
 * @return 成功返回0，失败返回-1
 */
int vox_file_write_all(vox_mpool_t* mpool, const char* path, const void* data, size_t size);

/**
 * 获取当前工作目录
 * @param mpool 内存池指针，必须非NULL
 * @return 成功返回路径字符串（需要调用vox_mpool_free释放），失败返回NULL
 */
char* vox_file_getcwd(vox_mpool_t* mpool);

/**
 * 更改当前工作目录
 * @param mpool 内存池指针，必须非NULL
 * @param path 目录路径
 * @return 成功返回0，失败返回-1
 */
int vox_file_chdir(vox_mpool_t* mpool, const char* path);

/**
 * 获取路径分隔符
 * @return 返回路径分隔符字符
 */
char vox_file_separator(void);

/**
 * 连接路径
 * @param mpool 内存池指针，必须非NULL
 * @param path1 路径1
 * @param path2 路径2
 * @return 成功返回连接后的路径（需要调用vox_mpool_free释放），失败返回NULL
 */
char* vox_file_join(vox_mpool_t* mpool, const char* path1, const char* path2);

/**
 * 规范化路径（移除多余的路径分隔符和.、..）
 * @param mpool 内存池指针，必须非NULL
 * @param path 路径
 * @return 成功返回规范化后的路径（需要调用vox_mpool_free释放），失败返回NULL
 */
char* vox_file_normalize(vox_mpool_t* mpool, const char* path);

/**
 * 获取文件扩展名
 * @param path 文件路径
 * @return 返回扩展名指针（指向path中的位置），没有扩展名返回NULL
 */
const char* vox_file_ext(const char* path);

/**
 * 获取文件名（不含路径）
 * @param path 文件路径
 * @return 返回文件名指针（指向path中的位置）
 */
const char* vox_file_basename(const char* path);

/**
 * 获取目录名（不含文件名）
 * @param mpool 内存池指针，必须非NULL
 * @param path 文件路径
 * @return 成功返回目录路径（需要调用vox_mpool_free释放），失败返回NULL
 */
char* vox_file_dirname(vox_mpool_t* mpool, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* VOX_FILE_H */
