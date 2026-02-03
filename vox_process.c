/*
 * vox_process.c - 跨平台进程管理实现
 * 提供统一的进程创建、管理和控制接口
 */

#ifndef __GLIBC_PREREQ
#define __GLIBC_PREREQ(maj, min) 0
#endif

#include "vox_process.h"
#include "vox_os.h"
#include "vox_mpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef VOX_OS_WINDOWS
    #include <fcntl.h>
    #include <process.h>
    #include <tlhelp32.h>
    #include <limits.h>
#else
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <sys/stat.h>
    #include <sys/mman.h>
    #include <fcntl.h>
    #include <signal.h>
    #include <time.h>
    #include <spawn.h>
    #include <sys/file.h>
    #include <semaphore.h>
    #include <errno.h>
    #ifdef VOX_OS_MACOS
        #include <crt_externs.h>
        #define environ (*_NSGetEnviron())
    #else
        extern char** environ;
        #include <sys/shm.h>
    #endif
    #ifndef VOX_OS_MACOS
        #include <sys/sem.h>
    #endif
    #ifdef VOX_OS_LINUX
        #include <sys/prctl.h>
    #endif
#endif

/* 进程结构 */
struct vox_process {
    vox_mpool_t* mpool;              /* 内存池指针 */
#ifdef VOX_OS_WINDOWS
    HANDLE handle;                    /* 进程句柄 */
    HANDLE thread_handle;            /* 主线程句柄 */
    HANDLE stdin_write;              /* 标准输入写入端 */
    HANDLE stdout_read;              /* 标准输出读取端 */
    HANDLE stderr_read;              /* 标准错误读取端 */
#else
    pid_t pid;                        /* 进程ID */
    int stdin_write;                  /* 标准输入写入端 */
    int stdout_read;                  /* 标准输出读取端 */
    int stderr_read;                  /* 标准错误读取端 */
    bool has_pipes;                   /* 是否有管道 */
#endif
    vox_process_id_t id;              /* 进程ID */
    bool detached;                    /* 是否已分离 */
};

/* ===== 进程创建和管理 ===== */

static void init_process_options(vox_process_options_t* opts) {
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    opts->stdin_redirect = VOX_PROCESS_REDIRECT_NONE;
    opts->stdout_redirect = VOX_PROCESS_REDIRECT_NONE;
    opts->stderr_redirect = VOX_PROCESS_REDIRECT_NONE;
    opts->detached = false;
    opts->create_no_window = false;
}

int vox_process_setname(const char* name) {
#ifdef VOX_OS_LINUX
    char buf[16];
    size_t len = 0;
    if (name) {
        while (len < sizeof(buf) - 1 && name[len]) buf[len] = name[len], ++len;
    }
    buf[len] = '\0';
    return prctl(PR_SET_NAME, buf, 0, 0, 0) == 0 ? 0 : -1;
#else
    (void)name;
    return 0;
#endif
}

