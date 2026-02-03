/*
 * vox_process.h - 跨平台进程管理抽象API
 * 提供统一的进程创建、管理和控制接口
 */

#ifndef VOX_PROCESS_H
#define VOX_PROCESS_H

#include "vox_os.h"
#include "vox_mpool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 进程不透明类型 */
typedef struct vox_process vox_process_t;

/* 进程ID类型 */
#ifdef VOX_OS_WINDOWS
    typedef DWORD vox_process_id_t;
#else
    #include <sys/types.h>
    #include <unistd.h>
    typedef pid_t vox_process_id_t;
#endif

/* 进程退出状态 */
typedef struct {
    bool exited;        /* 是否正常退出 */
    int exit_code;      /* 退出码（exited为true时有效） */
    bool signaled;      /* 是否被信号终止（POSIX） */
    int signal;         /* 信号编号（signaled为true时有效） */
} vox_process_status_t;

/* 标准流重定向类型 */
typedef enum {
    VOX_PROCESS_STDIN = 0,   /* 标准输入 */
    VOX_PROCESS_STDOUT,      /* 标准输出 */
    VOX_PROCESS_STDERR       /* 标准错误 */
} vox_process_stream_t;

/* 重定向目标类型 */
typedef enum {
    VOX_PROCESS_REDIRECT_NONE = 0,  /* 不重定向（继承父进程） */
    VOX_PROCESS_REDIRECT_PIPE,      /* 重定向到管道 */
    VOX_PROCESS_REDIRECT_FILE,      /* 重定向到文件 */
    VOX_PROCESS_REDIRECT_NULL       /* 重定向到空设备 */
} vox_process_redirect_t;

/* 进程启动选项 */
typedef struct {
    const char* working_dir;         /* 工作目录，NULL表示使用当前目录 */
    const char** env;                /* 环境变量数组，NULL表示继承父进程环境 */
    vox_process_redirect_t stdin_redirect;  /* 标准输入重定向 */
    vox_process_redirect_t stdout_redirect; /* 标准输出重定向 */
    vox_process_redirect_t stderr_redirect; /* 标准错误重定向 */
    const char* stdin_file;          /* 标准输入文件路径（stdin_redirect为FILE时） */
    const char* stdout_file;         /* 标准输出文件路径（stdout_redirect为FILE时） */
    const char* stderr_file;         /* 标准错误文件路径（stderr_redirect为FILE时） */
    bool detached;                   /* 是否分离进程（Windows） */
    bool create_no_window;           /* 是否不创建窗口（Windows） */
} vox_process_options_t;

/* ===== 进程创建和管理 ===== */

/**
 * 创建并启动新进程
 * @param mpool 内存池指针，必须非NULL
 * @param command 要执行的命令（程序路径）
 * @param argv 命令行参数数组，最后一个元素必须为NULL
 * @param options 进程启动选项，可为NULL使用默认选项
 * @return 成功返回进程指针，失败返回NULL
 */
vox_process_t* vox_process_create(vox_mpool_t* mpool, const char* command, 
                                   const char* const* argv, 
                                   const vox_process_options_t* options);

/**
 * 等待进程结束
 * @param process 进程指针
 * @param status 输出进程退出状态，可为NULL
 * @param timeout_ms 超时时间（毫秒），0表示无限等待
 * @return 成功返回0，超时返回1，失败返回-1
 */
int vox_process_wait(vox_process_t* process, vox_process_status_t* status, 
                     uint32_t timeout_ms);

/**
 * 终止进程
 * @param process 进程指针
 * @param force 是否强制终止（发送SIGKILL/TerminateProcess）
 * @return 成功返回0，失败返回-1
 */
int vox_process_terminate(vox_process_t* process, bool force);

/**
 * 获取进程ID
 * @param process 进程指针
 * @return 返回进程ID，失败返回0
 */
vox_process_id_t vox_process_get_id(const vox_process_t* process);

/**
 * 检查进程是否仍在运行
 * @param process 进程指针
 * @return 运行中返回true，已退出返回false
 */
