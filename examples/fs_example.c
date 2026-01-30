/*
 * fs_example.c - 异步文件操作示例程序
 * 演示 vox_fs 的异步文件操作功能
 */

#include "../vox_loop.h"
#include "../vox_fs.h"
#include "../vox_mpool.h"
#include "../vox_log.h"
#include "../vox_os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* 全局变量 */
static vox_loop_t* g_loop = NULL;
static int g_pending_ops = 0;

/* 操作上下文 */
typedef struct {
    const char* operation;
    void* data;
} fs_ctx_t;

/* 信号处理 */
static void signal_handler(int sig) {
    (void)sig;
    if (g_loop) {
        printf("\n收到信号，停止事件循环...\n");
        vox_loop_stop(g_loop);
    }
}

/* 文件打开回调 */
static void on_file_open(vox_fs_t* fs, int status, void* user_data) {
    fs_ctx_t* ctx = (fs_ctx_t*)user_data;
    
    printf("[%s] 文件打开完成, status=%d\n", ctx->operation, status);
    
    if (status != 0) {
        printf("[%s] 文件打开失败\n", ctx->operation);
        g_pending_ops--;
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        vox_fs_destroy(fs);
        return;
    }
    
    printf("[%s] 文件已成功打开\n", ctx->operation);
}

/* 文件读取回调 */
static void on_file_read(vox_fs_t* fs, ssize_t nread, const void* buf, void* user_data) {
    fs_ctx_t* ctx = (fs_ctx_t*)user_data;
    
    printf("[%s] 文件读取完成, nread=%zd\n", ctx->operation, nread);
    
    if (nread < 0) {
        printf("[%s] 文件读取错误\n", ctx->operation);
        g_pending_ops--;
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        vox_fs_close(fs, NULL);
        vox_fs_destroy(fs);
        return;
    }
    
    if (nread == 0) {
        printf("[%s] 文件读取完成（到达文件末尾）\n", ctx->operation);
    } else {
        printf("[%s] 读取内容 (前 %zd 字节):\n", ctx->operation, nread);
        /* 打印前100个字符 */
        size_t print_len = (size_t)nread < 100 ? (size_t)nread : 100;
        for (size_t i = 0; i < print_len; i++) {
            char c = ((const char*)buf)[i];
            if (c >= 32 && c < 127) {
                printf("%c", c);
            } else {
                printf(".");
            }
        }
        if ((size_t)nread > 100) {
            printf("...");
        }
        printf("\n");
    }
    
    g_pending_ops--;
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    vox_mpool_free(mpool, ctx);
    vox_fs_close(fs, NULL);
    vox_fs_destroy(fs);
}

/* 文件写入回调 */
static void on_file_write(vox_fs_t* fs, int status, void* user_data) {
    fs_ctx_t* ctx = (fs_ctx_t*)user_data;
    
    printf("[%s] 文件写入完成, status=%d\n", ctx->operation, status);
    
    if (status != 0) {
        printf("[%s] 文件写入失败\n", ctx->operation);
    } else {
        printf("[%s] 文件写入成功\n", ctx->operation);
    }
    
    g_pending_ops--;
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    vox_mpool_free(mpool, ctx);
    vox_fs_close(fs, NULL);
    vox_fs_destroy(fs);
}

/* 文件关闭回调 */
static void on_file_close(vox_fs_t* fs, int status, void* user_data) {
    (void)fs;
    (void)user_data;
    
    if (status != 0) {
        printf("文件关闭失败, status=%d\n", status);
    } else {
        printf("文件已关闭\n");
    }
}

/* 文件信息回调 */
static void on_file_stat(vox_fs_t* fs, int status, const vox_file_info_t* info, void* user_data) {
    fs_ctx_t* ctx = (fs_ctx_t*)user_data;
    
    printf("[%s] 文件信息获取完成, status=%d\n", ctx->operation, status);
    
    if (status != 0) {
        printf("[%s] 获取文件信息失败\n", ctx->operation);
    } else if (info) {
        printf("[%s] 文件信息:\n", ctx->operation);
        printf("  存在: %s\n", info->exists ? "是" : "否");
        printf("  是目录: %s\n", info->is_directory ? "是" : "否");
        printf("  是普通文件: %s\n", info->is_regular_file ? "是" : "否");
        printf("  文件大小: %lld 字节\n", (long long)info->size);
        printf("  修改时间: %lld\n", (long long)info->modified_time);
        printf("  访问时间: %lld\n", (long long)info->accessed_time);
        printf("  创建时间: %lld\n", (long long)info->created_time);
    }
    
    g_pending_ops--;
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    vox_mpool_free(mpool, ctx);
    vox_fs_destroy(fs);
}