vox_process_t* vox_process_create(vox_mpool_t* mpool, const char* command, 
                                   const char* const* argv, 
                                   const vox_process_options_t* options) {
    if (!mpool || !command) return NULL;
    
    vox_process_t* proc = (vox_process_t*)vox_mpool_alloc(mpool, sizeof(vox_process_t));
    if (!proc) return NULL;
    
    memset(proc, 0, sizeof(*proc));
    proc->mpool = mpool;
    
    vox_process_options_t opts;
    if (options) {
        opts = *options;
    } else {
        init_process_options(&opts);
    }
    
#ifdef VOX_OS_WINDOWS
    /* 创建管道 */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    
    HANDLE stdin_read = NULL;
    HANDLE stdout_write = NULL;
    HANDLE stderr_write = NULL;
    
    if (opts.stdin_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        if (!CreatePipe(&stdin_read, &proc->stdin_write, &sa, 0)) {
            vox_mpool_free(mpool, proc);
            return NULL;
        }
        SetHandleInformation(proc->stdin_write, HANDLE_FLAG_INHERIT, 0);
    }
    
    if (opts.stdout_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        if (!CreatePipe(&proc->stdout_read, &stdout_write, &sa, 0)) {
            if (proc->stdin_write) CloseHandle(proc->stdin_write);
            if (stdin_read) CloseHandle(stdin_read);
            vox_mpool_free(mpool, proc);
            return NULL;
        }
        SetHandleInformation(proc->stdout_read, HANDLE_FLAG_INHERIT, 0);
    }
    
    if (opts.stderr_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        if (!CreatePipe(&proc->stderr_read, &stderr_write, &sa, 0)) {
            if (proc->stdin_write) CloseHandle(proc->stdin_write);
            if (proc->stdout_read) CloseHandle(proc->stdout_read);
            if (stdin_read) CloseHandle(stdin_read);
            if (stdout_write) CloseHandle(stdout_write);
            vox_mpool_free(mpool, proc);
            return NULL;
        }
        SetHandleInformation(proc->stderr_read, HANDLE_FLAG_INHERIT, 0);
    }
    
    /* 构建命令行 */
    size_t cmd_len = strlen(command) + 1;
    if (argv) {
        for (int i = 0; argv[i]; i++) {
            cmd_len += strlen(argv[i]) + 3;  /* +3 for space and quotes */
        }
    }
    
    char* cmd_line = (char*)vox_mpool_alloc(mpool, cmd_len);
    if (!cmd_line) {
        if (proc->stdin_write) CloseHandle(proc->stdin_write);
        if (proc->stdout_read) CloseHandle(proc->stdout_read);
        if (proc->stderr_read) CloseHandle(proc->stderr_read);
        if (stdin_read) CloseHandle(stdin_read);
        if (stdout_write) CloseHandle(stdout_write);
        if (stderr_write) CloseHandle(stderr_write);
        vox_mpool_free(mpool, proc);
        return NULL;
    }
    
    /* 安全地构建命令行 */
    size_t written = 0;
    int ret = snprintf(cmd_line + written, cmd_len - written, "%s", command);
    if (ret > 0) written += (size_t)ret;
    if (argv) {
        for (int i = 0; argv[i]; i++) {
            ret = snprintf(cmd_line + written, cmd_len - written, " %s", argv[i]);
            if (ret > 0) written += (size_t)ret;
        }
    }
    
    /* 设置启动信息 */
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    
    /* 跟踪需要关闭的文件句柄 */
    HANDLE stdin_file_handle = INVALID_HANDLE_VALUE;
    HANDLE stdout_file_handle = INVALID_HANDLE_VALUE;
    HANDLE stderr_file_handle = INVALID_HANDLE_VALUE;
    
    if (opts.stdin_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        si.hStdInput = stdin_read;
    } else if (opts.stdin_redirect == VOX_PROCESS_REDIRECT_NULL) {
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    } else if (opts.stdin_redirect == VOX_PROCESS_REDIRECT_FILE && opts.stdin_file) {
        stdin_file_handle = CreateFileA(opts.stdin_file, GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        si.hStdInput = (stdin_file_handle != INVALID_HANDLE_VALUE) ? stdin_file_handle : GetStdHandle(STD_INPUT_HANDLE);
    } else {
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    
    if (opts.stdout_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        si.hStdOutput = stdout_write;
    } else if (opts.stdout_redirect == VOX_PROCESS_REDIRECT_NULL) {
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    } else if (opts.stdout_redirect == VOX_PROCESS_REDIRECT_FILE && opts.stdout_file) {
        stdout_file_handle = CreateFileA(opts.stdout_file, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        si.hStdOutput = (stdout_file_handle != INVALID_HANDLE_VALUE) ? stdout_file_handle : GetStdHandle(STD_OUTPUT_HANDLE);
    } else {
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    
    if (opts.stderr_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        si.hStdError = stderr_write;
    } else if (opts.stderr_redirect == VOX_PROCESS_REDIRECT_NULL) {
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    } else if (opts.stderr_redirect == VOX_PROCESS_REDIRECT_FILE && opts.stderr_file) {
        stderr_file_handle = CreateFileA(opts.stderr_file, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        si.hStdError = (stderr_file_handle != INVALID_HANDLE_VALUE) ? stderr_file_handle : GetStdHandle(STD_ERROR_HANDLE);
    } else {
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    
    si.dwFlags = STARTF_USESTDHANDLES;
    
    DWORD creation_flags = 0;
    if (opts.create_no_window) {
        creation_flags |= CREATE_NO_WINDOW;
    }
    if (opts.detached) {
        creation_flags |= DETACHED_PROCESS;
    }
    
    /* 设置工作目录 */
    const char* working_dir = opts.working_dir;
    
    /* 创建进程 */
    BOOL success = CreateProcessA(
        NULL,                    /* 应用程序名 */
        cmd_line,                /* 命令行 */
        NULL,                    /* 进程安全属性 */
        NULL,                    /* 线程安全属性 */
        TRUE,                    /* 继承句柄 */
        creation_flags,         /* 创建标志 */
        NULL,                    /* 环境变量（继承） */
        working_dir,             /* 工作目录 */
        &si,                     /* 启动信息 */
        &pi                      /* 进程信息 */
    );
    
    vox_mpool_free(mpool, cmd_line);
    
    /* 关闭不需要的句柄 */
    if (stdin_read) CloseHandle(stdin_read);
    if (stdout_write) CloseHandle(stdout_write);
    if (stderr_write) CloseHandle(stderr_write);
    
    /* 关闭文件句柄（子进程已继承，父进程不再需要） */
    if (stdin_file_handle != INVALID_HANDLE_VALUE) CloseHandle(stdin_file_handle);
    if (stdout_file_handle != INVALID_HANDLE_VALUE) CloseHandle(stdout_file_handle);
    if (stderr_file_handle != INVALID_HANDLE_VALUE) CloseHandle(stderr_file_handle);
    
    if (!success) {
        if (proc->stdin_write) CloseHandle(proc->stdin_write);
        if (proc->stdout_read) CloseHandle(proc->stdout_read);
        if (proc->stderr_read) CloseHandle(proc->stderr_read);
        vox_mpool_free(mpool, proc);
        return NULL;
    }
    
    proc->handle = pi.hProcess;
    proc->thread_handle = pi.hThread;
    proc->id = pi.dwProcessId;
    proc->detached = opts.detached;
    
    if (opts.detached) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
#else
    /* 创建管道 */
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    
    if (opts.stdin_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        if (pipe(stdin_pipe) != 0) {
            vox_mpool_free(mpool, proc);
            return NULL;
        }
        proc->stdin_write = stdin_pipe[1];
    }
    
    if (opts.stdout_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        if (pipe(stdout_pipe) != 0) {
            if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
            if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
            vox_mpool_free(mpool, proc);
            return NULL;
        }
        proc->stdout_read = stdout_pipe[0];
    }
    
    if (opts.stderr_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        if (pipe(stderr_pipe) != 0) {
            if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
            if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
            if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
            if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
            vox_mpool_free(mpool, proc);
            return NULL;
        }
        proc->stderr_read = stderr_pipe[0];
    }
    
    proc->has_pipes = (opts.stdin_redirect == VOX_PROCESS_REDIRECT_PIPE ||
                       opts.stdout_redirect == VOX_PROCESS_REDIRECT_PIPE ||
                       opts.stderr_redirect == VOX_PROCESS_REDIRECT_PIPE);
    
    /* 准备参数数组 */
    size_t argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    
    char** exec_argv = (char**)vox_mpool_alloc(mpool, (argc + 2) * sizeof(char*));
    if (!exec_argv) {
        if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
        if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
        if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
        if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
        if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
        if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
        vox_mpool_free(mpool, proc);
        return NULL;
    }
    
    exec_argv[0] = (char*)command;
    if (argv) {
        for (size_t i = 0; i < argc; i++) {
            exec_argv[i + 1] = (char*)argv[i];
        }
    }
    exec_argv[argc + 1] = NULL;
    
    /* 准备环境变量 */
    char** exec_env = NULL;
    if (opts.env) {
        size_t env_count = 0;
        while (opts.env[env_count]) env_count++;
        exec_env = (char**)vox_mpool_alloc(mpool, (env_count + 1) * sizeof(char*));
        if (exec_env) {
            for (size_t i = 0; i < env_count; i++) {
                exec_env[i] = (char*)opts.env[i];
            }
            exec_env[env_count] = NULL;
        }
    }
    
    /* 使用 posix_spawn 创建进程 */
    posix_spawn_file_actions_t file_actions;
    int action_result = posix_spawn_file_actions_init(&file_actions);
    if (action_result != 0) {
        if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
        if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
        if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
        if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
        if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
        if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
        vox_mpool_free(mpool, exec_argv);
        if (exec_env) vox_mpool_free(mpool, exec_env);
        vox_mpool_free(mpool, proc);
        return NULL;
    }
    
    /* 设置工作目录 */
    if (opts.working_dir) {
        /* 尝试使用 posix_spawn_file_actions_addchdir_np (glibc 2.29+) */
        #if defined(__GLIBC__) && __GLIBC_PREREQ(2, 29)
            action_result = posix_spawn_file_actions_addchdir_np(&file_actions, opts.working_dir);
            if (action_result != 0) {
                posix_spawn_file_actions_destroy(&file_actions);
                if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
                if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
                if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
                if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
                if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
                if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
                vox_mpool_free(mpool, exec_argv);
                if (exec_env) vox_mpool_free(mpool, exec_env);
                vox_mpool_free(mpool, proc);
                return NULL;
            }
        #endif
    }
    
    /* 设置标准输入 */
    if (opts.stdin_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        action_result = posix_spawn_file_actions_adddup2(&file_actions, stdin_pipe[0], STDIN_FILENO);
        if (action_result == 0) {
            action_result = posix_spawn_file_actions_addclose(&file_actions, stdin_pipe[1]);
        }
        if (action_result != 0) {
            posix_spawn_file_actions_destroy(&file_actions);
            if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
            if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
            if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
            if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
            if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
            if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
            vox_mpool_free(mpool, exec_argv);
            if (exec_env) vox_mpool_free(mpool, exec_env);
            vox_mpool_free(mpool, proc);
            return NULL;
        }
    } else if (opts.stdin_redirect == VOX_PROCESS_REDIRECT_NULL) {
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            action_result = posix_spawn_file_actions_adddup2(&file_actions, null_fd, STDIN_FILENO);
            if (action_result == 0) {
                action_result = posix_spawn_file_actions_addclose(&file_actions, null_fd);
            }
            if (action_result != 0) {
                close(null_fd);
            }
        }
    } else if (opts.stdin_redirect == VOX_PROCESS_REDIRECT_FILE && opts.stdin_file) {
        int fd = open(opts.stdin_file, O_RDONLY);
        if (fd >= 0) {
            action_result = posix_spawn_file_actions_adddup2(&file_actions, fd, STDIN_FILENO);
            if (action_result == 0) {
                action_result = posix_spawn_file_actions_addclose(&file_actions, fd);
            }
            if (action_result != 0) {
                close(fd);
            }
        }
    }
    
    /* 设置标准输出 */
    if (opts.stdout_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        action_result = posix_spawn_file_actions_adddup2(&file_actions, stdout_pipe[1], STDOUT_FILENO);
        if (action_result == 0) {
            action_result = posix_spawn_file_actions_addclose(&file_actions, stdout_pipe[0]);
        }
        if (action_result != 0) {
            posix_spawn_file_actions_destroy(&file_actions);
            if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
            if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
            if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
            if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
            if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
            if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
            vox_mpool_free(mpool, exec_argv);
            if (exec_env) vox_mpool_free(mpool, exec_env);
            vox_mpool_free(mpool, proc);
            return NULL;
        }
    } else if (opts.stdout_redirect == VOX_PROCESS_REDIRECT_NULL) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            action_result = posix_spawn_file_actions_adddup2(&file_actions, null_fd, STDOUT_FILENO);
            if (action_result == 0) {
                action_result = posix_spawn_file_actions_addclose(&file_actions, null_fd);
            }
            if (action_result != 0) {
                close(null_fd);
            }
        }
    } else if (opts.stdout_redirect == VOX_PROCESS_REDIRECT_FILE && opts.stdout_file) {
        int fd = open(opts.stdout_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            action_result = posix_spawn_file_actions_adddup2(&file_actions, fd, STDOUT_FILENO);
            if (action_result == 0) {
                action_result = posix_spawn_file_actions_addclose(&file_actions, fd);
            }
            if (action_result != 0) {
                close(fd);
            }
        }
    }
    
    /* 设置标准错误 */
    if (opts.stderr_redirect == VOX_PROCESS_REDIRECT_PIPE) {
        action_result = posix_spawn_file_actions_adddup2(&file_actions, stderr_pipe[1], STDERR_FILENO);
        if (action_result == 0) {
            action_result = posix_spawn_file_actions_addclose(&file_actions, stderr_pipe[0]);
        }
        if (action_result != 0) {
            posix_spawn_file_actions_destroy(&file_actions);
            if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
            if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
            if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
            if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
            if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
            if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
            vox_mpool_free(mpool, exec_argv);
            if (exec_env) vox_mpool_free(mpool, exec_env);
            vox_mpool_free(mpool, proc);
            return NULL;
        }
    } else if (opts.stderr_redirect == VOX_PROCESS_REDIRECT_NULL) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            action_result = posix_spawn_file_actions_adddup2(&file_actions, null_fd, STDERR_FILENO);
            if (action_result == 0) {
                action_result = posix_spawn_file_actions_addclose(&file_actions, null_fd);
            }
            if (action_result != 0) {
                close(null_fd);
            }
        }
    } else if (opts.stderr_redirect == VOX_PROCESS_REDIRECT_FILE && opts.stderr_file) {
        int fd = open(opts.stderr_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            action_result = posix_spawn_file_actions_adddup2(&file_actions, fd, STDERR_FILENO);
            if (action_result == 0) {
                action_result = posix_spawn_file_actions_addclose(&file_actions, fd);
            }
            if (action_result != 0) {
                close(fd);
            }
        }
    }
    
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    
    if (opts.detached) {
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
    }
    
    /* 注意：在 posix_spawnp 之前不要关闭父进程端的管道，
     * 因为 posix_spawnp 需要这些文件描述符在调用时仍然有效 */
    int result = posix_spawnp(&proc->pid, command, &file_actions, &attr, 
                              exec_argv, exec_env ? exec_env : environ);
    
    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&attr);
    
    /* 现在可以安全地关闭父进程端的管道了 */
    if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
    if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
    if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
    
    if (result != 0) {
        /* 保存 errno（posix_spawnp 会设置 errno） */
        int saved_errno = errno;
        if (proc->stdin_write >= 0) close(proc->stdin_write);
        if (proc->stdout_read >= 0) close(proc->stdout_read);
        if (proc->stderr_read >= 0) close(proc->stderr_read);
        vox_mpool_free(mpool, exec_argv);
        if (exec_env) vox_mpool_free(mpool, exec_env);
        vox_mpool_free(mpool, proc);
        /* 恢复 errno 以便调用者可以检查 */
        errno = saved_errno;
        return NULL;
    }
    
    proc->id = proc->pid;
    proc->detached = opts.detached;
    
    vox_mpool_free(mpool, exec_argv);
    if (exec_env) vox_mpool_free(mpool, exec_env);
#endif
    
    return proc;
}