bool vox_process_is_running(const vox_process_t* process);

/**
 * 获取进程退出状态（不等待）
 * @param process 进程指针
 * @param status 输出进程退出状态
 * @return 进程已退出返回0，仍在运行返回1，失败返回-1
 */
int vox_process_get_status(vox_process_t* process, vox_process_status_t* status);

/**
 * 销毁进程对象（不终止进程）
 * @param process 进程指针
 */
void vox_process_destroy(vox_process_t* process);

/**
 * 设置当前进程名（用于 ps/top 显示，类似 nginx 的 master/worker）
 * Linux 下使用 prctl(PR_SET_NAME)，名字最多 15 个字符（16 字节含 \\0）；其他平台无操作。
 * @param name 进程名，NULL 安全（无操作）
 * @return Linux 成功返回 0，失败返回 -1；其他平台恒返回 0
 */
int vox_process_setname(const char* name);

/* ===== 标准输入输出操作 ===== */

/**
 * 从进程的标准输出读取数据
 * @param process 进程指针
 * @param buffer 缓冲区
 * @param size 缓冲区大小
 * @return 成功返回读取的字节数，失败返回-1，EOF返回0
 */
int64_t vox_process_read_stdout(vox_process_t* process, void* buffer, size_t size);

/**
 * 向进程的标准输入写入数据
 * @param process 进程指针
 * @param buffer 数据缓冲区
 * @param size 数据大小
 * @return 成功返回写入的字节数，失败返回-1
 */
int64_t vox_process_write_stdin(vox_process_t* process, const void* buffer, size_t size);

/**
 * 关闭进程的标准输入
 * @param process 进程指针
 * @return 成功返回0，失败返回-1
 */
int vox_process_close_stdin(vox_process_t* process);

/* ===== 当前进程操作 ===== */

/**
 * 获取当前进程ID
 * @return 返回当前进程ID
 */
vox_process_id_t vox_process_get_current_id(void);

/**
 * 获取父进程ID
 * @return 返回父进程ID，失败返回0
 */
vox_process_id_t vox_process_get_parent_id(void);

/**
 * 退出当前进程
 * @param exit_code 退出码
 */
void vox_process_exit(int exit_code);

/* ===== 环境变量操作 ===== */

/**
 * 获取环境变量值
 * @param mpool 内存池指针，必须非NULL
 * @param name 环境变量名
 * @return 成功返回环境变量值（需要调用vox_mpool_free释放），失败返回NULL
 */
char* vox_process_getenv(vox_mpool_t* mpool, const char* name);

/**
 * 设置环境变量
 * @param name 环境变量名
 * @param value 环境变量值，NULL表示删除环境变量
 * @return 成功返回0，失败返回-1
 */
int vox_process_setenv(const char* name, const char* value);

/**
 * 取消设置环境变量
 * @param name 环境变量名
 * @return 成功返回0，失败返回-1
 */
int vox_process_unsetenv(const char* name);

/* ===== 便捷函数 ===== */

/**
 * 执行命令并获取输出
 * @param mpool 内存池指针，必须非NULL
 * @param command 要执行的命令
 * @param argv 命令行参数数组，最后一个元素必须为NULL
 * @param output 输出缓冲区指针，需要调用vox_mpool_free释放
 * @param output_size 输出缓冲区大小
 * @param exit_code 输出退出码，可为NULL
 * @return 成功返回0，失败返回-1
 */
int vox_process_execute(vox_mpool_t* mpool, const char* command, 
                        const char* const* argv,
                        char** output, size_t* output_size, int* exit_code);

/* ===== 进程间通信 (IPC) ===== */

/* 共享内存对象 */
typedef struct vox_shm vox_shm_t;

/**
 * 创建或打开共享内存
 * @param mpool 内存池指针，必须非NULL
 * @param name 共享内存名称
 * @param size 共享内存大小（字节）
 * @param create 是否创建（true=创建，false=打开已存在的）
 * @return 成功返回共享内存对象指针，失败返回NULL
 */
