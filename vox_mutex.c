/*
 * vox_mutex.c - 跨平台线程同步原语实现
 */

#include "vox_mutex.h"
#include "vox_os.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef VOX_OS_WINDOWS
    #include <synchapi.h>
#else
    #include <pthread.h>
    #include <semaphore.h>
    #include <errno.h>
    #include <time.h>
#endif

/* ===== 互斥锁 (Mutex) ===== */

int vox_mutex_create(vox_mutex_t* mutex) {
    if (!mutex) return -1;
    
#ifdef VOX_OS_WINDOWS
    InitializeCriticalSection(&mutex->cs);
    return 0;
#else
    if (pthread_mutex_init(&mutex->mutex, NULL) != 0) {
        return -1;
    }
    return 0;
#endif
}

void vox_mutex_destroy(vox_mutex_t* mutex) {
    if (!mutex) return;
    
#ifdef VOX_OS_WINDOWS
    DeleteCriticalSection(&mutex->cs);
#else
    pthread_mutex_destroy(&mutex->mutex);
#endif
}

int vox_mutex_lock(vox_mutex_t* mutex) {
    if (!mutex) return -1;
    
#ifdef VOX_OS_WINDOWS
    EnterCriticalSection(&mutex->cs);
    return 0;
#else
    return pthread_mutex_lock(&mutex->mutex) == 0 ? 0 : -1;
#endif
}

int vox_mutex_trylock(vox_mutex_t* mutex) {
    if (!mutex) return -1;
    
#ifdef VOX_OS_WINDOWS
    return TryEnterCriticalSection(&mutex->cs) ? 0 : -1;
#else
    return pthread_mutex_trylock(&mutex->mutex) == 0 ? 0 : -1;
#endif
}

int vox_mutex_unlock(vox_mutex_t* mutex) {
    if (!mutex) return -1;
    
#ifdef VOX_OS_WINDOWS
    LeaveCriticalSection(&mutex->cs);
    return 0;
#else
    return pthread_mutex_unlock(&mutex->mutex) == 0 ? 0 : -1;
#endif
}

/* ===== 读写锁 (Read-Write Lock) ===== */

#ifdef VOX_OS_WINDOWS
/* Windows 使用 TLS 来跟踪每个线程持有的锁类型 */
static DWORD g_rwlock_tls_key = TLS_OUT_OF_INDEXES;
static volatile LONG g_rwlock_tls_init = 0;  /* 0=未初始化, 1=初始化中, 2=已成功, -1=失败 */

/* 初始化 TLS 键（线程安全） */
static void init_rwlock_tls(void) {
    if (InterlockedCompareExchange(&g_rwlock_tls_init, 1, 0) == 0) {
        /* 第一个线程初始化 TLS 键 */
        DWORD key = TlsAlloc();
        if (key != TLS_OUT_OF_INDEXES) {
            g_rwlock_tls_key = key;
            InterlockedExchange(&g_rwlock_tls_init, 2);  /* 标记为已初始化 */
        } else {
            InterlockedExchange(&g_rwlock_tls_init, -1);  /* 初始化失败 */
        }
    } else {
        /* 其他线程等待初始化完成 */
        while (g_rwlock_tls_init == 1) {
            Sleep(0);  /* 让出 CPU，等待初始化完成 */
        }
    }
}

/* 获取当前线程的锁计数
 * 返回值：0=无锁，正数=读锁计数（1,2,3...），-1=写锁
 */
static LONG get_thread_lock_count(void) {
    if (g_rwlock_tls_key == TLS_OUT_OF_INDEXES) {
        init_rwlock_tls();
    }
    if (g_rwlock_tls_key == TLS_OUT_OF_INDEXES) {
        return 0;  /* TLS 初始化失败 */
    }
    void* value = TlsGetValue(g_rwlock_tls_key);
    return (LONG)(intptr_t)value;
}

/* 设置当前线程的锁计数 */
static void set_thread_lock_count(LONG count) {
    if (g_rwlock_tls_key == TLS_OUT_OF_INDEXES) {
        init_rwlock_tls();
    }
    if (g_rwlock_tls_key != TLS_OUT_OF_INDEXES) {
        TlsSetValue(g_rwlock_tls_key, (void*)(intptr_t)count);
    }
}
#endif