int vox_process_wait(vox_process_t* process, vox_process_status_t* status, 
                     uint32_t timeout_ms) {
    if (!process) return -1;
    
    if (process->detached) {
        return -1;  /* 分离的进程无法等待 */
    }
    
#ifdef VOX_OS_WINDOWS
    if (process->handle == NULL) {
        return -1;
    }
    
    DWORD wait_result;
    if (timeout_ms == 0) {
        wait_result = WaitForSingleObject(process->handle, INFINITE);
    } else {
        wait_result = WaitForSingleObject(process->handle, timeout_ms);
    }
    
    if (wait_result == WAIT_TIMEOUT) {
        return 1;  /* 超时 */
    }
    
    if (wait_result != WAIT_OBJECT_0) {
        return -1;  /* 失败 */
    }
    
    if (status) {
        DWORD exit_code;
        GetExitCodeProcess(process->handle, &exit_code);
        memset(status, 0, sizeof(*status));
        status->exited = true;
        status->exit_code = (int)exit_code;
    }
    
    return 0;
#else
    int wait_result;
    int wait_status;
    
    if (timeout_ms == 0) {
        wait_result = waitpid(process->pid, &wait_status, 0);
    } else {
        /* 简单的超时实现：使用非阻塞等待 */
        wait_result = waitpid(process->pid, &wait_status, WNOHANG);
        if (wait_result == 0) {
            /* 进程仍在运行，等待超时 */
            /* 使用 nanosleep 或简单的循环等待 */
            struct timespec ts;
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000;
            nanosleep(&ts, NULL);
            wait_result = waitpid(process->pid, &wait_status, WNOHANG);
            if (wait_result == 0) {
                return 1;  /* 超时 */
            }
        }
    }
    
    if (wait_result < 0) {
        return -1;
    }
    
    if (status) {
        memset(status, 0, sizeof(*status));
        if (WIFEXITED(wait_status)) {
            status->exited = true;
            status->exit_code = WEXITSTATUS(wait_status);
        } else if (WIFSIGNALED(wait_status)) {
            status->signaled = true;
            status->signal = WTERMSIG(wait_status);
        }
    }
    
    return 0;
#endif
}

int vox_process_terminate(vox_process_t* process, bool force) {
    if (!process) return -1;
    
    if (process->detached) {
        return -1;  /* 分离的进程无法终止 */
    }
    
#ifdef VOX_OS_WINDOWS
    if (process->handle == NULL) {
        return -1;
    }
    
    if (force) {
        return TerminateProcess(process->handle, 1) ? 0 : -1;
    } else {
        /* 发送关闭消息（仅对GUI程序有效） */
        return TerminateProcess(process->handle, 0) ? 0 : -1;
    }
#else
    if (force) {
        return kill(process->pid, SIGKILL) == 0 ? 0 : -1;
    } else {
        return kill(process->pid, SIGTERM) == 0 ? 0 : -1;
    }
#endif
}

vox_process_id_t vox_process_get_id(const vox_process_t* process) {
    if (!process) return 0;
    return process->id;
}

bool vox_process_is_running(const vox_process_t* process) {
    if (!process) return false;
    
    if (process->detached) {
        return false;  /* 分离的进程无法检查状态 */
    }
    
#ifdef VOX_OS_WINDOWS
    if (process->handle == NULL) {
        return false;
    }
    
    DWORD exit_code;
    if (!GetExitCodeProcess(process->handle, &exit_code)) {
        return false;
    }
    
    return exit_code == STILL_ACTIVE;
#else
    int wait_result = waitpid(process->pid, NULL, WNOHANG);
    if (wait_result < 0) {
        return false;  /* 进程不存在 */
    }
    return wait_result == 0;  /* 0 表示进程仍在运行 */
#endif
}

int vox_process_get_status(vox_process_t* process, vox_process_status_t* status) {
    if (!process || !status) return -1;
    
    if (process->detached) {
        return -1;  /* 分离的进程无法获取状态 */
    }
    
    memset(status, 0, sizeof(*status));
    
#ifdef VOX_OS_WINDOWS
    if (process->handle == NULL) {
        return -1;
    }
    
    DWORD exit_code;
    if (!GetExitCodeProcess(process->handle, &exit_code)) {
        return -1;
    }
    
    if (exit_code == STILL_ACTIVE) {
        return 1;  /* 仍在运行 */
    }
    
    status->exited = true;
    status->exit_code = (int)exit_code;
    return 0;
#else
    int wait_status;
    int wait_result = waitpid(process->pid, &wait_status, WNOHANG);
    
    if (wait_result < 0) {
        return -1;
    }
    
    if (wait_result == 0) {
        return 1;  /* 仍在运行 */
    }
    
    if (WIFEXITED(wait_status)) {
        status->exited = true;
        status->exit_code = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        status->signaled = true;
        status->signal = WTERMSIG(wait_status);
    }
    
    return 0;
#endif
}