vox_shm_t* vox_shm_create(vox_mpool_t* mpool, const char* name, size_t size, bool create);

/**
 * 获取共享内存地址
 * @param shm 共享内存对象指针
 * @return 返回共享内存地址，失败返回NULL
 */
void* vox_shm_get_ptr(vox_shm_t* shm);

/**
 * 获取共享内存大小
 * @param shm 共享内存对象指针
 * @return 返回共享内存大小（字节）
 */
size_t vox_shm_get_size(vox_shm_t* shm);

/**
 * 销毁共享内存对象（不删除共享内存）
 * @param shm 共享内存对象指针
 */
void vox_shm_destroy(vox_shm_t* shm);

/**
 * 删除共享内存（从系统中删除）
 * @param name 共享内存名称
 * @return 成功返回0，失败返回-1
 */
int vox_shm_unlink(const char* name);

/* 命名管道/FIFO对象 */
typedef struct vox_named_pipe vox_named_pipe_t;

/**
 * 创建命名管道（FIFO）
 * @param name 管道名称
 * @return 成功返回0，失败返回-1
 */
int vox_named_pipe_create(const char* name);

/**
 * 打开命名管道
 * @param mpool 内存池指针，必须非NULL
 * @param name 管道名称
 * @param read_only 是否只读（true=只读，false=只写）
 * @return 成功返回管道对象指针，失败返回NULL
 */
vox_named_pipe_t* vox_named_pipe_open(vox_mpool_t* mpool, const char* name, bool read_only);

/**
 * 从命名管道读取数据
 * @param pipe 管道对象指针
 * @param buffer 缓冲区
 * @param size 缓冲区大小
 * @return 成功返回读取的字节数，失败返回-1，EOF返回0
 */
int64_t vox_named_pipe_read(vox_named_pipe_t* pipe, void* buffer, size_t size);

/**
 * 向命名管道写入数据
 * @param pipe 管道对象指针
 * @param buffer 数据缓冲区
 * @param size 数据大小
 * @return 成功返回写入的字节数，失败返回-1
 */
int64_t vox_named_pipe_write(vox_named_pipe_t* pipe, const void* buffer, size_t size);

/**
 * 关闭命名管道
 * @param pipe 管道对象指针
 */
void vox_named_pipe_close(vox_named_pipe_t* pipe);

/**
 * 删除命名管道
 * @param name 管道名称
 * @return 成功返回0，失败返回-1
 */
int vox_named_pipe_unlink(const char* name);

/* 进程间信号量对象 */
typedef struct vox_ipc_semaphore vox_ipc_semaphore_t;

/**
 * 创建或打开命名信号量
 * @param mpool 内存池指针，必须非NULL
 * @param name 信号量名称
 * @param initial_value 初始值（仅在创建时有效）
 * @param create 是否创建（true=创建，false=打开已存在的）
 * @return 成功返回信号量对象指针，失败返回NULL
 */
vox_ipc_semaphore_t* vox_ipc_semaphore_create(vox_mpool_t* mpool, const char* name, 
                                               uint32_t initial_value, bool create);

/**
 * 等待信号量（P操作）
 * @param sem 信号量对象指针
 * @return 成功返回0，失败返回-1
 */
int vox_ipc_semaphore_wait(vox_ipc_semaphore_t* sem);

/**
 * 尝试等待信号量（非阻塞）
 * @param sem 信号量对象指针
 * @return 成功返回0，失败返回-1
 */
int vox_ipc_semaphore_trywait(vox_ipc_semaphore_t* sem);

/**
 * 超时等待信号量
 * @param sem 信号量对象指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，超时返回1，失败返回-1
 */
int vox_ipc_semaphore_timedwait(vox_ipc_semaphore_t* sem, uint32_t timeout_ms);

/**
 * 释放信号量（V操作）
 * @param sem 信号量对象指针
 * @return 成功返回0，失败返回-1
 */
int vox_ipc_semaphore_post(vox_ipc_semaphore_t* sem);

