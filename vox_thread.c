/*
 * vox_thread.c - 跨平台线程和线程本地存储实现
 */

/* 必须在包含任何头文件之前定义 _GNU_SOURCE，以使用 pthread_setaffinity_np */
#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif

#include "vox_thread.h"
#include "vox_os.h"
#include "vox_mpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef VOX_OS_WINDOWS
    #include <process.h>
#else
    #include <pthread.h>
    #include <unistd.h>
    #include <sched.h>
    #include <sys/resource.h>
    #include <errno.h>
    #ifdef VOX_OS_LINUX
        #include <sys/syscall.h>
    #endif
#endif

/* ===== 线程结构 ===== */

#ifdef VOX_OS_WINDOWS
struct vox_thread {
    vox_mpool_t* mpool;         /* 内存池指针 */
    HANDLE handle;              /* Windows线程句柄 */
    vox_thread_id_t id;         /* 线程ID */
    vox_thread_func_t func;     /* 线程函数 */
    void* user_data;            /* 用户数据 */
    int exit_code;              /* 退出码 */
};

/* Windows线程入口函数 */
static unsigned __stdcall thread_wrapper(void* arg) {
    vox_thread_t* thread = (vox_thread_t*)arg;
    thread->exit_code = thread->func(thread->user_data);
    return (unsigned)thread->exit_code;
}
#else
struct vox_thread {
    vox_mpool_t* mpool;         /* 内存池指针 */
    pthread_t thread;           /* POSIX线程 */
    vox_thread_id_t id;         /* 线程ID */
    vox_thread_func_t func;     /* 线程函数 */
    void* user_data;            /* 用户数据 */
    int exit_code;              /* 退出码 */
    bool detached;              /* 是否已分离 */
};

/* POSIX线程入口函数 */
static void* thread_wrapper(void* arg) {
    vox_thread_t* thread = (vox_thread_t*)arg;
    thread->exit_code = thread->func(thread->user_data);
    return (void*)(intptr_t)thread->exit_code;
}
#endif

/* ===== 线程函数实现 ===== */

vox_thread_t* vox_thread_create(vox_mpool_t* mpool, vox_thread_func_t func, void* user_data) {
    if (!mpool || !func) return NULL;
    
    vox_thread_t* thread = (vox_thread_t*)vox_mpool_alloc(mpool, sizeof(vox_thread_t));
    if (!thread) return NULL;
    
    memset(thread, 0, sizeof(vox_thread_t));
    thread->mpool = mpool;
    thread->func = func;
    thread->user_data = user_data;
    thread->exit_code = -1;
    
#ifdef VOX_OS_WINDOWS
    thread->handle = (HANDLE)_beginthreadex(
        NULL,                    /* 安全属性 */
        0,                       /* 栈大小（默认） */
        thread_wrapper,          /* 入口函数 */
        thread,                  /* 参数 */
        0,                       /* 创建标志（立即运行） */
        (unsigned int*)&thread->id  /* 线程ID */
    );
    
    if (!thread->handle) {
        vox_mpool_free(mpool, thread);
        return NULL;
    }
#else
    thread->detached = false;
    if (pthread_create(&thread->thread, NULL, thread_wrapper, thread) != 0) {
        vox_mpool_free(mpool, thread);
        return NULL;
    }
    
    /* 获取线程ID */
    thread->id = (vox_thread_id_t)(uintptr_t)thread->thread;
#endif
    
    return thread;
}

int vox_thread_join(vox_thread_t* thread, int* exit_code) {
    if (!thread) return -1;
    
    vox_mpool_t* mpool = thread->mpool;
    
#ifdef VOX_OS_WINDOWS
    if (WaitForSingleObject(thread->handle, INFINITE) != WAIT_OBJECT_0) {
        return -1;
    }
    
    if (exit_code) {
        *exit_code = thread->exit_code;
    }
    
    CloseHandle(thread->handle);
    vox_mpool_free(mpool, thread);
    return 0;
#else
    void* retval;
    if (pthread_join(thread->thread, &retval) != 0) {
        return -1;
    }
    
    if (exit_code) {
        *exit_code = thread->exit_code;
    }
    
    vox_mpool_free(mpool, thread);
    return 0;
#endif
}

int vox_thread_detach(vox_thread_t* thread) {
    if (!thread) return -1;
    
    vox_mpool_t* mpool = thread->mpool;
    
#ifdef VOX_OS_WINDOWS
    /* Windows线程在创建后自动可分离，只需关闭句柄 */
    CloseHandle(thread->handle);
    vox_mpool_free(mpool, thread);
    return 0;
#else
    if (pthread_detach(thread->thread) != 0) {
        return -1;
    }
    
    thread->detached = true;
    vox_mpool_free(mpool, thread);
    return 0;
#endif
}