int vox_rwlock_create(vox_rwlock_t* rwlock) {
    if (!rwlock) return -1;
    
#ifdef VOX_OS_WINDOWS
    InitializeSRWLock(&rwlock->srwlock);
    /* 确保 TLS 已初始化 */
    init_rwlock_tls();
    return 0;
#else
    if (pthread_rwlock_init(&rwlock->rwlock, NULL) != 0) {
        return -1;
    }
    return 0;
#endif
}

void vox_rwlock_destroy(vox_rwlock_t* rwlock) {
    if (!rwlock) return;
    
#ifdef VOX_OS_WINDOWS
    /* SRWLock不需要显式销毁 */
#else
    pthread_rwlock_destroy(&rwlock->rwlock);
#endif
}

int vox_rwlock_rdlock(vox_rwlock_t* rwlock) {
    if (!rwlock) return -1;
    
#ifdef VOX_OS_WINDOWS
    LONG count = get_thread_lock_count();
    if (count < 0) {
        /* 当前持有写锁，不应该获取读锁（会导致死锁） */
        return -1;
    }
    AcquireSRWLockShared(&rwlock->srwlock);
    set_thread_lock_count(count + 1);  /* 增加读锁计数 */
    return 0;
#else
    return pthread_rwlock_rdlock(&rwlock->rwlock) == 0 ? 0 : -1;
#endif
}

int vox_rwlock_tryrdlock(vox_rwlock_t* rwlock) {
    if (!rwlock) return -1;
    
#ifdef VOX_OS_WINDOWS
    LONG count = get_thread_lock_count();
    if (count < 0) {
        /* 当前持有写锁，不应该获取读锁 */
        return -1;
    }
    if (TryAcquireSRWLockShared(&rwlock->srwlock)) {
        set_thread_lock_count(count + 1);  /* 增加读锁计数 */
        return 0;
    }
    return -1;
#else
    return pthread_rwlock_tryrdlock(&rwlock->rwlock) == 0 ? 0 : -1;
#endif
}

int vox_rwlock_wrlock(vox_rwlock_t* rwlock) {
    if (!rwlock) return -1;
    
#ifdef VOX_OS_WINDOWS
    LONG count = get_thread_lock_count();
    if (count != 0) {
        /* 当前持有读锁或写锁，不应该获取写锁（会导致死锁） */
        return -1;
    }
    AcquireSRWLockExclusive(&rwlock->srwlock);
    set_thread_lock_count(-1);  /* 标记为写锁 */
    return 0;
#else
    return pthread_rwlock_wrlock(&rwlock->rwlock) == 0 ? 0 : -1;
#endif
}

int vox_rwlock_trywrlock(vox_rwlock_t* rwlock) {
    if (!rwlock) return -1;
    
#ifdef VOX_OS_WINDOWS
    LONG count = get_thread_lock_count();
    if (count != 0) {
        /* 当前持有读锁或写锁，不应该获取写锁 */
        return -1;
    }
    if (TryAcquireSRWLockExclusive(&rwlock->srwlock)) {
        set_thread_lock_count(-1);  /* 标记为写锁 */
        return 0;
    }
    return -1;
#else
    return pthread_rwlock_trywrlock(&rwlock->rwlock) == 0 ? 0 : -1;
#endif
}

int vox_rwlock_unlock(vox_rwlock_t* rwlock) {
    if (!rwlock) return -1;
    
#ifdef VOX_OS_WINDOWS
    LONG count = get_thread_lock_count();
    if (count > 0) {
        /* 释放读锁，减少计数 */
        count--;
        set_thread_lock_count(count);
        ReleaseSRWLockShared(&rwlock->srwlock);
        return 0;
    } else if (count == -1) {
        /* 释放写锁 */
        set_thread_lock_count(0);
        ReleaseSRWLockExclusive(&rwlock->srwlock);
        return 0;
    } else {
        /* 错误：当前线程没有持有锁（count == 0） */
        return -1;
    }
#else
    return pthread_rwlock_unlock(&rwlock->rwlock) == 0 ? 0 : -1;
#endif
}