void vox_process_destroy(vox_process_t* process) {
    if (!process) return;
    
#ifdef VOX_OS_WINDOWS
    if (process->handle && !process->detached) {
        CloseHandle(process->handle);
    }
    if (process->thread_handle && !process->detached) {
        CloseHandle(process->thread_handle);
    }
    if (process->stdin_write) {
        CloseHandle(process->stdin_write);
    }
    if (process->stdout_read) {
        CloseHandle(process->stdout_read);
    }
    if (process->stderr_read) {
        CloseHandle(process->stderr_read);
    }
#else
    if (process->has_pipes) {
        if (process->stdin_write >= 0) {
            close(process->stdin_write);
        }
        if (process->stdout_read >= 0) {
            close(process->stdout_read);
        }
        if (process->stderr_read >= 0) {
            close(process->stderr_read);
        }
    }
#endif
    
    vox_mpool_free(process->mpool, process);
}

/* ===== 标准输入输出操作 ===== */

int64_t vox_process_read_stdout(vox_process_t* process, void* buffer, size_t size) {
    if (!process || !buffer || size == 0) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (process->stdout_read == NULL) {
        return -1;
    }
    
    DWORD bytes_read = 0;
    if (!ReadFile(process->stdout_read, buffer, (DWORD)size, &bytes_read, NULL)) {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE) {
            return 0;  /* EOF */
        }
        return -1;
    }
    
    return (int64_t)bytes_read;
#else
    if (process->stdout_read < 0) {
        return -1;
    }
    
    ssize_t bytes_read = read(process->stdout_read, buffer, size);
    if (bytes_read < 0) {
        return -1;
    }
    
    return (int64_t)bytes_read;
#endif
}

int64_t vox_process_write_stdin(vox_process_t* process, const void* buffer, size_t size) {
    if (!process || !buffer || size == 0) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (process->stdin_write == NULL) {
        return -1;
    }
    
    DWORD bytes_written = 0;
    if (!WriteFile(process->stdin_write, buffer, (DWORD)size, &bytes_written, NULL)) {
        return -1;
    }
    
    return (int64_t)bytes_written;
#else
    if (process->stdin_write < 0) {
        return -1;
    }
    
    ssize_t bytes_written = write(process->stdin_write, buffer, size);
    if (bytes_written < 0) {
        return -1;
    }
    
    return (int64_t)bytes_written;
#endif
}

int vox_process_close_stdin(vox_process_t* process) {
    if (!process) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (process->stdin_write) {
        CloseHandle(process->stdin_write);
        process->stdin_write = NULL;
        return 0;
    }
#else
    if (process->stdin_write >= 0) {
        close(process->stdin_write);
        process->stdin_write = -1;
        return 0;
    }
#endif
    
    return -1;
}

/* ===== 当前进程操作 ===== */

vox_process_id_t vox_process_get_current_id(void) {
#ifdef VOX_OS_WINDOWS
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}

vox_process_id_t vox_process_get_parent_id(void) {
#ifdef VOX_OS_WINDOWS
    /* Windows 没有直接获取父进程ID的API，需要查询 */
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    DWORD current_pid = GetCurrentProcessId();
    DWORD parent_pid = 0;
    
    if (Process32First(snapshot, &pe)) {
        do {
            if (pe.th32ProcessID == current_pid) {
                parent_pid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32Next(snapshot, &pe));
    }
    
    CloseHandle(snapshot);
    return parent_pid;
#else
    return getppid();
#endif
}

void vox_process_exit(int exit_code) {
#ifdef VOX_OS_WINDOWS
    ExitProcess(exit_code);
#else
    exit(exit_code);
#endif
}

/* ===== 环境变量操作 ===== */

char* vox_process_getenv(vox_mpool_t* mpool, const char* name) {
    if (!mpool || !name) return NULL;
    
#ifdef VOX_OS_WINDOWS
    /* Windows 上使用 GetEnvironmentVariableA 以确保获取最新值 */
    DWORD len = GetEnvironmentVariableA(name, NULL, 0);
    if (len == 0) {
        DWORD error = GetLastError();
        if (error == ERROR_ENVVAR_NOT_FOUND) {
            return NULL;  /* 环境变量不存在 */
        }
        /* 其他错误（如缓冲区不足）也返回 NULL */
        (void)error;  /* 避免未使用变量警告 */
        return NULL;
    }
    
    char* result = (char*)vox_mpool_alloc(mpool, len);
    if (!result) return NULL;
    
    DWORD actual_len = GetEnvironmentVariableA(name, result, len);
    if (actual_len == 0 || actual_len >= len) {
        vox_mpool_free(mpool, result);
        return NULL;
    }
    
    return result;
#else
    const char* value = getenv(name);
    if (!value) return NULL;
    
    size_t len = strlen(value) + 1;
    char* result = (char*)vox_mpool_alloc(mpool, len);
    if (!result) return NULL;
    
    strcpy(result, value);
    return result;
#endif
}

int vox_process_setenv(const char* name, const char* value) {
    if (!name) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (value) {
        return SetEnvironmentVariableA(name, value) ? 0 : -1;
    } else {
        return SetEnvironmentVariableA(name, NULL) ? 0 : -1;
    }
#else
    if (value) {
        return setenv(name, value, 1) == 0 ? 0 : -1;
    } else {
        return unsetenv(name) == 0 ? 0 : -1;
    }
#endif
}

int vox_process_unsetenv(const char* name) {
    return vox_process_setenv(name, NULL);
}

/* ===== 便捷函数 ===== */

int vox_process_execute(vox_mpool_t* mpool, const char* command, 
                        const char* const* argv,
                        char** output, size_t* output_size, int* exit_code) {
    if (!mpool || !command || !output) return -1;
    
    vox_process_options_t opts;
    init_process_options(&opts);
    opts.stdout_redirect = VOX_PROCESS_REDIRECT_PIPE;
    /* 将 stderr 重定向到 NULL，因为我们只读取 stdout */
    opts.stderr_redirect = VOX_PROCESS_REDIRECT_NULL;
    
    vox_process_t* proc = vox_process_create(mpool, command, argv, &opts);
    if (!proc) {
        return -1;
    }
    
    /* 读取输出 */
    size_t buffer_size = 4096;
    size_t buffer_used = 0;
    char* buffer = (char*)vox_mpool_alloc(mpool, buffer_size);
    if (!buffer) {
        vox_process_destroy(proc);
        return -1;
    }
    
    char temp[4096];
    int64_t bytes_read;
    
    /* 读取 stdout（stderr 已经合并到 stdout） */
    while ((bytes_read = vox_process_read_stdout(proc, temp, sizeof(temp))) > 0) {
        if (buffer_used + bytes_read > buffer_size) {
            size_t new_size = buffer_size * 2;
            while (buffer_used + bytes_read > new_size) {
                new_size *= 2;
            }
            char* new_buffer = (char*)vox_mpool_realloc(mpool, buffer, new_size);
            if (!new_buffer) {
                vox_mpool_free(mpool, buffer);
                vox_process_destroy(proc);
                return -1;
            }
            buffer = new_buffer;
            buffer_size = new_size;
        }
        memcpy(buffer + buffer_used, temp, (size_t)bytes_read);
        buffer_used += (size_t)bytes_read;
    }
    
    /* 检查读取错误 */
    if (bytes_read < 0) {
        vox_mpool_free(mpool, buffer);
        vox_process_destroy(proc);
        return -1;
    }
    
    /* 等待进程结束 */
    vox_process_status_t status;
    if (vox_process_wait(proc, &status, 0) == 0) {
        if (exit_code) {
            *exit_code = status.exit_code;
        }
    } else {
        if (exit_code) {
            *exit_code = -1;
        }
    }
    
    vox_process_destroy(proc);
    
    *output = buffer;
    if (output_size) {
        *output_size = buffer_used;
    }
    
    return 0;
}

/* ===== 进程间通信 (IPC) ===== */

/* 共享内存结构 */
struct vox_shm {
    vox_mpool_t* mpool;
#ifdef VOX_OS_WINDOWS
    HANDLE handle;
    void* ptr;
#else
    int fd;
    void* ptr;
    bool created;
#endif
    size_t size;
    char name[256];
};

vox_shm_t* vox_shm_create(vox_mpool_t* mpool, const char* name, size_t size, bool create) {
    if (!mpool || !name || size == 0) return NULL;
    
    vox_shm_t* shm = (vox_shm_t*)vox_mpool_alloc(mpool, sizeof(vox_shm_t));
    if (!shm) return NULL;
    
    memset(shm, 0, sizeof(*shm));
    shm->mpool = mpool;
    shm->size = size;
    strncpy(shm->name, name, sizeof(shm->name) - 1);
    shm->name[sizeof(shm->name) - 1] = '\0';
    
#ifdef VOX_OS_WINDOWS
    /* Windows 使用文件映射 */
    /* 先尝试 Local\（不需要管理员权限），如果失败再尝试 Global\ */
    char shm_name[256];
    const char* prefixes[] = {"Local\\", "Global\\", ""};
    HANDLE hMapping = NULL;
    bool found = false;
    
    for (int i = 0; i < 3 && !found; i++) {
        snprintf(shm_name, sizeof(shm_name), "%s%s", prefixes[i], name);
        
        if (create) {
            /* 先尝试打开已存在的 */
            hMapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
            if (hMapping) {
                /* 已存在，使用它 */
                found = true;
                create = false;
            } else {
                /* 创建新的 */
                SetLastError(0);  /* 清除之前的错误 */
                hMapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 
                                               (DWORD)size, shm_name);
                if (hMapping) {
                    /* 检查是否真的创建了新对象（而不是打开了已存在的） */
                    DWORD last_error = GetLastError();
                    if (last_error == ERROR_ALREADY_EXISTS) {
                        CloseHandle(hMapping);
                        /* 打开已存在的 */
                        hMapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
                        if (hMapping) {
                            found = true;
                            create = false;
                        }
                    } else {
                        found = true;
                    }
                }
            }
        } else {
            hMapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
            if (hMapping) {
                found = true;
            }
        }
    }
    
    if (!found || !hMapping) {
        vox_mpool_free(mpool, shm);
        return NULL;
    }
    
    shm->handle = hMapping;
    
    shm->ptr = MapViewOfFile(shm->handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!shm->ptr) {
        CloseHandle(shm->handle);
        vox_mpool_free(mpool, shm);
        return NULL;
    }
#else
    /* POSIX 共享内存 */
    char shm_name[256];
    snprintf(shm_name, sizeof(shm_name), "/%s", name);
    
    if (create) {
        shm_unlink(shm_name);  /* 如果已存在则删除 */
    }
    
    shm->fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm->fd < 0) {
        vox_mpool_free(mpool, shm);
        return NULL;
    }
    
    shm->created = create;
    
    if (create) {
        if (ftruncate(shm->fd, (off_t)size) != 0) {
            close(shm->fd);
            shm_unlink(shm_name);
            vox_mpool_free(mpool, shm);
            return NULL;
        }
    }
    
    shm->ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0);
    if (shm->ptr == MAP_FAILED) {
        close(shm->fd);
        if (create) shm_unlink(shm_name);
        vox_mpool_free(mpool, shm);
        return NULL;
    }