/**
 * 获取信号量当前值
 * @param sem 信号量对象指针
 * @return 返回信号量值，失败返回-1
 */
int vox_ipc_semaphore_get_value(vox_ipc_semaphore_t* sem);

/**
 * 销毁信号量对象
 * @param sem 信号量对象指针
 */
void vox_ipc_semaphore_destroy(vox_ipc_semaphore_t* sem);

/**
 * 删除命名信号量
 * @param name 信号量名称
 * @return 成功返回0，失败返回-1
 */
int vox_ipc_semaphore_unlink(const char* name);

/* 进程间互斥锁对象 */
typedef struct vox_ipc_mutex vox_ipc_mutex_t;

/**
 * 创建或打开进程间互斥锁
 * @param mpool 内存池指针，必须非NULL
 * @param name 互斥锁名称
 * @param create 是否创建（true=创建，false=打开已存在的）
 * @return 成功返回互斥锁对象指针，失败返回NULL
 */
vox_ipc_mutex_t* vox_ipc_mutex_create(vox_mpool_t* mpool, const char* name, bool create);

/**
 * 锁定互斥锁
 * @param mutex 互斥锁对象指针
 * @return 成功返回0，失败返回-1
 */
int vox_ipc_mutex_lock(vox_ipc_mutex_t* mutex);

/**
 * 尝试锁定互斥锁（非阻塞）
 * @param mutex 互斥锁对象指针
 * @return 成功返回0，失败返回-1
 */
int vox_ipc_mutex_trylock(vox_ipc_mutex_t* mutex);

/**
 * 解锁互斥锁
 * @param mutex 互斥锁对象指针
 * @return 成功返回0，失败返回-1
 */
int vox_ipc_mutex_unlock(vox_ipc_mutex_t* mutex);

/**
 * 销毁互斥锁对象
 * @param mutex 互斥锁对象指针
 */
void vox_ipc_mutex_destroy(vox_ipc_mutex_t* mutex);

/**
 * 删除进程间互斥锁
 * @param name 互斥锁名称
 * @return 成功返回0，失败返回-1
 */
int vox_ipc_mutex_unlink(const char* name);

/* 文件锁对象 */
typedef struct vox_file_lock vox_file_lock_t;

/**
 * 创建文件锁
 * @param mpool 内存池指针，必须非NULL
 * @param file_path 文件路径
 * @return 成功返回文件锁对象指针，失败返回NULL
 */
vox_file_lock_t* vox_file_lock_create(vox_mpool_t* mpool, const char* file_path);

/**
 * 获取文件锁（阻塞）
 * @param lock 文件锁对象指针
 * @param exclusive 是否独占锁（true=独占，false=共享）
 * @return 成功返回0，失败返回-1
 */
int vox_file_lock_lock(vox_file_lock_t* lock, bool exclusive);

/**
 * 尝试获取文件锁（非阻塞）
 * @param lock 文件锁对象指针
 * @param exclusive 是否独占锁
 * @return 成功返回0，失败返回-1
 */
int vox_file_lock_trylock(vox_file_lock_t* lock, bool exclusive);

/**
 * 释放文件锁
 * @param lock 文件锁对象指针
 * @return 成功返回0，失败返回-1
 */
int vox_file_lock_unlock(vox_file_lock_t* lock);

/**
 * 销毁文件锁对象
 * @param lock 文件锁对象指针
 */
void vox_file_lock_destroy(vox_file_lock_t* lock);

/* ===== 信号处理 ===== */

/* 信号处理函数类型 */
typedef void (*vox_signal_handler_t)(int signal);

/**
 * 注册信号处理函数
 * @param signal 信号编号
 * @param handler 信号处理函数，NULL表示使用默认处理
 * @return 成功返回0，失败返回-1
 */
int vox_process_signal_register(int signal, vox_signal_handler_t handler);

/**
 * 恢复信号默认处理
 * @param signal 信号编号
 * @return 成功返回0，失败返回-1
 */
int vox_process_signal_reset(int signal);

/**
 * 忽略信号
 * @param signal 信号编号
 * @return 成功返回0，失败返回-1
 */
