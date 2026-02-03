/*
 * vox_mutex.h - 跨平台线程同步原语抽象API
 * 提供互斥锁、读写锁、递归锁、自旋锁、信号量等同步机制
 */

#ifndef VOX_MUTEX_H
#define VOX_MUTEX_H

#include "vox_os.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef VOX_OS_WINDOWS
    #include <synchapi.h>
#else
    #include <pthread.h>
    #include <semaphore.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 互斥锁 (Mutex) ===== */

#ifdef VOX_OS_WINDOWS
struct vox_mutex {
    CRITICAL_SECTION cs;
};
#else
struct vox_mutex {
    pthread_mutex_t mutex;
};
#endif

typedef struct vox_mutex vox_mutex_t;

/**
 * 创建互斥锁（初始化结构体）
 * @param mutex 互斥锁结构体指针，必须非NULL
 * @return 成功返回0，失败返回-1
 */
int vox_mutex_create(vox_mutex_t* mutex);

/**
 * 销毁互斥锁（清理资源，不释放内存）
 * @param mutex 互斥锁指针
 */
void vox_mutex_destroy(vox_mutex_t* mutex);

/**
 * 加锁（阻塞）
 * @param mutex 互斥锁指针
 * @return 成功返回0，失败返回-1
 */
int vox_mutex_lock(vox_mutex_t* mutex);

/**
 * 尝试加锁（非阻塞）
 * @param mutex 互斥锁指针
 * @return 成功返回0，锁已被占用返回-1
 */
int vox_mutex_trylock(vox_mutex_t* mutex);

/**
 * 解锁
 * @param mutex 互斥锁指针
 * @return 成功返回0，失败返回-1
 */
int vox_mutex_unlock(vox_mutex_t* mutex);

/* ===== 读写锁 (Read-Write Lock) ===== */

#ifdef VOX_OS_WINDOWS
struct vox_rwlock {
    SRWLOCK srwlock;
};
#else
struct vox_rwlock {
    pthread_rwlock_t rwlock;
};
#endif

typedef struct vox_rwlock vox_rwlock_t;

/**
 * 创建读写锁（初始化结构体）
 * @param rwlock 读写锁结构体指针，必须非NULL
 * @return 成功返回0，失败返回-1
 */
int vox_rwlock_create(vox_rwlock_t* rwlock);

/**
 * 销毁读写锁（清理资源，不释放内存）
 * @param rwlock 读写锁指针
 */
void vox_rwlock_destroy(vox_rwlock_t* rwlock);

/**
 * 获取读锁（阻塞）
 * @param rwlock 读写锁指针
 * @return 成功返回0，失败返回-1
 */
int vox_rwlock_rdlock(vox_rwlock_t* rwlock);

/**
 * 尝试获取读锁（非阻塞）
 * @param rwlock 读写锁指针
 * @return 成功返回0，失败返回-1
 */
int vox_rwlock_tryrdlock(vox_rwlock_t* rwlock);

/**
 * 获取写锁（阻塞）
 * @param rwlock 读写锁指针
 * @return 成功返回0，失败返回-1
 */
int vox_rwlock_wrlock(vox_rwlock_t* rwlock);

/**
 * 尝试获取写锁（非阻塞）
 * @param rwlock 读写锁指针
 * @return 成功返回0，失败返回-1
 */
int vox_rwlock_trywrlock(vox_rwlock_t* rwlock);

/**
 * 解锁（读锁或写锁）
 * @param rwlock 读写锁指针
 * @return 成功返回0，失败返回-1
 */
int vox_rwlock_unlock(vox_rwlock_t* rwlock);

/* ===== 递归锁 (Recursive Mutex) ===== */

#ifdef VOX_OS_WINDOWS
struct vox_rmutex {
    CRITICAL_SECTION cs;
};
#else
struct vox_rmutex {
    pthread_mutex_t mutex;
};
#endif

typedef struct vox_rmutex vox_rmutex_t;

/**
 * 创建递归锁（初始化结构体）
 * @param rmutex 递归锁结构体指针，必须非NULL
 * @return 成功返回0，失败返回-1
 */
int vox_rmutex_create(vox_rmutex_t* rmutex);

/**
 * 销毁递归锁（清理资源，不释放内存）
 * @param rmutex 递归锁指针
 */
void vox_rmutex_destroy(vox_rmutex_t* rmutex);

