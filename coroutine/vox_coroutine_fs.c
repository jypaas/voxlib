/*
 * vox_coroutine_fs.c - 文件系统协程适配器实现
 */

#include "vox_coroutine_fs.h"
#include "../vox_log.h"
#include "../vox_mpool.h"
#include <string.h>

/* 内部状态结构 */
typedef struct {
    vox_coroutine_promise_t* promise;
    int status;
    ssize_t nread;
    vox_file_info_t file_info;
} vox_coroutine_fs_state_t;

/* ===== 回调函数 ===== */

static void fs_open_cb(vox_fs_t* fs, int status, void* user_data) {
    (void)fs;
    vox_coroutine_fs_state_t* state = (vox_coroutine_fs_state_t*)user_data;
    state->status = status;
    vox_coroutine_promise_complete(state->promise, status, NULL);
}

static void fs_read_cb(vox_fs_t* fs, ssize_t nread, const void* buf, void* user_data) {
    (void)fs;
    vox_coroutine_fs_state_t* state = (vox_coroutine_fs_state_t*)user_data;
    state->nread = nread;
    state->status = (nread >= 0) ? 0 : -1;
    vox_coroutine_promise_complete(state->promise, state->status, (void*)buf);
}

static void fs_write_cb(vox_fs_t* fs, int status, void* user_data) {
    (void)fs;
    vox_coroutine_fs_state_t* state = (vox_coroutine_fs_state_t*)user_data;
    state->status = status;
    vox_coroutine_promise_complete(state->promise, status, NULL);
}

static void fs_close_cb(vox_fs_t* fs, int status, void* user_data) {
    (void)fs;
    vox_coroutine_fs_state_t* state = (vox_coroutine_fs_state_t*)user_data;
    state->status = status;
    vox_coroutine_promise_complete(state->promise, status, NULL);
}

static void fs_stat_cb(vox_fs_t* fs, int status, const vox_file_info_t* info, void* user_data) {
    (void)fs;
    vox_coroutine_fs_state_t* state = (vox_coroutine_fs_state_t*)user_data;
    state->status = status;
    if (status == 0 && info) {
        memcpy(&state->file_info, info, sizeof(vox_file_info_t));
    }
    vox_coroutine_promise_complete(state->promise, status, NULL);
}

/* ===== 协程适配实现 ===== */

int vox_coroutine_fs_open_await(vox_coroutine_t* co,
                                 vox_fs_t* fs,
                                 const char* path,
                                 vox_file_mode_t mode) {
    if (!co || !fs || !path) {
        return -1;
    }

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    vox_coroutine_fs_state_t state = {0};
    state.promise = vox_coroutine_promise_create(loop);
    if (!state.promise) {
        return -1;
    }

    /* 设置用户数据 */
    fs->handle.data = &state;

    /* 发起异步打开操作 */
    if (vox_fs_open(fs, path, mode, fs_open_cb) < 0) {
        vox_coroutine_promise_destroy(state.promise);
        return -1;
    }

    /* 等待Promise完成 */
    int ret = vox_coroutine_await(co, state.promise);
    
    vox_coroutine_promise_destroy(state.promise);
    return (ret == 0 && state.status == 0) ? 0 : -1;
}

int vox_coroutine_fs_read_await(vox_coroutine_t* co,
                                 vox_fs_t* fs,
                                 void* buf,
                                 size_t len,
                                 int64_t offset,
                                 ssize_t* out_nread) {
    if (!co || !fs || !buf) {
        return -1;
    }

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    vox_coroutine_fs_state_t state = {0};
    state.promise = vox_coroutine_promise_create(loop);
    if (!state.promise) {
        return -1;
    }

    /* 设置用户数据 */
    fs->handle.data = &state;

    /* 发起异步读取操作 */
    if (vox_fs_read(fs, buf, len, offset, fs_read_cb) < 0) {
        vox_coroutine_promise_destroy(state.promise);
        return -1;
    }

    /* 等待Promise完成 */
    int ret = vox_coroutine_await(co, state.promise);
    
    if (out_nread) {
        *out_nread = state.nread;
    }

    vox_coroutine_promise_destroy(state.promise);
    return (ret == 0 && state.status == 0) ? 0 : -1;
}

int vox_coroutine_fs_write_await(vox_coroutine_t* co,
                                  vox_fs_t* fs,
                                  const void* buf,
                                  size_t len,
                                  int64_t offset) {
    if (!co || !fs || !buf) {
        return -1;
    }

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    vox_coroutine_fs_state_t state = {0};
    state.promise = vox_coroutine_promise_create(loop);
    if (!state.promise) {
        return -1;
    }

    /* 设置用户数据 */
    fs->handle.data = &state;

    /* 发起异步写入操作 */
    if (vox_fs_write(fs, buf, len, offset, fs_write_cb) < 0) {
        vox_coroutine_promise_destroy(state.promise);
        return -1;
    }

    /* 等待Promise完成 */
    int ret = vox_coroutine_await(co, state.promise);
    
    vox_coroutine_promise_destroy(state.promise);
    return (ret == 0 && state.status == 0) ? 0 : -1;
}