vox_thread_id_t vox_thread_id(const vox_thread_t* thread) {
    if (!thread) return 0;
    return thread->id;
}

vox_thread_id_t vox_thread_self(void) {
#ifdef VOX_OS_WINDOWS
    return (vox_thread_id_t)GetCurrentThreadId();
#else
    /* 在POSIX系统中，pthread_t可能不是简单整数
     * 我们使用pthread_self()的指针值作为ID
     * 注意：这只在同一进程内有效，不能跨进程比较 */
    pthread_t self = pthread_self();
    return (vox_thread_id_t)(uintptr_t)self;
#endif
}

bool vox_thread_id_equal(vox_thread_id_t id1, vox_thread_id_t id2) {
#ifdef VOX_OS_WINDOWS
    return id1 == id2;
#else
    /* 在POSIX系统中，使用pthread_equal进行比较 */
    pthread_t t1 = (pthread_t)(uintptr_t)id1;
    pthread_t t2 = (pthread_t)(uintptr_t)id2;
    return pthread_equal(t1, t2) != 0;
#endif
}

void vox_thread_yield(void) {
#ifdef VOX_OS_WINDOWS
    SwitchToThread();
#else
    sched_yield();
#endif
}

void vox_thread_sleep(uint32_t ms) {
#ifdef VOX_OS_WINDOWS
    Sleep(ms);
#else
    usleep(ms * 1000);  /* usleep使用微秒 */
#endif
}

/* ===== 线程优先级 ===== */

int vox_thread_set_priority(vox_thread_t* thread, vox_thread_priority_t priority) {
    if (priority < VOX_THREAD_PRIORITY_LOWEST || priority > VOX_THREAD_PRIORITY_TIME_CRITICAL) {
        return -1;
    }
    
#ifdef VOX_OS_WINDOWS
    HANDLE handle;
    if (thread) {
        handle = thread->handle;
    } else {
        handle = GetCurrentThread();
    }
    
    int win_priority;
    switch (priority) {
        case VOX_THREAD_PRIORITY_LOWEST:
            win_priority = THREAD_PRIORITY_LOWEST;
            break;
        case VOX_THREAD_PRIORITY_BELOW_NORMAL:
            win_priority = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case VOX_THREAD_PRIORITY_NORMAL:
            win_priority = THREAD_PRIORITY_NORMAL;
            break;
        case VOX_THREAD_PRIORITY_ABOVE_NORMAL:
            win_priority = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case VOX_THREAD_PRIORITY_HIGHEST:
            win_priority = THREAD_PRIORITY_HIGHEST;
            break;
        case VOX_THREAD_PRIORITY_TIME_CRITICAL:
            win_priority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
        default:
            return -1;
    }
    
    if (SetThreadPriority(handle, win_priority) == 0) {
        return -1;
    }
    return 0;
#else
    pthread_t pthread;
    if (thread) {
        pthread = thread->thread;
    } else {
        pthread = pthread_self();
    }
    
    /* Linux上使用nice值来设置优先级，不需要root权限 */
    /* nice值范围：-20（最高优先级）到19（最低优先级） */
    int nice_value;
    int use_realtime = 0;
    
    switch (priority) {
        case VOX_THREAD_PRIORITY_LOWEST:
            nice_value = 19;
            break;
        case VOX_THREAD_PRIORITY_BELOW_NORMAL:
            nice_value = 10;
            break;
        case VOX_THREAD_PRIORITY_NORMAL:
            nice_value = 0;
            break;
        case VOX_THREAD_PRIORITY_ABOVE_NORMAL:
            nice_value = -10;
            break;
        case VOX_THREAD_PRIORITY_HIGHEST:
            nice_value = -19;
            break;
        case VOX_THREAD_PRIORITY_TIME_CRITICAL:
            /* TIME_CRITICAL需要实时调度策略，需要root权限 */
            use_realtime = 1;
            break;
        default:
            return -1;
    }
    
    if (use_realtime) {
        /* 尝试设置实时调度策略 */
        struct sched_param param;
        param.sched_priority = sched_get_priority_max(SCHED_FIFO);
        if (pthread_setschedparam(pthread, SCHED_FIFO, &param) == 0) {
            return 0;
        }
        /* 如果失败（没有权限），回退到最高nice值 */
        nice_value = -20;
    }
    
    /* 使用setpriority设置nice值 */
    #ifdef VOX_OS_LINUX
        /* 在Linux上，使用gettid()获取线程ID */
        /* 注意：setpriority的PRIO_PROCESS选项在某些系统上可能不支持线程级别的设置 */
        /* 我们尝试使用gettid()获取线程ID，如果失败则使用getpid()获取进程ID */
        pid_t tid = (pid_t)syscall(SYS_gettid);
        int saved_errno;
        
        if (tid > 0) {
            /* 尝试使用线程ID设置优先级 */
            errno = 0;
            if (setpriority(PRIO_PROCESS, tid, nice_value) == 0) {
                return 0;
            }
            saved_errno = errno;
        } else {
            saved_errno = EINVAL;
        }
        
        /* 如果使用线程ID失败，尝试使用进程ID（会影响整个进程） */
        errno = 0;
        pid_t pid = getpid();
        if (setpriority(PRIO_PROCESS, pid, nice_value) == 0) {
            return 0;
        }
        saved_errno = errno;
        
        /* 如果都失败，检查是否是正常优先级（nice=0）且当前已经是0 */
        /* 这种情况下，即使setpriority返回错误，实际上优先级已经是正确的 */
        if (nice_value == 0) {
            errno = 0;
            int current_nice = getpriority(PRIO_PROCESS, 0);
            /* getpriority成功时返回nice值（-20到19），失败时返回-1并设置errno */
            if (errno == 0 && current_nice == 0) {
                /* 当前nice值已经是0，设置成功 */
                return 0;
            }
            /* 如果无法获取当前nice值，但我们要设置的是0（默认值），也可以认为成功 */
            if (errno != 0 && saved_errno == EPERM) {
                /* 权限不足，但对于正常优先级（默认值），可以认为已经是正确的 */
                return 0;
            }
        }
        
        /* 对于提高优先级（nice < 0），如果失败且是权限问题，这是预期的 */
        /* 但对于降低优先级（nice > 0），通常不应该失败 */
        if (nice_value > 0 && saved_errno == EPERM) {
            /* 降低优先级不应该需要特殊权限，如果失败可能是其他原因 */
            return -1;
        }
        
        /* setpriority失败，返回错误 */
        return -1;
    #else
        /* 非Linux系统，无法设置 per-thread nice，NORMAL 视为默认即成功 */
        (void)nice_value;
        if (!use_realtime) {
            if (priority == VOX_THREAD_PRIORITY_NORMAL) {
                return 0;  /* 默认即为 NORMAL，视为成功 */
            }
            return -1;
        }
        /* 实时优先级已经在上面处理了 */
        return -1;
    #endif
#endif
}