/**
 * 加锁（阻塞，同一线程可重复加锁）
 * @param rmutex 递归锁指针
 * @return 成功返回0，失败返回-1
 */
int vox_rmutex_lock(vox_rmutex_t* rmutex);

/**
 * 尝试加锁（非阻塞）
 * @param rmutex 递归锁指针
 * @return 成功返回0，锁已被占用返回-1
 */
int vox_rmutex_trylock(vox_rmutex_t* rmutex);

/**
 * 解锁
 * @param rmutex 递归锁指针
 * @return 成功返回0，失败返回-1
 */
int vox_rmutex_unlock(vox_rmutex_t* rmutex);

/* ===== 自旋锁 (Spinlock) ===== */

#ifdef VOX_OS_WINDOWS
struct vox_spinlock {
    CRITICAL_SECTION cs;
};
#elif defined(VOX_OS_LINUX)
/* Linux 提供 pthread_spinlock_t */
struct vox_spinlock {
    pthread_spinlock_t spinlock;
};
#else
/* macOS/BSD 等无 pthread_spinlock，用互斥锁替代 */
struct vox_spinlock {
    pthread_mutex_t mutex;
};
#endif

typedef struct vox_spinlock vox_spinlock_t;

/**
 * 创建自旋锁（初始化结构体）
 * @param spinlock 自旋锁结构体指针，必须非NULL
 * @return 成功返回0，失败返回-1
 */
int vox_spinlock_create(vox_spinlock_t* spinlock);

/**
 * 销毁自旋锁（清理资源，不释放内存）
 * @param spinlock 自旋锁指针
 */
void vox_spinlock_destroy(vox_spinlock_t* spinlock);

/**
 * 加锁（自旋等待）
 * @param spinlock 自旋锁指针
 */
void vox_spinlock_lock(vox_spinlock_t* spinlock);

/**
 * 尝试加锁（非阻塞）
 * @param spinlock 自旋锁指针
 * @return 成功返回true，锁已被占用返回false
 */
bool vox_spinlock_trylock(vox_spinlock_t* spinlock);

/**
 * 解锁
 * @param spinlock 自旋锁指针
 */
void vox_spinlock_unlock(vox_spinlock_t* spinlock);

/* ===== 信号量 (Semaphore) ===== */

#ifdef VOX_OS_WINDOWS
struct vox_semaphore {
    HANDLE sem;
    volatile LONG count;  /* 用于获取当前值（近似值） */
};
#elif defined(VOX_OS_MACOS)
/* macOS 不支持 unnamed sem_t，用 mutex+cond 实现 */
struct vox_semaphore {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint32_t count;
};
#else
struct vox_semaphore {
    sem_t sem;
};
#endif

typedef struct vox_semaphore vox_semaphore_t;

/**
 * 创建信号量（初始化结构体）
 * @param sem 信号量结构体指针，必须非NULL
 * @param initial_value 初始值
 * @return 成功返回0，失败返回-1
 */
int vox_semaphore_create(vox_semaphore_t* sem, uint32_t initial_value);

/**
 * 销毁信号量（清理资源，不释放内存）
 * @param sem 信号量指针
 */
void vox_semaphore_destroy(vox_semaphore_t* sem);

/**
 * 等待信号量（P操作，减1）
 * @param sem 信号量指针
 * @return 成功返回0，失败返回-1
 */
int vox_semaphore_wait(vox_semaphore_t* sem);

/**
 * 尝试等待信号量（非阻塞）
 * @param sem 信号量指针
 * @return 成功返回0，信号量为0返回-1
 */
int vox_semaphore_trywait(vox_semaphore_t* sem);

/**
 * 等待信号量（带超时，毫秒）
 * @param sem 信号量指针
 * @param timeout_ms 超时时间（毫秒），0表示不等待，-1表示无限等待
 * @return 成功返回0，超时返回-1
 */
int vox_semaphore_timedwait(vox_semaphore_t* sem, int32_t timeout_ms);

/**
 * 释放信号量（V操作，加1）
 * @param sem 信号量指针
 * @return 成功返回0，失败返回-1
 */
int vox_semaphore_post(vox_semaphore_t* sem);

/**
 * 获取信号量当前值
 * @param sem 信号量指针
 * @return 返回当前值，失败返回-1
 */
int vox_semaphore_get_value(vox_semaphore_t* sem);