/* ===== 递归锁 (Recursive Mutex) ===== */

int vox_rmutex_create(vox_rmutex_t* rmutex) {
    if (!rmutex) return -1;
    
#ifdef VOX_OS_WINDOWS
    InitializeCriticalSection(&rmutex->cs);
    return 0;
#else
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    
    int result = pthread_mutex_init(&rmutex->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    
    return (result == 0) ? 0 : -1;
#endif
}

void vox_rmutex_destroy(vox_rmutex_t* rmutex) {
    if (!rmutex) return;
    
#ifdef VOX_OS_WINDOWS
    DeleteCriticalSection(&rmutex->cs);
#else
    pthread_mutex_destroy(&rmutex->mutex);
#endif
}

int vox_rmutex_lock(vox_rmutex_t* rmutex) {
    if (!rmutex) return -1;
    
#ifdef VOX_OS_WINDOWS
    EnterCriticalSection(&rmutex->cs);
    return 0;
#else
    return pthread_mutex_lock(&rmutex->mutex) == 0 ? 0 : -1;
#endif
}

int vox_rmutex_trylock(vox_rmutex_t* rmutex) {
    if (!rmutex) return -1;
    
#ifdef VOX_OS_WINDOWS
    return TryEnterCriticalSection(&rmutex->cs) ? 0 : -1;
#else
    return pthread_mutex_trylock(&rmutex->mutex) == 0 ? 0 : -1;
#endif
}

int vox_rmutex_unlock(vox_rmutex_t* rmutex) {
    if (!rmutex) return -1;
    
#ifdef VOX_OS_WINDOWS
    LeaveCriticalSection(&rmutex->cs);
    return 0;
#else
    return pthread_mutex_unlock(&rmutex->mutex) == 0 ? 0 : -1;
#endif
}

/* ===== 自旋锁 (Spinlock) ===== */

int vox_spinlock_create(vox_spinlock_t* spinlock) {
    if (!spinlock) return -1;
    
#ifdef VOX_OS_WINDOWS
    /* Windows使用CRITICAL_SECTION，可以配置自旋次数 */
    InitializeCriticalSectionAndSpinCount(&spinlock->cs, 4000);
    return 0;
#elif defined(VOX_OS_LINUX)
    if (pthread_spin_init(&spinlock->spinlock, PTHREAD_PROCESS_PRIVATE) != 0) {
        return -1;
    }
    return 0;
#else
    /* macOS/BSD 使用互斥锁替代 */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
    int ret = pthread_mutex_init(&spinlock->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    return ret == 0 ? 0 : -1;
#endif
}

void vox_spinlock_destroy(vox_spinlock_t* spinlock) {
    if (!spinlock) return;
    
#ifdef VOX_OS_WINDOWS
    DeleteCriticalSection(&spinlock->cs);
#elif defined(VOX_OS_LINUX)
    pthread_spin_destroy(&spinlock->spinlock);
#else
    pthread_mutex_destroy(&spinlock->mutex);
#endif
}

void vox_spinlock_lock(vox_spinlock_t* spinlock) {
    if (!spinlock) return;
    
#ifdef VOX_OS_WINDOWS
    EnterCriticalSection(&spinlock->cs);
#elif defined(VOX_OS_LINUX)
    pthread_spin_lock(&spinlock->spinlock);
#else
    pthread_mutex_lock(&spinlock->mutex);
#endif
}

bool vox_spinlock_trylock(vox_spinlock_t* spinlock) {
    if (!spinlock) return false;
    
#ifdef VOX_OS_WINDOWS
    return TryEnterCriticalSection(&spinlock->cs) ? true : false;
#elif defined(VOX_OS_LINUX)
    return pthread_spin_trylock(&spinlock->spinlock) == 0;
#else
    return pthread_mutex_trylock(&spinlock->mutex) == 0;
#endif
}

void vox_spinlock_unlock(vox_spinlock_t* spinlock) {
    if (!spinlock) return;
    
#ifdef VOX_OS_WINDOWS
    LeaveCriticalSection(&spinlock->cs);
#elif defined(VOX_OS_LINUX)
    pthread_spin_unlock(&spinlock->spinlock);
#else
    pthread_mutex_unlock(&spinlock->mutex);
#endif
}

/* ===== 信号量 (Semaphore) ===== */

int vox_semaphore_create(vox_semaphore_t* sem, uint32_t initial_value) {
    if (!sem) return -1;
    
#ifdef VOX_OS_WINDOWS
    sem->sem = CreateSemaphore(NULL, (LONG)initial_value, LONG_MAX, NULL);
    if (!sem->sem) {
        return -1;
    }
    sem->count = (LONG)initial_value;
    return 0;
#elif defined(__APPLE__)
    if (pthread_mutex_init(&sem->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&sem->cond, NULL) != 0) {
        pthread_mutex_destroy(&sem->mutex);
        return -1;
    }
    sem->count = initial_value;
    return 0;
#else
    return (sem_init(&sem->sem, 0, initial_value) != 0) ? -1 : 0;
#endif
}

void vox_semaphore_destroy(vox_semaphore_t* sem) {
    if (!sem) return;
    
#ifdef VOX_OS_WINDOWS
    CloseHandle(sem->sem);
#elif defined(__APPLE__)
    pthread_cond_destroy(&sem->cond);
    pthread_mutex_destroy(&sem->mutex);
#else
    sem_destroy(&sem->sem);
#endif
}

int vox_semaphore_wait(vox_semaphore_t* sem) {
    if (!sem) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (WaitForSingleObject(sem->sem, INFINITE) == WAIT_OBJECT_0) {
        InterlockedDecrement(&sem->count);
        return 0;
    }
    return -1;
#elif defined(__APPLE__)
    if (pthread_mutex_lock(&sem->mutex) != 0) return -1;
    while (sem->count == 0) {
        if (pthread_cond_wait(&sem->cond, &sem->mutex) != 0) {
            pthread_mutex_unlock(&sem->mutex);
            return -1;
        }
    }
    sem->count--;
    pthread_mutex_unlock(&sem->mutex);
    return 0;
#else
    return sem_wait(&sem->sem) == 0 ? 0 : -1;
#endif
}

int vox_semaphore_trywait(vox_semaphore_t* sem) {
    if (!sem) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (WaitForSingleObject(sem->sem, 0) == WAIT_OBJECT_0) {
        InterlockedDecrement(&sem->count);
        return 0;
    }
    return -1;
#elif defined(__APPLE__)
    if (pthread_mutex_lock(&sem->mutex) != 0) return -1;
    if (sem->count == 0) {
        pthread_mutex_unlock(&sem->mutex);
        return -1;
    }
    sem->count--;
    pthread_mutex_unlock(&sem->mutex);
    return 0;
#else
    return sem_trywait(&sem->sem) == 0 ? 0 : -1;
#endif
}

int vox_semaphore_timedwait(vox_semaphore_t* sem, int32_t timeout_ms) {
    if (!sem) return -1;
    
    if (timeout_ms == 0) {
        return vox_semaphore_trywait(sem);
    }
    
#ifdef VOX_OS_WINDOWS
    DWORD timeout = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    if (WaitForSingleObject(sem->sem, timeout) == WAIT_OBJECT_0) {
        InterlockedDecrement(&sem->count);
        return 0;
    }
    return -1;
#elif defined(__APPLE__)
    if (timeout_ms < 0) {
        return vox_semaphore_wait(sem);
    }
    if (pthread_mutex_lock(&sem->mutex) != 0) return -1;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    int ret = -1;
    while (sem->count == 0) {
        int r = pthread_cond_timedwait(&sem->cond, &sem->mutex, &ts);
        if (r == ETIMEDOUT) {
            pthread_mutex_unlock(&sem->mutex);
            return -1;
        }
        if (r != 0) {
            pthread_mutex_unlock(&sem->mutex);
            return -1;
        }
    }
    sem->count--;
    ret = 0;
    pthread_mutex_unlock(&sem->mutex);
    return ret;
#elif defined(VOX_OS_LINUX)
    if (timeout_ms < 0) {
        return sem_wait(&sem->sem) == 0 ? 0 : -1;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    return sem_timedwait(&sem->sem, &ts) == 0 ? 0 : -1;
#else
    if (timeout_ms < 0) {
        return sem_wait(&sem->sem) == 0 ? 0 : -1;
    }
    uint64_t deadline_us = (uint64_t)timeout_ms * 1000;
    struct timespec start;
    clock_gettime(CLOCK_REALTIME, &start);
    uint64_t start_us = (uint64_t)start.tv_sec * 1000000 + (uint64_t)start.tv_nsec / 1000;
    while (1) {
        if (sem_trywait(&sem->sem) == 0) return 0;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        uint64_t now_us = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
        if (now_us - start_us >= deadline_us) return -1;
        usleep(1000);
    }
#endif
}

int vox_semaphore_post(vox_semaphore_t* sem) {
    if (!sem) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (ReleaseSemaphore(sem->sem, 1, NULL)) {
        InterlockedIncrement(&sem->count);
        return 0;
    }
    return -1;
#elif defined(__APPLE__)
    if (pthread_mutex_lock(&sem->mutex) != 0) return -1;
    sem->count++;
    pthread_cond_signal(&sem->cond);
    pthread_mutex_unlock(&sem->mutex);
    return 0;
#else
    return sem_post(&sem->sem) == 0 ? 0 : -1;
#endif
}

int vox_semaphore_get_value(vox_semaphore_t* sem) {
    if (!sem) return -1;
    
#ifdef VOX_OS_WINDOWS
    return (int)sem->count;
#elif defined(__APPLE__)
    if (pthread_mutex_lock(&sem->mutex) != 0) return -1;
    int value = (int)sem->count;
    pthread_mutex_unlock(&sem->mutex);
    return value;
#else
    int value;
    return (sem_getvalue(&sem->sem, &value) == 0) ? value : -1;
#endif
}

/* ===== 屏障 (Barrier) ===== */

int vox_barrier_create(vox_barrier_t* barrier, uint32_t count) {
    if (!barrier || count == 0) return -1;
    
#ifdef VOX_OS_WINDOWS
    #if VOX_BARRIER_USE_NATIVE
        if (InitializeSynchronizationBarrier(&barrier->barrier, (LONG)count, -1) == 0) {
            return -1;
        }
        barrier->count = count;
        return 0;
    #else
        /* 使用条件变量和互斥锁实现 */
        InitializeCriticalSection(&barrier->mutex);
        InitializeConditionVariable(&barrier->condition);
        barrier->count = count;
        barrier->waiting = 0;
        barrier->generation = 0;
        return 0;
    #endif
#elif defined(VOX_OS_LINUX)
    if (pthread_barrier_init(&barrier->barrier, NULL, count) != 0) {
        return -1;
    }
    barrier->count = count;
    return 0;
#else
    /* macOS/BSD 使用互斥锁+条件变量实现 */
    if (pthread_mutex_init(&barrier->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&barrier->condition, NULL) != 0) {
        pthread_mutex_destroy(&barrier->mutex);
        return -1;
    }
    barrier->count = count;
    barrier->waiting = 0;
    barrier->generation = 0;
    return 0;
#endif
}

void vox_barrier_destroy(vox_barrier_t* barrier) {
    if (!barrier) return;
    
#ifdef VOX_OS_WINDOWS
    #if VOX_BARRIER_USE_NATIVE
        DeleteSynchronizationBarrier(&barrier->barrier);
    #else
        DeleteCriticalSection(&barrier->mutex);
        /* CONDITION_VARIABLE 不需要显式销毁 */
    #endif
#elif defined(VOX_OS_LINUX)
    pthread_barrier_destroy(&barrier->barrier);
#else
    pthread_cond_destroy(&barrier->condition);
    pthread_mutex_destroy(&barrier->mutex);
#endif
}

int vox_barrier_wait(vox_barrier_t* barrier) {
    if (!barrier) return -1;
    
#ifdef VOX_OS_WINDOWS
    #if VOX_BARRIER_USE_NATIVE
        /* Windows 8+ 使用原生 SYNCHRONIZATION_BARRIER */
        /* EnterSynchronizationBarrier 总是成功，返回 TRUE 表示是最后一个到达的线程，
         * 返回 FALSE 表示不是最后一个，但等待成功 */
        EnterSynchronizationBarrier(&barrier->barrier, SYNCHRONIZATION_BARRIER_FLAGS_BLOCK_ONLY);
        return 0;
    #else
        /* Windows 7 及以下使用条件变量和互斥锁实现 */
        EnterCriticalSection(&barrier->mutex);
        
        uint32_t gen = barrier->generation;
        barrier->waiting++;
        
        if (barrier->waiting == barrier->count) {
            /* 最后一个到达的线程：重置计数器并唤醒所有等待的线程 */
            barrier->waiting = 0;
            barrier->generation++;
            /* 先唤醒所有等待的线程，再释放互斥锁 */
            WakeAllConditionVariable(&barrier->condition);
            LeaveCriticalSection(&barrier->mutex);
            return 0;
        } else {
            /* 等待其他线程：等待 generation 改变 */
            while (gen == barrier->generation) {
                /* SleepConditionVariableCS 会自动释放互斥锁并等待，唤醒后重新获取 */
                if (SleepConditionVariableCS(&barrier->condition, &barrier->mutex, INFINITE) == 0) {
                    /* 等待失败（不应该发生，因为超时是INFINITE） */
                    LeaveCriticalSection(&barrier->mutex);
                    return -1;
                }
            }
            /* generation 已改变，说明所有线程都已到达 */
            LeaveCriticalSection(&barrier->mutex);
            return 0;
        }
    #endif
#elif defined(VOX_OS_LINUX)
    int result = pthread_barrier_wait(&barrier->barrier);
    if (result == PTHREAD_BARRIER_SERIAL_THREAD) {
        /* 最后一个到达的线程 */
        return 0;
    } else if (result == 0) {
        /* 其他线程 */
        return 0;
    } else {
        /* 错误 */
        return -1;
    }
#else
    /* macOS/BSD 使用互斥锁+条件变量实现 */
    pthread_mutex_lock(&barrier->mutex);
    uint32_t gen = barrier->generation;
    barrier->waiting++;
    if (barrier->waiting == barrier->count) {
        barrier->waiting = 0;
        barrier->generation++;
        pthread_cond_broadcast(&barrier->condition);
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
    while (gen == barrier->generation) {
        if (pthread_cond_wait(&barrier->condition, &barrier->mutex) != 0) {
            pthread_mutex_unlock(&barrier->mutex);
            return -1;
        }
    }
    pthread_mutex_unlock(&barrier->mutex);
    return 0;
#endif
}

/* ===== 事件 (Event) ===== */

int vox_event_create(vox_event_t* event, bool manual_reset, bool initial_state) {
    if (!event) return -1;
    
#ifdef VOX_OS_WINDOWS
    event->handle = CreateEvent(NULL, manual_reset ? TRUE : FALSE, 
                                 initial_state ? TRUE : FALSE, NULL);
    if (!event->handle) {
        return -1;
    }
    event->manual_reset = manual_reset;
    return 0;
#else
    if (pthread_mutex_init(&event->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&event->condition, NULL) != 0) {
        pthread_mutex_destroy(&event->mutex);
        return -1;
    }
    event->signaled = initial_state;
    event->manual_reset = manual_reset;
    event->pulse_count = 0;
    return 0;
#endif
}

void vox_event_destroy(vox_event_t* event) {
    if (!event) return;
    
#ifdef VOX_OS_WINDOWS
    if (event->handle) {
        CloseHandle(event->handle);
    }
#else
    pthread_cond_destroy(&event->condition);
    pthread_mutex_destroy(&event->mutex);
#endif
}

int vox_event_wait(vox_event_t* event) {
    if (!event) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (WaitForSingleObject(event->handle, INFINITE) == WAIT_OBJECT_0) {
        return 0;
    }
    return -1;
#else
    pthread_mutex_lock(&event->mutex);
    uint32_t saved_pulse_count = event->pulse_count;
    while (!event->signaled && event->pulse_count == saved_pulse_count) {
        pthread_cond_wait(&event->condition, &event->mutex);
    }
    /* 检查是否是因为脉冲而被唤醒 */
    if (event->pulse_count != saved_pulse_count) {
        /* 脉冲事件：即使 signaled 可能是 false，也认为事件已触发 */
        pthread_mutex_unlock(&event->mutex);
        return 0;
    }
    /* 如果是自动重置事件，等待后自动重置 */
    if (!event->manual_reset) {
        event->signaled = false;
    }
    pthread_mutex_unlock(&event->mutex);
    return 0;
#endif
}

int vox_event_trywait(vox_event_t* event) {
    if (!event) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (WaitForSingleObject(event->handle, 0) == WAIT_OBJECT_0) {
        return 0;
    }
    return -1;
#else
    pthread_mutex_lock(&event->mutex);
    if (event->signaled) {
        /* 如果是自动重置事件，等待后自动重置 */
        if (!event->manual_reset) {
            event->signaled = false;
        }
        pthread_mutex_unlock(&event->mutex);
        return 0;
    }
    pthread_mutex_unlock(&event->mutex);
    return -1;
#endif
}

int vox_event_timedwait(vox_event_t* event, int32_t timeout_ms) {
    if (!event) return -1;
    
    if (timeout_ms == 0) {
        return vox_event_trywait(event);
    }
    
#ifdef VOX_OS_WINDOWS
    DWORD timeout = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD result = WaitForSingleObject(event->handle, timeout);
    if (result == WAIT_OBJECT_0) {
        return 0;
    } else if (result == WAIT_TIMEOUT) {
        return -1;  /* 超时 */
    }
    return -1;
#else
    if (timeout_ms < 0) {
        return vox_event_wait(event);
    }
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    
    pthread_mutex_lock(&event->mutex);
    int result = 0;
    while (!event->signaled) {
        int ret = pthread_cond_timedwait(&event->condition, &event->mutex, &ts);
        if (ret == ETIMEDOUT) {
            result = -1;  /* 超时 */
            break;
        } else if (ret != 0) {
            result = -1;  /* 错误 */
            break;
        }
        /* 检查是否在等待过程中被触发 */
        if (event->signaled) {
            break;
        }
    }
    
    if (result == 0 && event->signaled) {
        /* 如果是自动重置事件，等待后自动重置 */
        if (!event->manual_reset) {
            event->signaled = false;
        }
    }
    
    pthread_mutex_unlock(&event->mutex);
    return result;
#endif
}

int vox_event_set(vox_event_t* event) {
    if (!event) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (SetEvent(event->handle)) {
        return 0;
    }
    return -1;
#else
    pthread_mutex_lock(&event->mutex);
    event->signaled = true;
    if (event->manual_reset) {
        /* 手动重置事件：唤醒所有等待的线程 */
        pthread_cond_broadcast(&event->condition);
    } else {
        /* 自动重置事件：只唤醒一个线程 */
        pthread_cond_signal(&event->condition);
    }
    pthread_mutex_unlock(&event->mutex);
    return 0;
#endif
}

int vox_event_reset(vox_event_t* event) {
    if (!event) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (ResetEvent(event->handle)) {
        return 0;
    }
    return -1;
#else
    pthread_mutex_lock(&event->mutex);
    event->signaled = false;
    pthread_mutex_unlock(&event->mutex);
    return 0;
#endif
}

int vox_event_pulse(vox_event_t* event) {
    if (!event) return -1;
    
#ifdef VOX_OS_WINDOWS
    if (PulseEvent(event->handle)) {
        return 0;
    }
    return -1;
#else
    pthread_mutex_lock(&event->mutex);
    /* 增加脉冲计数器，这样等待线程可以检测到脉冲 */
    event->pulse_count++;
    if (event->manual_reset) {
        /* 手动重置事件的脉冲：唤醒所有等待的线程 */
        event->signaled = true;
        pthread_cond_broadcast(&event->condition);
        /* 立即重置 signaled，但等待线程可以通过 pulse_count 检测到脉冲 */
        event->signaled = false;
    } else {
        /* 自动重置事件的脉冲：只唤醒一个线程 */
        event->signaled = true;
        pthread_cond_signal(&event->condition);
        /* 对于自动重置事件，等待线程被唤醒后会自动重置 signaled */
        /* 但为了确保脉冲语义，我们也在发送信号后立即重置 */
        event->signaled = false;
    }
    pthread_mutex_unlock(&event->mutex);
    return 0;
#endif
}