int vox_thread_get_priority(vox_thread_t* thread, vox_thread_priority_t* priority) {
    if (!priority) return -1;
    
#ifdef VOX_OS_WINDOWS
    HANDLE handle;
    if (thread) {
        handle = thread->handle;
    } else {
        handle = GetCurrentThread();
    }
    
    int win_priority = GetThreadPriority(handle);
    if (win_priority == THREAD_PRIORITY_ERROR_RETURN) {
        return -1;
    }
    
    /* 将Windows优先级转换为vox_thread_priority_t */
    switch (win_priority) {
        case THREAD_PRIORITY_LOWEST:
            *priority = VOX_THREAD_PRIORITY_LOWEST;
            break;
        case THREAD_PRIORITY_BELOW_NORMAL:
            *priority = VOX_THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case THREAD_PRIORITY_NORMAL:
            *priority = VOX_THREAD_PRIORITY_NORMAL;
            break;
        case THREAD_PRIORITY_ABOVE_NORMAL:
            *priority = VOX_THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case THREAD_PRIORITY_HIGHEST:
            *priority = VOX_THREAD_PRIORITY_HIGHEST;
            break;
        case THREAD_PRIORITY_TIME_CRITICAL:
            *priority = VOX_THREAD_PRIORITY_TIME_CRITICAL;
            break;
        default:
            *priority = VOX_THREAD_PRIORITY_NORMAL;
            break;
    }
    return 0;
#else
    pthread_t pthread;
    if (thread) {
        pthread = thread->thread;
    } else {
        pthread = pthread_self();
    }
    
    int policy;
    struct sched_param param;
    if (pthread_getschedparam(pthread, &policy, &param) != 0) {
        return -1;
    }
    
    /* 将Linux优先级转换为vox_thread_priority_t */
    int prio = param.sched_priority;
    if (prio >= 15) {
        *priority = VOX_THREAD_PRIORITY_LOWEST;
    } else if (prio >= 5) {
        *priority = VOX_THREAD_PRIORITY_BELOW_NORMAL;
    } else if (prio >= -5) {
        *priority = VOX_THREAD_PRIORITY_NORMAL;
    } else if (prio >= -15) {
        *priority = VOX_THREAD_PRIORITY_ABOVE_NORMAL;
    } else if (prio >= -19) {
        *priority = VOX_THREAD_PRIORITY_HIGHEST;
    } else {
        *priority = VOX_THREAD_PRIORITY_TIME_CRITICAL;
    }
    return 0;
#endif
}

/* ===== CPU亲和力 ===== */