#endif
    
    return shm;
}

void* vox_shm_get_ptr(vox_shm_t* shm) {
    if (!shm) return NULL;
    return shm->ptr;
}

size_t vox_shm_get_size(vox_shm_t* shm) {
    if (!shm) return 0;
    return shm->size;
}

void vox_shm_destroy(vox_shm_t* shm) {
    if (!shm) return;
    
#ifdef VOX_OS_WINDOWS
    if (shm->ptr) {
        UnmapViewOfFile(shm->ptr);
    }
    if (shm->handle) {
        CloseHandle(shm->handle);
    }
#else
    if (shm->ptr && shm->ptr != MAP_FAILED) {
        munmap(shm->ptr, shm->size);
    }
    if (shm->fd >= 0) {
        close(shm->fd);
    }
#endif
    
    vox_mpool_free(shm->mpool, shm);
}

int vox_shm_unlink(const char* name) {
    if (!name) return -1;
    
#ifdef VOX_OS_WINDOWS
    /* Windows 上共享内存会在所有句柄关闭后自动删除 */
    return 0;
#else
    char shm_name[256];
    snprintf(shm_name, sizeof(shm_name), "/%s", name);
    return shm_unlink(shm_name) == 0 ? 0 : -1;
#endif
}

/* 命名管道结构 */
struct vox_named_pipe {
    vox_mpool_t* mpool;
#ifdef VOX_OS_WINDOWS
    HANDLE handle;
#else
    int fd;
#endif
    bool read_only;
    char name[256];
};

int vox_named_pipe_create(const char* name) {
    if (!name) return -1;
    
#ifdef VOX_OS_WINDOWS
    char pipe_name[256];
    snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\%s", name);
    
    HANDLE hPipe = CreateNamedPipeA(
        pipe_name,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,  /* 最大实例数 */
        4096,  /* 输出缓冲区大小 */
        4096,  /* 输入缓冲区大小 */
        0,  /* 默认超时 */
        NULL  /* 默认安全属性 */
    );
    
    if (hPipe == INVALID_HANDLE_VALUE) {
        return -1;
    }
    
    CloseHandle(hPipe);
    return 0;
#else
    /* 如果已存在，先删除（忽略错误） */
    unlink(name);
    /* 创建命名管道 */
    return mkfifo(name, 0666) == 0 ? 0 : -1;
#endif
}

vox_named_pipe_t* vox_named_pipe_open(vox_mpool_t* mpool, const char* name, bool read_only) {
    if (!mpool || !name) return NULL;
    
    vox_named_pipe_t* pipe = (vox_named_pipe_t*)vox_mpool_alloc(mpool, sizeof(vox_named_pipe_t));
    if (!pipe) return NULL;
    
    memset(pipe, 0, sizeof(*pipe));
    pipe->mpool = mpool;
    pipe->read_only = read_only;
    strncpy(pipe->name, name, sizeof(pipe->name) - 1);
    pipe->name[sizeof(pipe->name) - 1] = '\0';
    
#ifdef VOX_OS_WINDOWS
    char pipe_name[256];
    snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\%s", name);
    
    pipe->handle = CreateFileA(
        pipe_name,
        read_only ? GENERIC_READ : GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );
    
    if (pipe->handle == INVALID_HANDLE_VALUE) {
        vox_mpool_free(mpool, pipe);
        return NULL;
    }
#else
    pipe->fd = open(name, read_only ? O_RDONLY : O_WRONLY);
    if (pipe->fd < 0) {
        vox_mpool_free(mpool, pipe);
        return NULL;
    }
#endif
    
    return pipe;
}

int64_t vox_named_pipe_read(vox_named_pipe_t* pipe, void* buffer, size_t size) {
    if (!pipe || !buffer || size == 0) return -1;
    if (!pipe->read_only) return -1;
    
#ifdef VOX_OS_WINDOWS
    DWORD bytes_read = 0;
    if (!ReadFile(pipe->handle, buffer, (DWORD)size, &bytes_read, NULL)) {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE) {
            return 0;  /* EOF */
        }
        return -1;
    }
    return (int64_t)bytes_read;
#else
    ssize_t bytes_read = read(pipe->fd, buffer, size);
    if (bytes_read < 0) {
        return -1;
    }
    return (int64_t)bytes_read;
#endif
}

int64_t vox_named_pipe_write(vox_named_pipe_t* pipe, const void* buffer, size_t size) {
    if (!pipe || !buffer || size == 0) return -1;
    if (pipe->read_only) return -1;
    
#ifdef VOX_OS_WINDOWS
    DWORD bytes_written = 0;
    if (!WriteFile(pipe->handle, buffer, (DWORD)size, &bytes_written, NULL)) {
        return -1;
    }
    return (int64_t)bytes_written;
#else
    ssize_t bytes_written = write(pipe->fd, buffer, size);
    if (bytes_written < 0) {
        return -1;
    }
    return (int64_t)bytes_written;
#endif
}