int vox_coroutine_fs_close_await(vox_coroutine_t* co,
                                  vox_fs_t* fs) {
    if (!co || !fs) {
        return -1;
    }

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    vox_coroutine_fs_state_t state = {0};
    state.promise = vox_coroutine_promise_create(loop);
    if (!state.promise) {
        return -1;
    }

    /* 设置用户数据 */
    fs->handle.data = &state;

    /* 发起异步关闭操作 */
    if (vox_fs_close(fs, fs_close_cb) < 0) {
        vox_coroutine_promise_destroy(state.promise);
        return -1;
    }

    /* 等待Promise完成 */
    int ret = vox_coroutine_await(co, state.promise);
    
    vox_coroutine_promise_destroy(state.promise);
    return (ret == 0 && state.status == 0) ? 0 : -1;
}

int vox_coroutine_fs_stat_await(vox_coroutine_t* co,
                                 vox_fs_t* fs,
                                 const char* path,
                                 vox_file_info_t* out_info) {
    if (!co || !fs) {
        return -1;
    }

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    vox_coroutine_fs_state_t state = {0};
    state.promise = vox_coroutine_promise_create(loop);
    if (!state.promise) {
        return -1;
    }

    /* 设置用户数据 */
    fs->handle.data = &state;

    /* 发起异步stat操作 */
    if (vox_fs_stat(fs, path, fs_stat_cb) < 0) {
        vox_coroutine_promise_destroy(state.promise);
        return -1;
    }

    /* 等待Promise完成 */
    int ret = vox_coroutine_await(co, state.promise);
    
    if (ret == 0 && state.status == 0 && out_info) {
        memcpy(out_info, &state.file_info, sizeof(vox_file_info_t));
    }

    vox_coroutine_promise_destroy(state.promise);
    return (ret == 0 && state.status == 0) ? 0 : -1;
}

/* ===== 便捷函数实现 ===== */

int vox_coroutine_fs_read_file_await(vox_coroutine_t* co,
                                      const char* path,
                                      void** out_data,
                                      size_t* out_size) {
    if (!co || !path || !out_data || !out_size) {
        return -1;
    }

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    vox_fs_t* fs = vox_fs_create(loop);
    if (!fs) {
        return -1;
    }

    /* 打开文件 */
    if (vox_coroutine_fs_open_await(co, fs, path, VOX_FILE_MODE_READ) < 0) {
        vox_fs_destroy(fs);
        return -1;
    }

    /* 获取文件大小 */
    vox_file_info_t info;
    if (vox_coroutine_fs_stat_await(co, fs, NULL, &info) < 0) {
        vox_coroutine_fs_close_await(co, fs);
        vox_fs_destroy(fs);
        return -1;
    }

    /* 获取内存池 */
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) {
        vox_coroutine_fs_close_await(co, fs);
        vox_fs_destroy(fs);
        return -1;
    }

    /* 分配缓冲区（使用内存池） */
    void* buffer = vox_mpool_alloc(mpool, info.size);
    if (!buffer) {
        vox_coroutine_fs_close_await(co, fs);
        vox_fs_destroy(fs);
        return -1;
    }

    /* 读取文件 */
    ssize_t nread;
    if (vox_coroutine_fs_read_await(co, fs, buffer, info.size, 0, &nread) < 0) {
        vox_mpool_free(mpool, buffer);
        vox_coroutine_fs_close_await(co, fs);
        vox_fs_destroy(fs);
        return -1;
    }

    /* 关闭文件 */
    vox_coroutine_fs_close_await(co, fs);
    vox_fs_destroy(fs);

    *out_data = buffer;
    *out_size = nread;
    return 0;
}

int vox_coroutine_fs_write_file_await(vox_coroutine_t* co,
                                       const char* path,
                                       const void* data,
                                       size_t size) {
    if (!co || !path || !data) {
        return -1;
    }

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    vox_fs_t* fs = vox_fs_create(loop);
    if (!fs) {
        return -1;
    }

    /* 打开文件（WRITE模式会创建新文件或截断已存在的文件） */
    if (vox_coroutine_fs_open_await(co, fs, path, VOX_FILE_MODE_WRITE) < 0) {
        vox_fs_destroy(fs);
        return -1;
    }

    /* 写入文件 */
    if (vox_coroutine_fs_write_await(co, fs, data, size, 0) < 0) {
        vox_coroutine_fs_close_await(co, fs);
        vox_fs_destroy(fs);
        return -1;
    }

    /* 关闭文件 */
    vox_coroutine_fs_close_await(co, fs);
    vox_fs_destroy(fs);

    return 0;
}

/* 释放通过vox_coroutine_fs_read_file_await分配的文件数据 */
void vox_coroutine_fs_free_file_data(vox_coroutine_t* co, void* data) {
    if (!co || !data) {
        return;
    }

    vox_loop_t* loop = vox_coroutine_get_loop(co);
    if (!loop) {
        return;
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (mpool) {
        vox_mpool_free(mpool, data);
    }
}
