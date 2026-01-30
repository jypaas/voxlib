/*
 * vox_fs.c - 文件异步操作实现
 */

#include "vox_fs.h"
#include "vox_loop.h"
#include "vox_file.h"
#include "vox_mpool.h"
#include "vox_tpool.h"
#include <string.h>
#include <stdlib.h>

#ifdef VOX_OS_LINUX
    #ifdef VOX_USE_IOURING
        #include "vox_uring.h"
    #endif
#endif

/* 获取文件IO线程池（从事件循环中获取） */
static vox_tpool_t* get_fs_thread_pool(vox_loop_t* loop) {
    if (!loop) {
        return NULL;
    }
    return vox_loop_get_thread_pool(loop);
}

/* 文件操作请求结构 */
typedef struct vox_fs_req {
    vox_fs_t* fs;
    void* user_data;
    union {
        struct {
            const char* path;
            vox_file_mode_t mode;
            vox_fs_open_cb cb;
        } open;
        struct {
            void* buf;
            size_t len;
            int64_t offset;
            vox_fs_read_cb cb;
        } read;
        struct {
            const void* buf;
            size_t len;
            int64_t offset;
            vox_fs_write_cb cb;
        } write;
        struct {
            vox_fs_close_cb cb;
        } close;
        struct {
            const char* path;
            vox_fs_stat_cb cb;
        } stat;
    } u;
} vox_fs_req_t;

/* 初始化文件句柄 */
int vox_fs_init(vox_fs_t* fs, vox_loop_t* loop) {
    if (!fs || !loop) {
        return -1;
    }
    
    memset(fs, 0, sizeof(vox_fs_t));
    
    /* 初始化句柄基类 */
    if (vox_handle_init((vox_handle_t*)fs, VOX_HANDLE_FILE, loop) != 0) {
        return -1;
    }
    
    fs->opened = false;
    fs->file = NULL;
    fs->path = NULL;
    
    return 0;
}