void vox_named_pipe_close(vox_named_pipe_t* pipe) {
    if (!pipe) return;
    
#ifdef VOX_OS_WINDOWS
    if (pipe->handle) {
        CloseHandle(pipe->handle);
        pipe->handle = NULL;
    }
#else
    if (pipe->fd >= 0) {
        close(pipe->fd);
        pipe->fd = -1;
    }
#endif
    
    vox_mpool_free(pipe->mpool, pipe);
}

int vox_named_pipe_unlink(const char* name) {
    if (!name) return -1;
    
#ifdef VOX_OS_WINDOWS
    /* Windows 命名管道会在所有句柄关闭后自动删除 */
    return 0;
#else
    return unlink(name) == 0 ? 0 : -1;
#endif
}

/* 进程间信号量结构 */
struct vox_ipc_semaphore {
    vox_mpool_t* mpool;
#ifdef VOX_OS_WINDOWS
    HANDLE sem;
    LONG count;  /* 信号量计数值（近似） */
#else
    sem_t* sem;
    bool created;
#endif
    char name[256];
};

vox_ipc_semaphore_t* vox_ipc_semaphore_create(vox_mpool_t* mpool, const char* name, 
                                               uint32_t initial_value, bool create) {
    if (!mpool || !name) return NULL;
    
    vox_ipc_semaphore_t* sem = (vox_ipc_semaphore_t*)vox_mpool_alloc(mpool, sizeof(vox_ipc_semaphore_t));
    if (!sem) return NULL;
    
    memset(sem, 0, sizeof(*sem));
    sem->mpool = mpool;
    strncpy(sem->name, name, sizeof(sem->name) - 1);
    sem->name[sizeof(sem->name) - 1] = '\0';
    
#ifdef VOX_OS_WINDOWS
    char sem_name[256];
    snprintf(sem_name, sizeof(sem_name), "Global\\%s", name);
    
    if (create) {
        sem->sem = CreateSemaphoreA(NULL, (LONG)initial_value, LONG_MAX, sem_name);
        if (!sem->sem) {
            vox_mpool_free(mpool, sem);
            return NULL;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            /* 已存在，打开它 */
            CloseHandle(sem->sem);
            sem->sem = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, sem_name);
            if (!sem->sem) {
                vox_mpool_free(mpool, sem);
                return NULL;
            }
            /* 无法获取已存在信号量的准确值，设为0 */
            sem->count = 0;
        } else {
            sem->count = (LONG)initial_value;
        }
    } else {
        sem->sem = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, sem_name);
        if (!sem->sem) {
            vox_mpool_free(mpool, sem);
            return NULL;
        }
        sem->count = 0;  /* 无法获取准确值 */
    }
#else
    char sem_name[256];
    snprintf(sem_name, sizeof(sem_name), "/%s", name);
    
    if (create) {
        sem_unlink(sem_name);  /* 如果已存在则删除 */
    }
    
    sem->sem = sem_open(sem_name, create ? (O_CREAT | O_EXCL) : 0, 0666, initial_value);
    if (sem->sem == SEM_FAILED) {
        vox_mpool_free(mpool, sem);
        return NULL;
    }
    
    sem->created = create;
#endif
    
    return sem;
}

int vox_ipc_semaphore_wait(vox_ipc_semaphore_t* sem) {
    if (!sem) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (WaitForSingleObject(sem->sem, INFINITE) == WAIT_OBJECT_0) {
        InterlockedDecrement(&sem->count);
        return 0;
    }
    return -1;
#else
    return sem_wait(sem->sem) == 0 ? 0 : -1;
#endif
}

int vox_ipc_semaphore_trywait(vox_ipc_semaphore_t* sem) {
    if (!sem) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (WaitForSingleObject(sem->sem, 0) == WAIT_OBJECT_0) {
        InterlockedDecrement(&sem->count);
        return 0;
    }
    return -1;
#else
    return sem_trywait(sem->sem) == 0 ? 0 : -1;
#endif
}

int vox_ipc_semaphore_timedwait(vox_ipc_semaphore_t* sem, uint32_t timeout_ms) {
    if (!sem) return -1;
    
#ifdef VOX_OS_WINDOWS
    DWORD timeout = (timeout_ms == 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD result = WaitForSingleObject(sem->sem, timeout);
    if (result == WAIT_OBJECT_0) {
        InterlockedDecrement(&sem->count);
        return 0;
    } else if (result == WAIT_TIMEOUT) {
        return 1;
    }
    return -1;
#elif defined(VOX_OS_LINUX)
    if (timeout_ms == 0) {
        return vox_ipc_semaphore_trywait(sem);
    }
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    
    int result = sem_timedwait(sem->sem, &ts);
    if (result == 0) {
        return 0;
    } else if (errno == ETIMEDOUT) {
        return 1;
    }
    return -1;
#else
    /* macOS 无 sem_timedwait，用轮询实现 */
    if (timeout_ms == 0) {
        return vox_ipc_semaphore_trywait(sem);
    }
    uint64_t deadline_us = (uint64_t)timeout_ms * 1000;
    struct timespec start;
    clock_gettime(CLOCK_REALTIME, &start);
    uint64_t start_us = (uint64_t)start.tv_sec * 1000000 + (uint64_t)start.tv_nsec / 1000;
    while (1) {
        if (sem_trywait(sem->sem) == 0) {
            return 0;
        }
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        uint64_t now_us = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
        if (now_us - start_us >= deadline_us) {
            return 1;  /* 超时 */
        }
        usleep(1000);
    }
#endif
}

int vox_ipc_semaphore_post(vox_ipc_semaphore_t* sem) {
    if (!sem) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (ReleaseSemaphore(sem->sem, 1, NULL)) {
        InterlockedIncrement(&sem->count);
        return 0;
    }
    return -1;
#else
    return sem_post(sem->sem) == 0 ? 0 : -1;
#endif
}

int vox_ipc_semaphore_get_value(vox_ipc_semaphore_t* sem) {
    if (!sem) return -1;
    
#ifdef VOX_OS_WINDOWS
    /* Windows 没有直接获取信号量值的API，返回近似值 */
    return (int)sem->count;
#else
    int value;
    #if defined(VOX_OS_MACOS)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
    #endif
    int ret = (sem_getvalue(sem->sem, &value) == 0) ? value : -1;
    #if defined(VOX_OS_MACOS)
    #pragma clang diagnostic pop
    #endif
    return ret;
#endif
}

void vox_ipc_semaphore_destroy(vox_ipc_semaphore_t* sem) {
    if (!sem) return;
    
#ifdef VOX_OS_WINDOWS
    if (sem->sem) {
        CloseHandle(sem->sem);
    }
#else
    if (sem->sem && sem->sem != SEM_FAILED) {
        sem_close(sem->sem);
    }
#endif
    
    vox_mpool_free(sem->mpool, sem);
}

int vox_ipc_semaphore_unlink(const char* name) {
    if (!name) return -1;
    
#ifdef VOX_OS_WINDOWS
    /* Windows 上信号量会在所有句柄关闭后自动删除 */
    return 0;
#else
    char sem_name[256];
    snprintf(sem_name, sizeof(sem_name), "/%s", name);
    return sem_unlink(sem_name) == 0 ? 0 : -1;
#endif
}

/* 进程间互斥锁结构 */
struct vox_ipc_mutex {
    vox_mpool_t* mpool;
#ifdef VOX_OS_WINDOWS
    HANDLE mutex;
#else
    sem_t* sem;  /* 使用信号量实现（值为1表示未锁定，0表示已锁定） */
    bool created;
#endif
    char name[256];
};

vox_ipc_mutex_t* vox_ipc_mutex_create(vox_mpool_t* mpool, const char* name, bool create) {
    if (!mpool || !name) return NULL;
    
    vox_ipc_mutex_t* mutex = (vox_ipc_mutex_t*)vox_mpool_alloc(mpool, sizeof(vox_ipc_mutex_t));
    if (!mutex) return NULL;
    
    memset(mutex, 0, sizeof(*mutex));
    mutex->mpool = mpool;
    strncpy(mutex->name, name, sizeof(mutex->name) - 1);
    mutex->name[sizeof(mutex->name) - 1] = '\0';
    
#ifdef VOX_OS_WINDOWS
    char mutex_name[256];
    snprintf(mutex_name, sizeof(mutex_name), "Global\\%s", name);
    
    if (create) {
        mutex->mutex = CreateMutexA(NULL, FALSE, mutex_name);
        if (!mutex->mutex) {
            vox_mpool_free(mpool, mutex);
            return NULL;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            /* 已存在，打开它 */
            CloseHandle(mutex->mutex);
            mutex->mutex = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, mutex_name);
            if (!mutex->mutex) {
                vox_mpool_free(mpool, mutex);
                return NULL;
            }
        }
    } else {
        mutex->mutex = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, mutex_name);
        if (!mutex->mutex) {
            vox_mpool_free(mpool, mutex);
            return NULL;
        }
    }