/* 异步读取文件示例（旧版本，保留作为参考） */
static int VOX_UNUSED_FUNC async_read_file(const char* path) {
    printf("\n=== 异步读取文件: %s ===\n", path);
    
    /* 创建文件句柄 */
    vox_fs_t* fs = vox_fs_create(g_loop);
    if (!fs) {
        fprintf(stderr, "创建文件句柄失败\n");
        return -1;
    }
    
    /* 分配上下文 */
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    fs_ctx_t* ctx = (fs_ctx_t*)vox_mpool_alloc(mpool, sizeof(fs_ctx_t));
    if (!ctx) {
        vox_fs_destroy(fs);
        return -1;
    }
    
    ctx->operation = "读取";
    ctx->data = NULL;
    
    /* 设置回调 */
    fs->read_cb = on_file_read;
    
    /* 分配读取缓冲区 */
    size_t buf_size = 4096;
    void* buf = vox_mpool_alloc(mpool, buf_size);
    if (!buf) {
        vox_mpool_free(mpool, ctx);
        vox_fs_destroy(fs);
        return -1;
    }
    
    /* 异步打开文件 */
    if (vox_fs_open(fs, path, VOX_FILE_MODE_READ, on_file_open) != 0) {
        fprintf(stderr, "启动异步打开文件失败\n");
        vox_mpool_free(mpool, buf);
        vox_mpool_free(mpool, ctx);
        vox_fs_destroy(fs);
        return -1;
    }
    
    /* 等待文件打开后，在回调中启动读取 */
    /* 为了简化，我们在打开回调中启动读取 */
    /* 但这里我们需要修改回调逻辑，所以先设置一个标志 */
    
    /* 实际上，我们需要在打开回调中启动读取 */
    /* 为了演示，我们使用一个简化的方式：先打开，然后在回调中读取 */
    
    /* 保存上下文到文件句柄的用户数据 */
    vox_handle_set_data((vox_handle_t*)fs, ctx);
    
    /* 保存缓冲区指针 */
    fs->read_buf = buf;
    fs->read_buf_size = buf_size;
    
    g_pending_ops++;
    return 0;
}

/* 异步写入文件示例（旧版本，保留作为参考） */
static int VOX_UNUSED_FUNC async_write_file(const char* path, const char* content) {
    printf("\n=== 异步写入文件: %s ===\n", path);
    
    /* 创建文件句柄 */
    vox_fs_t* fs = vox_fs_create(g_loop);
    if (!fs) {
        fprintf(stderr, "创建文件句柄失败\n");
        return -1;
    }
    
    /* 分配上下文 */
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    fs_ctx_t* ctx = (fs_ctx_t*)vox_mpool_alloc(mpool, sizeof(fs_ctx_t));
    if (!ctx) {
        vox_fs_destroy(fs);
        return -1;
    }
    
    ctx->operation = "写入";
    ctx->data = NULL;
    
    /* 设置回调 */
    fs->write_cb = on_file_write;
    fs->close_cb = on_file_close;
    
    /* 复制写入内容 */
    size_t content_len = strlen(content);
    void* write_buf = vox_mpool_alloc(mpool, content_len);
    if (!write_buf) {
        vox_mpool_free(mpool, ctx);
        vox_fs_destroy(fs);
        return -1;
    }
    memcpy(write_buf, content, content_len);
    
    /* 保存上下文 */
    vox_handle_set_data((vox_handle_t*)fs, ctx);
    
    /* 保存写入缓冲区 */
    fs->write_buf = write_buf;
    fs->write_buf_size = content_len;
    
    /* 异步打开文件（写入模式） */
    if (vox_fs_open(fs, path, VOX_FILE_MODE_WRITE, on_file_open) != 0) {
        fprintf(stderr, "启动异步打开文件失败\n");
        vox_mpool_free(mpool, write_buf);
        vox_mpool_free(mpool, ctx);
        vox_fs_destroy(fs);
        return -1;
    }
    
    /* 在打开回调中启动写入 */
    /* 为了简化，我们修改打开回调来启动写入 */
    
    g_pending_ops++;
    return 0;
}

