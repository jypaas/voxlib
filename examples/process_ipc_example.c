/*
 * process_ipc_example.c - 进程间通信示例程序
 * 演示 vox_process 的多进程开发功能
 */

#include "../vox_process.h"
#include "../vox_mpool.h"
#include "../vox_os.h"  /* 包含平台特定头文件 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef VOX_OS_WINDOWS
    #ifndef SIGINT
        #define SIGINT 2
    #endif
    #ifndef SIGTERM
        #define SIGTERM 15
    #endif
#else
    #include <unistd.h>
    #include <signal.h>
#endif

/* 测试共享内存 */
void test_shared_memory(void) {
    printf("\n=== 测试共享内存 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    const char* shm_name = "vox_test_shm";
    size_t shm_size = 4096;
    
    /* 创建共享内存 */
    vox_shm_t* shm = vox_shm_create(mpool, shm_name, shm_size, true);
    if (!shm) {
        printf("创建共享内存失败，尝试打开已存在的...\n");
        shm = vox_shm_create(mpool, shm_name, shm_size, false);
        if (!shm) {
            printf("打开共享内存也失败\n");
            vox_mpool_destroy(mpool);
            return;
        }
    }
    
    printf("共享内存创建/打开成功，大小: %zu 字节\n", vox_shm_get_size(shm));
    
    void* ptr = vox_shm_get_ptr(shm);
    if (ptr) {
        /* 写入数据 */
        const char* message = "Hello from shared memory!";
        strcpy((char*)ptr, message);
        printf("写入数据到共享内存: %s\n", message);
        
        /* 读取数据 */
        printf("从共享内存读取: %s\n", (char*)ptr);
    }
    
    vox_shm_destroy(shm);
    vox_shm_unlink(shm_name);
    vox_mpool_destroy(mpool);
    printf("共享内存测试完成\n");
}

/* 测试命名管道 */
void test_named_pipe(void) {
    printf("\n=== 测试命名管道 ===\n");
    
    /* 尝试使用 /tmp 目录（如果可用），因为某些文件系统（如 WSL 的 Windows 文件系统）不支持 FIFO */
    const char* pipe_name = "vox_test_pipe";
    char pipe_path[512] = {0};
    
#ifdef VOX_OS_WINDOWS
    snprintf(pipe_path, sizeof(pipe_path), "%s", pipe_name);
#else
    /* 在 POSIX 系统上，尝试使用 /tmp 目录 */
    const char* tmp_dir = getenv("TMPDIR");
    if (!tmp_dir) tmp_dir = "/tmp";
    snprintf(pipe_path, sizeof(pipe_path), "%s/%s", tmp_dir, pipe_name);
#endif
    
    /* 创建命名管道 */
    if (vox_named_pipe_create(pipe_path) != 0) {
        printf("创建命名管道失败: %s\n", pipe_path);
        printf("提示：可能是文件系统不支持 FIFO（如 WSL 的 Windows 文件系统）\n");
        printf("      或权限问题。某些文件系统（如 FAT32/NTFS）不支持命名管道\n");
    } else {
        printf("命名管道创建成功: %s\n", pipe_path);
    }
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    /* 注意：在实际应用中，读写应该在不同的进程中 */
    printf("注意：命名管道通常需要在不同的进程中进行读写\n");
    
    vox_named_pipe_unlink(pipe_path);
    vox_mpool_destroy(mpool);
    printf("命名管道测试完成\n");
}

/* 测试进程间信号量 */
void test_ipc_semaphore(void) {
    printf("\n=== 测试进程间信号量 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    const char* sem_name = "vox_test_sem";
    uint32_t initial_value = 2;
    
    /* 创建信号量 */
    vox_ipc_semaphore_t* sem = vox_ipc_semaphore_create(mpool, sem_name, initial_value, true);
    if (!sem) {
        printf("创建信号量失败，尝试打开已存在的...\n");
        sem = vox_ipc_semaphore_create(mpool, sem_name, 0, false);
        if (!sem) {
            printf("打开信号量也失败\n");
            vox_mpool_destroy(mpool);
            return;
        }
    }
    
    printf("信号量创建/打开成功\n");
    
    /* 获取信号量值 */
    int value = vox_ipc_semaphore_get_value(sem);
    printf("信号量当前值: %d\n", value);
    
    /* 等待信号量 */
    printf("等待信号量...\n");
    if (vox_ipc_semaphore_wait(sem) == 0) {
        printf("获取信号量成功\n");
        value = vox_ipc_semaphore_get_value(sem);
        printf("信号量当前值: %d\n", value);
        
        /* 释放信号量 */
        if (vox_ipc_semaphore_post(sem) == 0) {
            printf("释放信号量成功\n");
        }
    }
    
    vox_ipc_semaphore_destroy(sem);
    vox_ipc_semaphore_unlink(sem_name);
    vox_mpool_destroy(mpool);
    printf("进程间信号量测试完成\n");
}

/* 测试进程间互斥锁 */
void test_ipc_mutex(void) {
    printf("\n=== 测试进程间互斥锁 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    const char* mutex_name = "vox_test_mutex";
    
    /* 创建互斥锁 */
    vox_ipc_mutex_t* mutex = vox_ipc_mutex_create(mpool, mutex_name, true);
    if (!mutex) {
        printf("创建互斥锁失败，尝试打开已存在的...\n");
        mutex = vox_ipc_mutex_create(mpool, mutex_name, false);
        if (!mutex) {
            printf("打开互斥锁也失败\n");
            vox_mpool_destroy(mpool);
            return;
        }
    }
    
    printf("互斥锁创建/打开成功\n");
    
    /* 锁定互斥锁 */
    printf("尝试锁定互斥锁...\n");
    if (vox_ipc_mutex_lock(mutex) == 0) {
        printf("锁定互斥锁成功\n");
        
        /* 尝试再次锁定
         * 注意：Windows 互斥锁是递归的，同一线程可以多次锁定
         * POSIX 使用信号量实现，不支持递归锁定 */
        if (vox_ipc_mutex_trylock(mutex) == 0) {
            printf("互斥锁递归锁定成功（Windows 特性，POSIX 不支持）\n");
            /* 需要解锁两次 */
            vox_ipc_mutex_unlock(mutex);
        } else {
            printf("互斥锁不支持递归锁定（POSIX 行为）\n");
        }
        
        /* 解锁 */
        if (vox_ipc_mutex_unlock(mutex) == 0) {
            printf("解锁互斥锁成功\n");
        }
    }
    
    vox_ipc_mutex_destroy(mutex);
    vox_ipc_mutex_unlink(mutex_name);
    vox_mpool_destroy(mpool);
    printf("进程间互斥锁测试完成\n");
}

/* 测试文件锁 */
void test_file_lock(void) {
    printf("\n=== 测试文件锁 ===\n");
    
    vox_mpool_t* mpool = vox_mpool_create();
    if (!mpool) {
        fprintf(stderr, "创建内存池失败\n");
        return;
    }
    
    const char* lock_file = "test_lock_file.lock";
    
    /* 创建文件锁 */
    vox_file_lock_t* lock = vox_file_lock_create(mpool, lock_file);
    if (!lock) {
        printf("创建文件锁失败\n");
        vox_mpool_destroy(mpool);
        return;
    }
    
    printf("文件锁创建成功\n");
    
    /* 获取独占锁 */
    printf("尝试获取独占锁...\n");
    if (vox_file_lock_lock(lock, true) == 0) {
        printf("获取独占锁成功\n");
        
        /* 尝试获取共享锁
         * 注意：文件锁的行为可能因平台而异 */
        if (vox_file_lock_trylock(lock, false) == 0) {
            printf("文件锁允许重叠锁定（平台特定行为）\n");
            vox_file_lock_unlock(lock);
        } else {
            printf("文件已锁定，无法再次锁定\n");
        }
        
        /* 释放锁 */
        if (vox_file_lock_unlock(lock) == 0) {
            printf("释放文件锁成功\n");
        }
    }
    
    vox_file_lock_destroy(lock);
    vox_mpool_destroy(mpool);
    printf("文件锁测试完成\n");
}

/* 信号处理函数（需要在函数外部定义） */
static bool g_signal_received = false;

static void signal_handler(int sig) {
    (void)sig;
    g_signal_received = true;
    printf("收到信号: %d\n", sig);
}

/* 测试信号处理 */
void test_signal_handling(void) {
    printf("\n=== 测试信号处理 ===\n");
    
    g_signal_received = false;
    
    if (vox_process_signal_register(SIGINT, signal_handler) == 0) {
        printf("注册 SIGINT 信号处理成功\n");
        printf("提示：按 Ctrl+C 可以触发信号（如果支持）\n");
    } else {
        printf("注册信号处理失败\n");
    }
    
    /* 忽略信号 */
    if (vox_process_signal_ignore(SIGTERM) == 0) {
        printf("忽略 SIGTERM 信号成功\n");
    }
    
    printf("信号处理测试完成\n");
}

/* 测试进程组 */
void test_process_group(void) {
    printf("\n=== 测试进程组 ===\n");
    
    vox_process_id_t current_pgid = vox_process_group_get_current();
    printf("当前进程组ID: %llu\n", (unsigned long long)current_pgid);
    
    /* 创建新进程组 */
    vox_process_id_t new_pgid = vox_process_group_create();
    if (new_pgid != 0) {
        if (new_pgid == current_pgid) {
            printf("当前进程已是会话领导者，返回当前进程组ID: %llu\n", (unsigned long long)new_pgid);
        } else {
            printf("创建新进程组成功，进程组ID: %llu\n", (unsigned long long)new_pgid);
        }
    } else {
        printf("创建新进程组失败\n");
    }
    
    printf("进程组测试完成\n");
}

int main(void) {
    printf("========================================\n");
    printf("    vox_process IPC 示例程序\n");
    printf("========================================\n");
    
    /* 测试IPC功能 */
    test_shared_memory();
    test_named_pipe();
    test_ipc_semaphore();
    test_ipc_mutex();
    test_file_lock();
    
    /* 测试信号和进程组 */
    test_signal_handling();
    test_process_group();
    
    printf("\n========================================\n");
    printf("    所有IPC测试完成\n");
    printf("========================================\n");
    
    return 0;
}