#else
    /* 使用信号量实现互斥锁（初始值为1） */
    char sem_name[256];
    snprintf(sem_name, sizeof(sem_name), "/%s_mutex", name);
    
    if (create) {
        sem_unlink(sem_name);
    }
    
    mutex->sem = sem_open(sem_name, create ? (O_CREAT | O_EXCL) : 0, 0666, 1);
    if (mutex->sem == SEM_FAILED) {
        vox_mpool_free(mpool, mutex);
        return NULL;
    }
    
    mutex->created = create;
#endif
    
    return mutex;
}

int vox_ipc_mutex_lock(vox_ipc_mutex_t* mutex) {
    if (!mutex) return -1;
    
#ifdef VOX_OS_WINDOWS
    return WaitForSingleObject(mutex->mutex, INFINITE) == WAIT_OBJECT_0 ? 0 : -1;
#else
    return sem_wait(mutex->sem) == 0 ? 0 : -1;
#endif
}

int vox_ipc_mutex_trylock(vox_ipc_mutex_t* mutex) {
    if (!mutex) return -1;
    
#ifdef VOX_OS_WINDOWS
    return WaitForSingleObject(mutex->mutex, 0) == WAIT_OBJECT_0 ? 0 : -1;
#else
    return sem_trywait(mutex->sem) == 0 ? 0 : -1;
#endif
}

int vox_ipc_mutex_unlock(vox_ipc_mutex_t* mutex) {
    if (!mutex) return -1;
    
#ifdef VOX_OS_WINDOWS
    return ReleaseMutex(mutex->mutex) ? 0 : -1;
#else
    return sem_post(mutex->sem) == 0 ? 0 : -1;
#endif
}

void vox_ipc_mutex_destroy(vox_ipc_mutex_t* mutex) {
    if (!mutex) return;
    
#ifdef VOX_OS_WINDOWS
    if (mutex->mutex) {
        CloseHandle(mutex->mutex);
    }
#else
    if (mutex->sem && mutex->sem != SEM_FAILED) {
        sem_close(mutex->sem);
    }
#endif
    
    vox_mpool_free(mutex->mpool, mutex);
}

int vox_ipc_mutex_unlink(const char* name) {
    if (!name) return -1;
    
#ifdef VOX_OS_WINDOWS
    /* Windows 上互斥锁会在所有句柄关闭后自动删除 */
    return 0;
#else
    char sem_name[256];
    snprintf(sem_name, sizeof(sem_name), "/%s_mutex", name);
    return sem_unlink(sem_name) == 0 ? 0 : -1;
#endif
}

/* 文件锁结构 */
struct vox_file_lock {
    vox_mpool_t* mpool;
    int fd;
    char file_path[512];
};

vox_file_lock_t* vox_file_lock_create(vox_mpool_t* mpool, const char* file_path) {
    if (!mpool || !file_path) return NULL;
    
    vox_file_lock_t* lock = (vox_file_lock_t*)vox_mpool_alloc(mpool, sizeof(vox_file_lock_t));
    if (!lock) return NULL;
    
    memset(lock, 0, sizeof(*lock));
    lock->mpool = mpool;
    strncpy(lock->file_path, file_path, sizeof(lock->file_path) - 1);
    lock->file_path[sizeof(lock->file_path) - 1] = '\0';
    
#ifdef VOX_OS_WINDOWS
    /* Windows 使用文件句柄 */
    HANDLE hFile = CreateFileA(file_path, GENERIC_READ | GENERIC_WRITE, 
                               0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        vox_mpool_free(mpool, lock);
        return NULL;
    }
    lock->fd = _open_osfhandle((intptr_t)hFile, _O_RDWR);
    if (lock->fd < 0) {
        CloseHandle(hFile);
        vox_mpool_free(mpool, lock);
        return NULL;
    }
#else
    lock->fd = open(file_path, O_RDWR | O_CREAT, 0666);
    if (lock->fd < 0) {
        vox_mpool_free(mpool, lock);
        return NULL;
    }
#endif
    
    return lock;
}

int vox_file_lock_lock(vox_file_lock_t* lock, bool exclusive) {
    if (!lock) return -1;
    
#ifdef VOX_OS_WINDOWS
    HANDLE hFile = (HANDLE)_get_osfhandle(lock->fd);
    if (hFile == INVALID_HANDLE_VALUE) return -1;
    
    OVERLAPPED overlapped = {0};
    DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    return LockFileEx(hFile, flags, 0, MAXDWORD, MAXDWORD, &overlapped) ? 0 : -1;
#else
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = exclusive ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(lock->fd, F_SETLKW, &fl) == 0 ? 0 : -1;
#endif
}

int vox_file_lock_trylock(vox_file_lock_t* lock, bool exclusive) {
    if (!lock) return -1;
    
#ifdef VOX_OS_WINDOWS
    HANDLE hFile = (HANDLE)_get_osfhandle(lock->fd);
    if (hFile == INVALID_HANDLE_VALUE) return -1;
    
    OVERLAPPED overlapped = {0};
    DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    flags |= LOCKFILE_FAIL_IMMEDIATELY;
    return LockFileEx(hFile, flags, 0, MAXDWORD, MAXDWORD, &overlapped) ? 0 : -1;
#else
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = exclusive ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(lock->fd, F_SETLK, &fl) == 0 ? 0 : -1;
#endif
}

int vox_file_lock_unlock(vox_file_lock_t* lock) {
    if (!lock) return -1;
    
#ifdef VOX_OS_WINDOWS
    HANDLE hFile = (HANDLE)_get_osfhandle(lock->fd);
    if (hFile == INVALID_HANDLE_VALUE) return -1;
    
    OVERLAPPED overlapped = {0};
    return UnlockFileEx(hFile, 0, MAXDWORD, MAXDWORD, &overlapped) ? 0 : -1;
#else
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(lock->fd, F_SETLK, &fl) == 0 ? 0 : -1;
#endif
}

void vox_file_lock_destroy(vox_file_lock_t* lock) {
    if (!lock) return;
    
    if (lock->fd >= 0) {
#ifdef VOX_OS_WINDOWS
        HANDLE hFile = (HANDLE)_get_osfhandle(lock->fd);
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
        }
        _close(lock->fd);
#else
        close(lock->fd);
#endif
    }
    
    vox_mpool_free(lock->mpool, lock);
}

/* ===== 信号处理 ===== */

/* 信号常量定义（Windows兼容） */
#ifdef VOX_OS_WINDOWS
    #ifndef SIGINT
        #define SIGINT 2
    #endif
    #ifndef SIGTERM
        #define SIGTERM 15
    #endif
    #ifndef SIGKILL
        #define SIGKILL 9
    #endif
#endif

static vox_signal_handler_t g_signal_handlers[32] = {0};

#ifdef VOX_OS_WINDOWS
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        if (g_signal_handlers[SIGINT]) {
            g_signal_handlers[SIGINT](SIGINT);
            return TRUE;
        }
    }
    return FALSE;
}
#endif