/* 异步获取文件信息示例 */
static int async_stat_file(const char* path) {
    printf("\n=== 异步获取文件信息: %s ===\n", path);
    
    /* 创建文件句柄 */
    vox_fs_t* fs = vox_fs_create(g_loop);
    if (!fs) {
        fprintf(stderr, "创建文件句柄失败\n");
        return -1;
    }
    
    /* 分配上下文 */
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    fs_ctx_t* ctx = (fs_ctx_t*)vox_mpool_alloc(mpool, sizeof(fs_ctx_t));
    if (!ctx) {
        vox_fs_destroy(fs);
        return -1;
    }
    
    ctx->operation = "获取信息";
    ctx->data = NULL;
    
    /* 设置回调 */
    fs->stat_cb = on_file_stat;
    
    /* 保存上下文 */
    vox_handle_set_data((vox_handle_t*)fs, ctx);
    
    /* 异步获取文件信息 */
    if (vox_fs_stat(fs, path, on_file_stat) != 0) {
        fprintf(stderr, "启动异步获取文件信息失败\n");
        vox_mpool_free(mpool, ctx);
        vox_fs_destroy(fs);
        return -1;
    }
    
    g_pending_ops++;
    return 0;
}

/* 改进的打开回调 - 在打开后自动启动读取 */
static void on_file_open_and_read(vox_fs_t* fs, int status, void* user_data) {
    fs_ctx_t* ctx = (fs_ctx_t*)user_data;
    
    printf("[%s] 文件打开完成, status=%d\n", ctx->operation, status);
    
    if (status != 0) {
        printf("[%s] 文件打开失败\n", ctx->operation);
        g_pending_ops--;
        vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
        vox_mpool_free(mpool, ctx);
        vox_fs_destroy(fs);
        return;
    }
    
    /* 文件打开成功，启动读取 */
    if (strcmp(ctx->operation, "读取") == 0) {
        printf("[%s] 启动异步读取...\n", ctx->operation);
        if (vox_fs_read(fs, fs->read_buf, fs->read_buf_size, -1, on_file_read) != 0) {
            printf("[%s] 启动异步读取失败\n", ctx->operation);
            g_pending_ops--;
            vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
            vox_mpool_free(mpool, ctx);
            vox_fs_close(fs, NULL);
            vox_fs_destroy(fs);
        }
    } else if (strcmp(ctx->operation, "写入") == 0) {
        printf("[%s] 启动异步写入...\n", ctx->operation);
        if (vox_fs_write(fs, fs->write_buf, fs->write_buf_size, -1, on_file_write) != 0) {
            printf("[%s] 启动异步写入失败\n", ctx->operation);
            g_pending_ops--;
            vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
            vox_mpool_free(mpool, ctx);
            vox_fs_close(fs, NULL);
            vox_fs_destroy(fs);
        }
    }
}