/* ===== 屏障 (Barrier) ===== */

#ifdef VOX_OS_WINDOWS
    /* 检查是否支持 SYNCHRONIZATION_BARRIER */
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0602  /* Windows 8 */
    #endif
    
    #if _WIN32_WINNT >= 0x0602
        /* Windows 8+ 使用原生 SYNCHRONIZATION_BARRIER */
        struct vox_barrier {
            SYNCHRONIZATION_BARRIER barrier;
            uint32_t count;
        };
        #define VOX_BARRIER_USE_NATIVE 1
    #else
        /* Windows 7 及以下使用条件变量和互斥锁实现 */
        struct vox_barrier {
            CRITICAL_SECTION mutex;
            CONDITION_VARIABLE condition;
            uint32_t count;
            uint32_t waiting;
            uint32_t generation;
        };
        #define VOX_BARRIER_USE_NATIVE 0
    #endif
#elif defined(VOX_OS_LINUX)
/* Linux 提供 pthread_barrier_t */
struct vox_barrier {
    pthread_barrier_t barrier;
    uint32_t count;
};
#else
/* macOS/BSD 等无 pthread_barrier_t，用互斥锁+条件变量实现 */
struct vox_barrier {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint32_t count;
    uint32_t waiting;
    uint32_t generation;
};
#endif

typedef struct vox_barrier vox_barrier_t;

/**
 * 创建屏障（初始化结构体）
 * @param barrier 屏障结构体指针，必须非NULL
 * @param count 需要等待的线程数量
 * @return 成功返回0，失败返回-1
 */
int vox_barrier_create(vox_barrier_t* barrier, uint32_t count);

/**
 * 销毁屏障（清理资源，不释放内存）
 * @param barrier 屏障指针
 */
void vox_barrier_destroy(vox_barrier_t* barrier);

/**
 * 等待屏障（阻塞直到所有线程到达）
 * @param barrier 屏障指针
 * @return 成功返回0，失败返回-1。返回0表示所有线程已到达，可以继续执行
 */
int vox_barrier_wait(vox_barrier_t* barrier);

/* ===== 事件 (Event) ===== */

#ifdef VOX_OS_WINDOWS
struct vox_event {
    HANDLE handle;
    bool manual_reset;
};
#else
struct vox_event {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool signaled;
    bool manual_reset;
    uint32_t pulse_count;  /* 脉冲计数器，用于检测脉冲事件 */
};
#endif

typedef struct vox_event vox_event_t;

/**
 * 创建事件（初始化结构体）
 * @param event 事件结构体指针，必须非NULL
 * @param manual_reset 是否手动重置（true=手动重置，false=自动重置）
 * @param initial_state 初始状态（true=已触发，false=未触发）
 * @return 成功返回0，失败返回-1
 */
int vox_event_create(vox_event_t* event, bool manual_reset, bool initial_state);

/**
 * 销毁事件（清理资源，不释放内存）
 * @param event 事件指针
 */
void vox_event_destroy(vox_event_t* event);

/**
 * 等待事件（阻塞直到事件被触发）
 * @param event 事件指针
 * @return 成功返回0，失败返回-1
 */
int vox_event_wait(vox_event_t* event);

/**
 * 尝试等待事件（非阻塞）
 * @param event 事件指针
 * @return 事件已触发返回0，未触发返回-1
 */
int vox_event_trywait(vox_event_t* event);

/**
 * 等待事件（带超时，毫秒）
 * @param event 事件指针
 * @param timeout_ms 超时时间（毫秒），0表示不等待，-1表示无限等待
 * @return 成功返回0，超时返回-1
 */
int vox_event_timedwait(vox_event_t* event, int32_t timeout_ms);

/**
 * 触发事件（设置事件为已触发状态）
 * @param event 事件指针
 * @return 成功返回0，失败返回-1
 */
int vox_event_set(vox_event_t* event);

/**
 * 重置事件（设置事件为未触发状态）
 * @param event 事件指针
 * @return 成功返回0，失败返回-1
 */
int vox_event_reset(vox_event_t* event);

/**
 * 脉冲事件（触发事件并立即重置，仅对手动重置事件有效）
 * @param event 事件指针
 * @return 成功返回0，失败返回-1
 */
int vox_event_pulse(vox_event_t* event);

#ifdef __cplusplus
}
#endif

#endif /* VOX_MUTEX_H */