/* 创建文件句柄 */
vox_fs_t* vox_fs_create(vox_loop_t* loop) {
    if (!loop) {
        return NULL;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_fs_t* fs = (vox_fs_t*)vox_mpool_alloc(mpool, sizeof(vox_fs_t));
    if (!fs) {
        return NULL;
    }
    
    if (vox_fs_init(fs, loop) != 0) {
        vox_mpool_free(mpool, fs);
        return NULL;
    }
    
    return fs;
}

/* 销毁文件句柄 */
void vox_fs_destroy(vox_fs_t* fs) {
    if (!fs) {
        return;
    }
    
    /* 如果文件已打开，先关闭 */
    if (fs->opened && fs->file) {
        vox_file_close(fs->file);
        fs->file = NULL;
        fs->opened = false;
    }
    
    /* 释放路径 */
    if (fs->path) {
        vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
        vox_mpool_free(mpool, fs->path);
        fs->path = NULL;
    }
    
    /* 释放缓冲区 */
    if (fs->read_buf) {
        vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
        vox_mpool_free(mpool, fs->read_buf);
        fs->read_buf = NULL;
        fs->read_buf_size = 0;
    }
    
    if (fs->write_buf) {
        vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
        vox_mpool_free(mpool, fs->write_buf);
        fs->write_buf = NULL;
        fs->write_buf_size = 0;
    }
    
    /* 关闭句柄 */
    vox_handle_close((vox_handle_t*)fs, NULL);
}

/* 线程池任务：打开文件 */
static void fs_open_task(void* user_data) {
    vox_fs_req_t* req = (vox_fs_req_t*)user_data;
    vox_fs_t* fs = req->fs;
    
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    fs->file = vox_file_open(mpool, req->u.open.path, req->u.open.mode);
    
    if (fs->file) {
        fs->opened = true;
        /* 保存路径 */
        size_t path_len = strlen(req->u.open.path) + 1;
        fs->path = (char*)vox_mpool_alloc(mpool, path_len);
        if (fs->path) {
            memcpy(fs->path, req->u.open.path, path_len);
        }
    }
}

/* 线程池任务完成回调：打开文件 */
static void fs_open_complete(void* user_data, int result) {
    vox_fs_req_t* req = (vox_fs_req_t*)user_data;
    vox_fs_t* fs = req->fs;
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    
    (void)result;  /* 未使用的参数 */
    int status = (fs->opened && fs->file) ? 0 : -1;
    
    /* 调用用户回调 */
    if (req->u.open.cb) {
        req->u.open.cb(fs, status, vox_handle_get_data((vox_handle_t*)fs));
    }
    
    /* 释放请求 */
    vox_mpool_free(mpool, req);
}

/* 异步打开文件 */
int vox_fs_open(vox_fs_t* fs, const char* path, vox_file_mode_t mode, vox_fs_open_cb cb) {
    if (!fs || !path) {
        return -1;
    }
    
    if (fs->opened) {
        return -1;  /* 已经打开 */
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    
    /* 创建请求 */
    vox_fs_req_t* req = (vox_fs_req_t*)vox_mpool_alloc(mpool, sizeof(vox_fs_req_t));
    if (!req) {
        return -1;
    }
    
    req->fs = fs;
    req->u.open.path = path;
    req->u.open.mode = mode;
    req->u.open.cb = cb;
    
    /* 获取线程池 */
    vox_tpool_t* tpool = get_fs_thread_pool(fs->handle.loop);
    if (!tpool) {
        vox_mpool_free(mpool, req);
        return -1;
    }
    
    /* 提交任务 */
    if (vox_tpool_submit(tpool, fs_open_task, req, fs_open_complete) != 0) {
        vox_mpool_free(mpool, req);
        return -1;
    }
    
    /* 激活句柄 */
    vox_handle_activate((vox_handle_t*)fs);
    
    return 0;
}

/* 线程池任务：读取文件 */
VOX_UNUSED_FUNC static void fs_read_task(void* user_data) {
    vox_fs_req_t* req = (vox_fs_req_t*)user_data;
    vox_fs_t* fs = req->fs;
    
    if (!fs->file) {
        return;
    }
    
    /* 如果指定了偏移量，先定位 */
    if (req->u.read.offset >= 0) {
        vox_file_seek(fs->file, req->u.read.offset, VOX_FILE_SEEK_SET);
    }
    
    /* 读取数据 */
    int64_t nread = vox_file_read(fs->file, req->u.read.buf, req->u.read.len);
    req->user_data = (void*)(intptr_t)nread;  /* 临时存储读取的字节数 */
}

/* 线程池任务完成回调：读取文件 */
VOX_UNUSED_FUNC static void fs_read_complete(void* user_data, int result) {
    vox_fs_req_t* req = (vox_fs_req_t*)user_data;
    vox_fs_t* fs = req->fs;
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    
    (void)result;  /* 未使用的参数 */
    ssize_t nread = (ssize_t)(intptr_t)req->user_data;
    
    /* 调用用户回调 */
    if (req->u.read.cb) {
        req->u.read.cb(fs, nread, req->u.read.buf, vox_handle_get_data((vox_handle_t*)fs));
    }
    
    /* 释放请求 */
    vox_mpool_free(mpool, req);
}

/* 异步读取文件 */
int vox_fs_read(vox_fs_t* fs, void* buf, size_t len, int64_t offset, vox_fs_read_cb cb) {
    if (!fs || !buf || len == 0) {
        return -1;
    }
    
    if (!fs->opened || !fs->file) {
        return -1;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    
    /* 创建请求 */
    vox_fs_req_t* req = (vox_fs_req_t*)vox_mpool_alloc(mpool, sizeof(vox_fs_req_t));
    if (!req) {
        return -1;
    }
    
    req->fs = fs;
    req->u.read.buf = buf;
    req->u.read.len = len;
    req->u.read.offset = offset;
    req->u.read.cb = cb;
    
    /* 获取线程池 */
    vox_tpool_t* tpool = get_fs_thread_pool(fs->handle.loop);
    if (!tpool) {
        vox_mpool_free(mpool, req);
        return -1;
    }
    
    /* 提交任务 */
    if (vox_tpool_submit(tpool, fs_read_task, req, fs_read_complete) != 0) {
        vox_mpool_free(mpool, req);
        return -1;
    }
    
    return 0;
}

/* 线程池任务：写入文件 */
static void fs_write_task(void* user_data) {
    vox_fs_req_t* req = (vox_fs_req_t*)user_data;
    vox_fs_t* fs = req->fs;
    
    if (!fs->file) {
        return;
    }
    
    /* 如果指定了偏移量，先定位 */
    if (req->u.write.offset >= 0) {
        vox_file_seek(fs->file, req->u.write.offset, VOX_FILE_SEEK_SET);
    }
    
    /* 写入数据 */
    int64_t nwritten = vox_file_write(fs->file, req->u.write.buf, req->u.write.len);
    req->user_data = (void*)(intptr_t)nwritten;  /* 临时存储写入的字节数 */
}

/* 线程池任务完成回调：写入文件 */
static void fs_write_complete(void* user_data, int result) {
    vox_fs_req_t* req = (vox_fs_req_t*)user_data;
    vox_fs_t* fs = req->fs;
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    
    (void)result;  /* 未使用的参数 */
    int64_t nwritten = (int64_t)(intptr_t)req->user_data;
    int status = (nwritten == (int64_t)req->u.write.len) ? 0 : -1;
    
    /* 调用用户回调 */
    if (req->u.write.cb) {
        req->u.write.cb(fs, status, vox_handle_get_data((vox_handle_t*)fs));
    }
    
    /* 释放请求 */
    vox_mpool_free(mpool, req);
}

/* 异步写入文件 */
int vox_fs_write(vox_fs_t* fs, const void* buf, size_t len, int64_t offset, vox_fs_write_cb cb) {
    if (!fs || !buf || len == 0) {
        return -1;
    }
    
    if (!fs->opened || !fs->file) {
        return -1;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    
    /* 创建请求 */
    vox_fs_req_t* req = (vox_fs_req_t*)vox_mpool_alloc(mpool, sizeof(vox_fs_req_t));
    if (!req) {
        return -1;
    }
    
    req->fs = fs;
    req->u.write.buf = buf;
    req->u.write.len = len;
    req->u.write.offset = offset;
    req->u.write.cb = cb;
    
    /* 获取线程池 */
    vox_tpool_t* tpool = get_fs_thread_pool(fs->handle.loop);
    if (!tpool) {
        vox_mpool_free(mpool, req);
        return -1;
    }
    
    /* 提交任务 */
    if (vox_tpool_submit(tpool, fs_write_task, req, fs_write_complete) != 0) {
        vox_mpool_free(mpool, req);
        return -1;
    }
    
    return 0;
}

/* 线程池任务：关闭文件 */
static void fs_close_task(void* user_data) {
    vox_fs_req_t* req = (vox_fs_req_t*)user_data;
    vox_fs_t* fs = req->fs;
    
    if (fs->file) {
        vox_file_close(fs->file);
        fs->file = NULL;
        fs->opened = false;
    }
}

/* 线程池任务完成回调：关闭文件 */
static void fs_close_complete(void* user_data, int result) {
    vox_fs_req_t* req = (vox_fs_req_t*)user_data;
    vox_fs_t* fs = req->fs;
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    
    (void)result;  /* 未使用的参数 */
    /* 调用用户回调 */
    if (req->u.close.cb) {
        req->u.close.cb(fs, 0, vox_handle_get_data((vox_handle_t*)fs));
    }
    
    /* 释放请求 */
    vox_mpool_free(mpool, req);
}

/* 异步关闭文件 */
int vox_fs_close(vox_fs_t* fs, vox_fs_close_cb cb) {
    if (!fs) {
        return -1;
    }
    
    if (!fs->opened || !fs->file) {
        if (cb) {
            cb(fs, -1, vox_handle_get_data((vox_handle_t*)fs));
        }
        return -1;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    
    /* 创建请求 */
    vox_fs_req_t* req = (vox_fs_req_t*)vox_mpool_alloc(mpool, sizeof(vox_fs_req_t));
    if (!req) {
        return -1;
    }
    
    req->fs = fs;
    req->u.close.cb = cb;
    
    /* 获取线程池 */
    vox_tpool_t* tpool = get_fs_thread_pool(fs->handle.loop);
    if (!tpool) {
        vox_mpool_free(mpool, req);
        return -1;
    }
    
    /* 提交任务 */
    if (vox_tpool_submit(tpool, fs_close_task, req, fs_close_complete) != 0) {
        vox_mpool_free(mpool, req);
        return -1;
    }
    
    return 0;
}

/* 线程池任务：获取文件信息 */
static void fs_stat_task(void* user_data) {
    vox_fs_req_t* req = (vox_fs_req_t*)user_data;
    vox_fs_t* fs = req->fs;
    
    const char* path = req->u.stat.path ? req->u.stat.path : fs->path;
    if (!path) {
        return;
    }
    
    /* 获取文件信息 */
    vox_file_info_t info;
    if (vox_file_stat(path, &info) == 0) {
        /* 保存信息到请求中 */
        vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
        vox_file_info_t* info_ptr = (vox_file_info_t*)vox_mpool_alloc(mpool, sizeof(vox_file_info_t));
        if (info_ptr) {
            *info_ptr = info;
            req->user_data = info_ptr;
        }
    }
}

/* 线程池任务完成回调：获取文件信息 */
static void fs_stat_complete(void* user_data, int result) {
    vox_fs_req_t* req = (vox_fs_req_t*)user_data;
    vox_fs_t* fs = req->fs;
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    
    (void)result;  /* 未使用的参数 */
    vox_file_info_t* info = (vox_file_info_t*)req->user_data;
    int status = info ? 0 : -1;
    
    /* 调用用户回调 */
    if (req->u.stat.cb) {
        req->u.stat.cb(fs, status, info, vox_handle_get_data((vox_handle_t*)fs));
    }
    
    /* 释放信息 */
    if (info) {
        vox_mpool_free(mpool, info);
    }
    
    /* 释放请求 */
    vox_mpool_free(mpool, req);
}

/* 异步获取文件信息 */
int vox_fs_stat(vox_fs_t* fs, const char* path, vox_fs_stat_cb cb) {
    if (!fs || !cb) {
        return -1;
    }
    
    const char* stat_path = path ? path : fs->path;
    if (!stat_path) {
        return -1;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    
    /* 创建请求 */
    vox_fs_req_t* req = (vox_fs_req_t*)vox_mpool_alloc(mpool, sizeof(vox_fs_req_t));
    if (!req) {
        return -1;
    }
    
    req->fs = fs;
    req->u.stat.path = stat_path;
    req->u.stat.cb = cb;
    
    /* 获取线程池 */
    vox_tpool_t* tpool = get_fs_thread_pool(fs->handle.loop);
    if (!tpool) {
        vox_mpool_free(mpool, req);
        return -1;
    }
    
    /* 提交任务 */
    if (vox_tpool_submit(tpool, fs_stat_task, req, fs_stat_complete) != 0) {
        vox_mpool_free(mpool, req);
        return -1;
    }
    
    return 0;
}

/* ===== 便捷函数实现 ===== */

/* 读取文件的内部状态 */
typedef struct {
    vox_fs_t* fs;
    vox_fs_read_cb cb;
    void* user_data;
    void* buf;
    size_t buf_size;
} read_file_state_t;

/* 读取文件完成回调（用于读取整个文件） */
static void read_file_read_cb(vox_fs_t* fs, ssize_t nread, const void* buf, void* user_data);

/* 关闭文件完成回调（用于读取整个文件） */
static void read_file_close_cb(vox_fs_t* fs, int status, void* user_data);

/* 打开文件完成回调（用于读取整个文件） */
static void read_file_open_cb(vox_fs_t* fs, int status, void* user_data) {
    read_file_state_t* state = (read_file_state_t*)user_data;
    if (!state || !fs) {
        return;
    }
    
    if (status != 0) {
        /* 打开失败，调用读取回调（nread为-1表示错误） */
        if (state->cb) {
            state->cb(fs, -1, NULL, state->user_data);
        }
        /* 清理 */
        vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
        vox_mpool_free(mpool, state);
        vox_fs_destroy(fs);
        return;
    }
    
    /* 获取文件大小 - 使用文件句柄获取大小 */
    if (fs->file) {
        int64_t file_size = vox_file_size(fs->file);
        if (file_size > 0) {
            state->buf_size = (size_t)file_size;
        } else {
            /* 如果无法获取大小，使用默认大小 */
            state->buf_size = 64 * 1024;  /* 64KB */
        }
    } else {
        /* 如果文件句柄不存在，使用默认大小 */
        state->buf_size = 64 * 1024;  /* 64KB */
    }
    
    /* 分配缓冲区 */
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    state->buf = vox_mpool_alloc(mpool, state->buf_size);
    if (!state->buf) {
        if (state->cb) {
            state->cb(fs, -1, NULL, state->user_data);
        }
        vox_mpool_free(mpool, state);
        vox_fs_destroy(fs);
        return;
    }
    
    /* 读取整个文件 */
    if (vox_fs_read(fs, state->buf, state->buf_size, 0, read_file_read_cb) != 0) {
        vox_mpool_free(mpool, state->buf);
        vox_mpool_free(mpool, state);
        if (state->cb) {
            state->cb(fs, -1, NULL, state->user_data);
        }
        vox_fs_destroy(fs);
    }
}

/* 读取文件完成回调（用于读取整个文件） */
static void read_file_read_cb(vox_fs_t* fs, ssize_t nread, const void* buf, void* user_data) {
    read_file_state_t* state = (read_file_state_t*)user_data;
    if (!state || !fs) {
        return;
    }
    
    /* 调用用户回调 */
    if (state->cb) {
        state->cb(fs, nread, buf, state->user_data);
    }
    
    /* 关闭并销毁文件句柄 */
    vox_fs_close(fs, read_file_close_cb);
    
    /* 保存状态供关闭回调使用 */
    state->fs = fs;
}

/* 关闭文件完成回调（用于读取整个文件） */
static void read_file_close_cb(vox_fs_t* fs, int status, void* user_data) {
    (void)status;
    read_file_state_t* state = (read_file_state_t*)user_data;
    if (!state || !fs) {
        return;
    }
    
    /* 清理 */
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    if (state->buf) {
        vox_mpool_free(mpool, state->buf);
    }
    vox_mpool_free(mpool, state);
    vox_fs_destroy(fs);
}

int vox_fs_read_file(vox_loop_t* loop,
                     const char* path,
                     vox_file_mode_t mode,
                     vox_fs_read_cb cb,
                     void* user_data) {
    if (!loop || !path || !cb) {
        return -1;
    }
    
    /* 创建文件句柄 */
    vox_fs_t* fs = vox_fs_create(loop);
    if (!fs) {
        return -1;
    }
    
    /* 创建状态 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    read_file_state_t* state = (read_file_state_t*)vox_mpool_alloc(mpool, sizeof(read_file_state_t));
    if (!state) {
        vox_fs_destroy(fs);
        return -1;
    }
    
    state->fs = fs;
    state->cb = cb;
    state->user_data = user_data;
    state->buf = NULL;
    state->buf_size = 0;
    
    /* 打开文件 */
    if (vox_fs_open(fs, path, mode, read_file_open_cb) != 0) {
        vox_mpool_free(mpool, state);
        vox_fs_destroy(fs);
        return -1;
    }
    
    return 0;
}

/* 写入文件的内部状态 */
typedef struct {
    vox_fs_t* fs;
    vox_fs_write_cb cb;
    void* user_data;
    const void* buf;
    size_t len;
} write_file_state_t;

/* 写入文件完成回调（用于写入整个文件） */
static void write_file_write_cb(vox_fs_t* fs, int status, void* user_data);

/* 关闭文件完成回调（用于写入整个文件） */
static void write_file_close_cb(vox_fs_t* fs, int status, void* user_data);

/* 打开文件完成回调（用于写入整个文件） */
static void write_file_open_cb(vox_fs_t* fs, int status, void* user_data) {
    write_file_state_t* state = (write_file_state_t*)user_data;
    if (!state || !fs) {
        return;
    }
    
    if (status != 0) {
        /* 打开失败，调用写入回调 */
        if (state->cb) {
            state->cb(fs, -1, state->user_data);
        }
        /* 清理 */
        vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
        vox_mpool_free(mpool, state);
        vox_fs_destroy(fs);
        return;
    }
    
    /* 写入整个缓冲区 */
    if (vox_fs_write(fs, state->buf, state->len, 0, write_file_write_cb) != 0) {
        vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
        vox_mpool_free(mpool, state);
        if (state->cb) {
            state->cb(fs, -1, state->user_data);
        }
        vox_fs_destroy(fs);
    }
}

/* 写入文件完成回调（用于写入整个文件） */
static void write_file_write_cb(vox_fs_t* fs, int status, void* user_data) {
    write_file_state_t* state = (write_file_state_t*)user_data;
    if (!state || !fs) {
        return;
    }
    
    /* 调用用户回调 */
    if (state->cb) {
        state->cb(fs, status, state->user_data);
    }
    
    /* 关闭并销毁文件句柄 */
    vox_fs_close(fs, write_file_close_cb);
    
    /* 保存状态供关闭回调使用 */
    state->fs = fs;
}

/* 关闭文件完成回调（用于写入整个文件） */
static void write_file_close_cb(vox_fs_t* fs, int status, void* user_data) {
    (void)status;
    write_file_state_t* state = (write_file_state_t*)user_data;
    if (!state || !fs) {
        return;
    }
    
    /* 清理 */
    vox_mpool_t* mpool = vox_loop_get_mpool(fs->handle.loop);
    vox_mpool_free(mpool, state);
    vox_fs_destroy(fs);
}

int vox_fs_write_file(vox_loop_t* loop,
                      const char* path,
                      vox_file_mode_t mode,
                      const void* buf,
                      size_t len,
                      vox_fs_write_cb cb,
                      void* user_data) {
    if (!loop || !path || !buf || len == 0 || !cb) {
        return -1;
    }
    
    /* 创建文件句柄 */
    vox_fs_t* fs = vox_fs_create(loop);
    if (!fs) {
        return -1;
    }
    
    /* 创建状态 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    write_file_state_t* state = (write_file_state_t*)vox_mpool_alloc(mpool, sizeof(write_file_state_t));
    if (!state) {
        vox_fs_destroy(fs);
        return -1;
    }
    
    state->fs = fs;
    state->cb = cb;
    state->user_data = user_data;
    state->buf = buf;
    state->len = len;
    
    /* 打开文件 */
    if (vox_fs_open(fs, path, mode, write_file_open_cb) != 0) {
        vox_mpool_free(mpool, state);
        vox_fs_destroy(fs);
        return -1;
    }
    
    return 0;
}