/* 改进的异步读取文件 */
static int async_read_file_improved(const char* path) {
    printf("\n=== 异步读取文件: %s ===\n", path);
    
    vox_fs_t* fs = vox_fs_create(g_loop);
    if (!fs) {
        fprintf(stderr, "创建文件句柄失败\n");
        return -1;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    fs_ctx_t* ctx = (fs_ctx_t*)vox_mpool_alloc(mpool, sizeof(fs_ctx_t));
    if (!ctx) {
        vox_fs_destroy(fs);
        return -1;
    }
    
    ctx->operation = "读取";
    ctx->data = NULL;
    
    fs->read_cb = on_file_read;
    fs->close_cb = on_file_close;
    
    size_t buf_size = 4096;
    void* buf = vox_mpool_alloc(mpool, buf_size);
    if (!buf) {
        vox_mpool_free(mpool, ctx);
        vox_fs_destroy(fs);
        return -1;
    }
    
    fs->read_buf = buf;
    fs->read_buf_size = buf_size;
    vox_handle_set_data((vox_handle_t*)fs, ctx);
    
    if (vox_fs_open(fs, path, VOX_FILE_MODE_READ, on_file_open_and_read) != 0) {
        fprintf(stderr, "启动异步打开文件失败\n");
        vox_mpool_free(mpool, buf);
        vox_mpool_free(mpool, ctx);
        vox_fs_destroy(fs);
        return -1;
    }
    
    g_pending_ops++;
    return 0;
}

/* 改进的异步写入文件 */
static int async_write_file_improved(const char* path, const char* content) {
    printf("\n=== 异步写入文件: %s ===\n", path);
    
    vox_fs_t* fs = vox_fs_create(g_loop);
    if (!fs) {
        fprintf(stderr, "创建文件句柄失败\n");
        return -1;
    }
    
    vox_mpool_t* mpool = vox_loop_get_mpool(g_loop);
    fs_ctx_t* ctx = (fs_ctx_t*)vox_mpool_alloc(mpool, sizeof(fs_ctx_t));
    if (!ctx) {
        vox_fs_destroy(fs);
        return -1;
    }
    
    ctx->operation = "写入";
    ctx->data = NULL;
    
    fs->write_cb = on_file_write;
    fs->close_cb = on_file_close;
    
    size_t content_len = strlen(content);
    void* write_buf = vox_mpool_alloc(mpool, content_len);
    if (!write_buf) {
        vox_mpool_free(mpool, ctx);
        vox_fs_destroy(fs);
        return -1;
    }
    memcpy(write_buf, content, content_len);
    
    fs->write_buf = write_buf;
    fs->write_buf_size = content_len;
    vox_handle_set_data((vox_handle_t*)fs, ctx);
    
    if (vox_fs_open(fs, path, VOX_FILE_MODE_WRITE, on_file_open_and_read) != 0) {
        fprintf(stderr, "启动异步打开文件失败\n");
        vox_mpool_free(mpool, write_buf);
        vox_mpool_free(mpool, ctx);
        vox_fs_destroy(fs);
        return -1;
    }
    
    g_pending_ops++;
    return 0;
}

int main(int argc, char* argv[]) {
    printf("=== vox_fs 异步文件操作示例 ===\n");
    printf("演示异步文件打开、读取、写入和获取信息\n\n");
    
    /* 创建事件循环 */
    g_loop = vox_loop_create();
    if (!g_loop) {
        fprintf(stderr, "创建事件循环失败\n");
        return 1;
    }
    
    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* 测试文件路径 */
    const char* test_file = "test_async_file.txt";
    
    /* 1. 先写入一个测试文件 */
    const char* test_content = "Hello, Async File I/O!\n"
                               "This is a test file for vox_fs.\n"
                               "Line 3: Testing asynchronous file operations.\n"
                               "Line 4: The file operations are non-blocking.\n";
    
    printf("步骤 1: 异步写入测试文件\n");
    if (async_write_file_improved(test_file, test_content) != 0) {
        fprintf(stderr, "异步写入文件失败\n");
        vox_loop_destroy(g_loop);
        return 1;
    }
    
    /* 等待写入完成 */
    while (g_pending_ops > 0) {
        vox_loop_run(g_loop, VOX_RUN_ONCE);
    }
    
    /* 2. 异步读取文件 */
    printf("\n步骤 2: 异步读取文件\n");
    if (async_read_file_improved(test_file) != 0) {
        fprintf(stderr, "异步读取文件失败\n");
        vox_loop_destroy(g_loop);
        return 1;
    }
    
    /* 等待读取完成 */
    while (g_pending_ops > 0) {
        vox_loop_run(g_loop, VOX_RUN_ONCE);
    }
    
    /* 3. 异步获取文件信息 */
    printf("\n步骤 3: 异步获取文件信息\n");
    if (async_stat_file(test_file) != 0) {
        fprintf(stderr, "异步获取文件信息失败\n");
        vox_loop_destroy(g_loop);
        return 1;
    }
    
    /* 等待获取信息完成 */
    while (g_pending_ops > 0) {
        vox_loop_run(g_loop, VOX_RUN_ONCE);
    }
    
    /* 4. 如果提供了命令行参数，尝试读取该文件 */
    if (argc > 1 && strcmp(argv[1], test_file) != 0) {
        printf("\n步骤 4: 异步读取指定文件: %s\n", argv[1]);
        if (async_read_file_improved(argv[1]) != 0) {
            printf("警告: 无法读取文件 %s\n", argv[1]);
        } else {
            while (g_pending_ops > 0) {
                vox_loop_run(g_loop, VOX_RUN_ONCE);
            }
        }
    }
    
    printf("\n=== 所有异步操作完成 ===\n");
    printf("按 Ctrl+C 退出，或等待事件循环结束\n");
    
    /* 运行事件循环直到停止 */
    vox_loop_run(g_loop, VOX_RUN_DEFAULT);
    
    /* 清理 */
    vox_loop_destroy(g_loop);
    
    printf("\n程序退出\n");
    return 0;
}