int vox_process_signal_ignore(int signal);

/**
 * 向进程发送信号
 * @param pid 进程ID
 * @param signal 信号编号
 * @return 成功返回0，失败返回-1
 */
int vox_process_signal_send(vox_process_id_t pid, int signal);

/**
 * 向进程组发送信号
 * @param pgid 进程组ID（0表示当前进程组）
 * @param signal 信号编号
 * @return 成功返回0，失败返回-1
 */
int vox_process_signal_send_group(vox_process_id_t pgid, int signal);

/* ===== 进程池管理 ===== */

/* 进程池对象 */
typedef struct vox_process_pool vox_process_pool_t;

/* 进程池任务函数类型 */
typedef int (*vox_process_pool_task_t)(void* task_data, void* worker_data);

/* 进程池配置 */
typedef struct {
    uint32_t worker_count;        /* 工作进程数量 */
    const char* worker_command;   /* 工作进程命令（NULL表示使用当前程序） */
    const char* const* worker_argv; /* 工作进程参数 */
    void* worker_data;            /* 传递给工作进程的数据 */
    bool auto_restart;            /* 工作进程退出后是否自动重启 */
    uint32_t max_restarts;        /* 最大重启次数（0表示无限制） */
} vox_process_pool_config_t;

/**
 * 创建进程池
 * @param mpool 内存池指针，必须非NULL
 * @param config 进程池配置
 * @return 成功返回进程池对象指针，失败返回NULL
 */
vox_process_pool_t* vox_process_pool_create(vox_mpool_t* mpool, 
                                             const vox_process_pool_config_t* config);

/**
 * 提交任务到进程池
 * @param pool 进程池对象指针
 * @param task_func 任务函数
 * @param task_data 任务数据
 * @return 成功返回0，失败返回-1
 */
int vox_process_pool_submit(vox_process_pool_t* pool, 
                            vox_process_pool_task_t task_func, 
                            void* task_data);

/**
 * 等待所有任务完成
 * @param pool 进程池对象指针
 * @param timeout_ms 超时时间（毫秒），0表示无限等待
 * @return 成功返回0，超时返回1，失败返回-1
 */
int vox_process_pool_wait(vox_process_pool_t* pool, uint32_t timeout_ms);

/**
 * 获取进程池状态
 * @param pool 进程池对象指针
 * @param active_workers 输出活跃工作进程数，可为NULL
 * @param pending_tasks 输出待处理任务数，可为NULL
 * @return 成功返回0，失败返回-1
 */
int vox_process_pool_get_status(vox_process_pool_t* pool, 
                                 uint32_t* active_workers, 
                                 uint32_t* pending_tasks);

/**
 * 停止进程池（等待所有任务完成）
 * @param pool 进程池对象指针
 * @return 成功返回0，失败返回-1
 */
int vox_process_pool_stop(vox_process_pool_t* pool);

/**
 * 销毁进程池
 * @param pool 进程池对象指针
 */
void vox_process_pool_destroy(vox_process_pool_t* pool);

/* ===== 进程组管理 ===== */

/**
 * 创建新进程组
 * @return 成功返回进程组ID，失败返回0
 */
vox_process_id_t vox_process_group_create(void);

/**
 * 获取当前进程组ID
 * @return 返回当前进程组ID
 */
vox_process_id_t vox_process_group_get_current(void);

/**
 * 设置进程组ID
 * @param pid 进程ID（0表示当前进程）
 * @param pgid 进程组ID（0表示使用pid作为进程组ID）
 * @return 成功返回0，失败返回-1
 */
int vox_process_group_set(vox_process_id_t pid, vox_process_id_t pgid);

/**
 * 向进程组发送信号
 * @param pgid 进程组ID（0表示当前进程组）
 * @param signal 信号编号
 * @return 成功返回0，失败返回-1
 */
int vox_process_group_signal(vox_process_id_t pgid, int signal);

#ifdef __cplusplus
}
#endif

#endif /* VOX_PROCESS_H */