int vox_process_signal_register(int signal, vox_signal_handler_t handler) {
    if (signal < 0 || signal >= 32) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (signal == SIGINT || signal == SIGTERM) {
        g_signal_handlers[signal] = handler;
        if (handler) {
            SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
        } else {
            SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
        }
        return 0;
    }
    return -1;  /* Windows 不支持其他信号 */
#else
    g_signal_handlers[signal] = handler;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    if (handler) {
        sa.sa_handler = handler;
    } else {
        sa.sa_handler = SIG_DFL;
    }
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    return sigaction(signal, &sa, NULL) == 0 ? 0 : -1;
#endif
}

int vox_process_signal_reset(int signal) {
    return vox_process_signal_register(signal, NULL);
}

int vox_process_signal_ignore(int signal) {
    if (signal < 0 || signal >= 32) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (signal == SIGINT || signal == SIGTERM) {
        g_signal_handlers[signal] = NULL;
        SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
        return 0;
    }
    return -1;
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    return sigaction(signal, &sa, NULL) == 0 ? 0 : -1;
#endif
}

int vox_process_signal_send(vox_process_id_t pid, int signal) {
    if (pid == 0) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (signal == SIGTERM || signal == SIGKILL) {
        HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!hProcess) return -1;
        BOOL result = TerminateProcess(hProcess, 1);
        CloseHandle(hProcess);
        return result ? 0 : -1;
    }
    return -1;
#else
    return kill(pid, signal) == 0 ? 0 : -1;
#endif
}

int vox_process_signal_send_group(vox_process_id_t pgid, int signal) {
#ifdef VOX_OS_WINDOWS
    /* Windows 不支持进程组 */
    VOX_UNUSED(pgid);
    VOX_UNUSED(signal);
    return -1;
#else
    if (pgid == 0) {
        pgid = getpgrp();
    }
    return killpg(pgid, signal) == 0 ? 0 : -1;
#endif
}

/* ===== 进程池管理 ===== */

/* 进程池结构 */
struct vox_process_pool {
    vox_mpool_t* mpool;
    vox_process_t** workers;
    uint32_t worker_count;
    bool auto_restart;
    uint32_t max_restarts;
    uint32_t* restart_count;
    void* worker_data;
};

vox_process_pool_t* vox_process_pool_create(vox_mpool_t* mpool, 
                                             const vox_process_pool_config_t* config) {
    if (!mpool || !config || config->worker_count == 0) return NULL;
    
    vox_process_pool_t* pool = (vox_process_pool_t*)vox_mpool_alloc(mpool, sizeof(vox_process_pool_t));
    if (!pool) return NULL;
    
    memset(pool, 0, sizeof(*pool));
    pool->mpool = mpool;
    pool->worker_count = config->worker_count;
    pool->auto_restart = config->auto_restart;
    pool->max_restarts = config->max_restarts;
    pool->worker_data = config->worker_data;
    
    pool->workers = (vox_process_t**)vox_mpool_alloc(mpool, 
                                                      config->worker_count * sizeof(vox_process_t*));
    if (!pool->workers) {
        vox_mpool_free(mpool, pool);
        return NULL;
    }
    
    if (config->auto_restart) {
        pool->restart_count = (uint32_t*)vox_mpool_alloc(mpool, 
                                                          config->worker_count * sizeof(uint32_t));
        if (!pool->restart_count) {
            vox_mpool_free(mpool, pool->workers);
            vox_mpool_free(mpool, pool);
            return NULL;
        }
        memset(pool->restart_count, 0, config->worker_count * sizeof(uint32_t));
    }
    
    /* 创建工作进程 */
    for (uint32_t i = 0; i < config->worker_count; i++) {
        if (config->worker_command) {
            pool->workers[i] = vox_process_create(mpool, config->worker_command, 
                                                  config->worker_argv, NULL);
        } else {
            /* 使用当前程序作为工作进程 */
            pool->workers[i] = NULL;  /* 需要特殊处理 */
        }
        if (!pool->workers[i] && config->worker_command) {
            /* 创建失败，清理已创建的进程 */
            for (uint32_t j = 0; j < i; j++) {
                if (pool->workers[j]) {
                    vox_process_terminate(pool->workers[j], true);
                    vox_process_destroy(pool->workers[j]);
                }
            }
            if (pool->restart_count) vox_mpool_free(mpool, pool->restart_count);
            vox_mpool_free(mpool, pool->workers);
            vox_mpool_free(mpool, pool);
            return NULL;
        }
    }
    
    return pool;
}

int vox_process_pool_submit(vox_process_pool_t* pool, 
                            vox_process_pool_task_t task_func, 
                            void* task_data) {
    /* 简化实现：进程池通常需要更复杂的任务队列机制 */
    (void)pool;
    (void)task_func;
    (void)task_data;
    return -1;  /* 待实现 */
}

int vox_process_pool_wait(vox_process_pool_t* pool, uint32_t timeout_ms) {
    if (!pool) return -1;
    
    /* 等待所有工作进程 */
    for (uint32_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i]) {
            vox_process_status_t status;
            vox_process_wait(pool->workers[i], &status, timeout_ms);
        }
    }
    
    return 0;
}

int vox_process_pool_get_status(vox_process_pool_t* pool, 
                                 uint32_t* active_workers, 
                                 uint32_t* pending_tasks) {
    if (!pool) return -1;
    
    if (active_workers) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < pool->worker_count; i++) {
            if (pool->workers[i] && vox_process_is_running(pool->workers[i])) {
                count++;
            }
        }
        *active_workers = count;
    }
    
    if (pending_tasks) {
        *pending_tasks = 0;  /* 待实现任务队列 */
    }
    
    return 0;
}

int vox_process_pool_stop(vox_process_pool_t* pool) {
    if (!pool) return -1;
    
    /* 终止所有工作进程 */
    for (uint32_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i]) {
            vox_process_terminate(pool->workers[i], false);
        }
    }
    
    /* 等待所有进程结束 */
    vox_process_pool_wait(pool, 5000);
    
    /* 强制终止仍在运行的进程 */
    for (uint32_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i] && vox_process_is_running(pool->workers[i])) {
            vox_process_terminate(pool->workers[i], true);
        }
    }
    
    return 0;
}

void vox_process_pool_destroy(vox_process_pool_t* pool) {
    if (!pool) return;
    
    vox_process_pool_stop(pool);
    
    for (uint32_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i]) {
            vox_process_destroy(pool->workers[i]);
        }
    }
    
    if (pool->workers) vox_mpool_free(pool->mpool, pool->workers);
    if (pool->restart_count) vox_mpool_free(pool->mpool, pool->restart_count);
    vox_mpool_free(pool->mpool, pool);
}

/* ===== 进程组管理 ===== */

vox_process_id_t vox_process_group_create(void) {
#ifdef VOX_OS_WINDOWS
    /* Windows 不支持进程组，返回当前进程ID */
    return GetCurrentProcessId();
#else
    /* setsid() 在已经是会话领导者的进程中会失败
     * 这是正常行为，返回当前进程组ID */
    pid_t pgid = setsid();
    if (pgid < 0) {
        /* 如果失败，可能是已经是会话领导者，返回当前进程组ID */
        pgid = getpgrp();
    }
    return (pgid > 0) ? (vox_process_id_t)pgid : 0;
#endif
}

vox_process_id_t vox_process_group_get_current(void) {
#ifdef VOX_OS_WINDOWS
    return GetCurrentProcessId();
#else
    return (vox_process_id_t)getpgrp();
#endif
}

int vox_process_group_set(vox_process_id_t pid, vox_process_id_t pgid) {
#ifdef VOX_OS_WINDOWS
    /* Windows 不支持进程组 */
    VOX_UNUSED(pid);
    VOX_UNUSED(pgid);
    return -1;
#else
    if (pid == 0) {
        pid = getpid();
    }
    if (pgid == 0) {
        pgid = (vox_process_id_t)pid;
    }
    return setpgid((pid_t)pid, (pid_t)pgid) == 0 ? 0 : -1;
#endif
}

int vox_process_group_signal(vox_process_id_t pgid, int signal) {
    return vox_process_signal_send_group(pgid, signal);
}