int vox_thread_set_affinity(vox_thread_t* thread, uint64_t cpu_mask) {
#ifdef VOX_OS_WINDOWS
    HANDLE handle;
    if (thread) {
        handle = thread->handle;
    } else {
        handle = GetCurrentThread();
    }
    
    /* Windows使用DWORD_PTR作为CPU掩码，最多支持64个CPU */
    DWORD_PTR mask = (DWORD_PTR)cpu_mask;
    if (SetThreadAffinityMask(handle, mask) == 0) {
        return -1;
    }
    return 0;
#elif defined(VOX_OS_LINUX)
    pthread_t pthread;
    if (thread) {
        pthread = thread->thread;
    } else {
        pthread = pthread_self();
    }
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    /* 将uint64_t掩码转换为cpu_set_t */
    for (int i = 0; i < 64 && i < CPU_SETSIZE; i++) {
        if (cpu_mask & (1ULL << i)) {
            CPU_SET(i, &cpuset);
        }
    }
    
    if (pthread_setaffinity_np(pthread, sizeof(cpu_set_t), &cpuset) != 0) {
        return -1;
    }
    return 0;
#else
    /* macOS/BSD 等不支持 pthread_setaffinity_np */
    (void)thread;
    (void)cpu_mask;
    return -1;
#endif
}

int vox_thread_get_affinity(vox_thread_t* thread, uint64_t* cpu_mask) {
    if (!cpu_mask) return -1;
    
#ifdef VOX_OS_WINDOWS
    HANDLE handle;
    if (thread) {
        handle = thread->handle;
    } else {
        handle = GetCurrentThread();
    }
    
    DWORD_PTR mask = SetThreadAffinityMask(handle, (DWORD_PTR)-1);
    if (mask == 0) {
        return -1;
    }
    
    /* 恢复原来的掩码 */
    SetThreadAffinityMask(handle, mask);
    
    *cpu_mask = (uint64_t)mask;
    return 0;
#elif defined(VOX_OS_LINUX)
    pthread_t pthread;
    if (thread) {
        pthread = thread->thread;
    } else {
        pthread = pthread_self();
    }
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    if (pthread_getaffinity_np(pthread, sizeof(cpu_set_t), &cpuset) != 0) {
        return -1;
    }
    
    /* 将cpu_set_t转换为uint64_t掩码 */
    *cpu_mask = 0;
    for (int i = 0; i < 64 && i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &cpuset)) {
            *cpu_mask |= (1ULL << i);
        }
    }
    return 0;
#else
    /* macOS/BSD 等不支持 pthread_getaffinity_np */
    (void)thread;
    return -1;
#endif
}

/* ===== 线程本地存储 (TLS) ===== */

#ifdef VOX_OS_WINDOWS
struct vox_tls_key {
    vox_mpool_t* mpool;         /* 内存池指针 */
    DWORD key;                  /* Windows TLS索引 */
    void (*destructor)(void*);   /* 析构函数 */
};
#else
struct vox_tls_key {
    vox_mpool_t* mpool;         /* 内存池指针 */
    pthread_key_t key;          /* POSIX TLS键 */
    void (*destructor)(void*);   /* 析构函数 */
};
#endif

vox_tls_key_t* vox_tls_key_create(vox_mpool_t* mpool, void (*destructor)(void*)) {
    if (!mpool) return NULL;
    
    vox_tls_key_t* tls = (vox_tls_key_t*)vox_mpool_alloc(mpool, sizeof(vox_tls_key_t));
    if (!tls) return NULL;
    
    tls->mpool = mpool;
    tls->destructor = destructor;
    
#ifdef VOX_OS_WINDOWS
    tls->key = TlsAlloc();
    if (tls->key == TLS_OUT_OF_INDEXES) {
        vox_mpool_free(mpool, tls);
        return NULL;
    }
#else
    if (pthread_key_create(&tls->key, destructor) != 0) {
        vox_mpool_free(mpool, tls);
        return NULL;
    }
#endif
    
    return tls;
}

int vox_tls_set(vox_tls_key_t* tls, void* value) {
    if (!tls) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (TlsSetValue(tls->key, value) == 0) {
        return -1;
    }
    return 0;
#else
    if (pthread_setspecific(tls->key, value) != 0) {
        return -1;
    }
    return 0;
#endif
}

void* vox_tls_get(vox_tls_key_t* tls) {
    if (!tls) return NULL;
    
#ifdef VOX_OS_WINDOWS
    return TlsGetValue(tls->key);
#else
    return pthread_getspecific(tls->key);
#endif
}

void vox_tls_key_destroy(vox_tls_key_t* tls) {
    if (!tls) return;
    
    vox_mpool_t* mpool = tls->mpool;
    
#ifdef VOX_OS_WINDOWS
    TlsFree(tls->key);
#else
    pthread_key_delete(tls->key);
#endif
    
    vox_mpool_free(mpool, tls);
}
