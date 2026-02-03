/*
 * vox_coroutine_pool.c - 协程池实现
 */

#ifdef VOX_OS_MACOS
#define _DARWIN_C_SOURCE 1
#endif

#include "vox_coroutine_pool.h"
#include "../vox_loop.h"
#include "../vox_log.h"
#include "../vox_mutex.h"

#ifdef VOX_OS_WINDOWS
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/* 协程池结构 */
struct vox_coroutine_pool {
    vox_loop_t* loop;
    vox_mpool_t* mpool;
    vox_coroutine_pool_config_t config;

    /* 空闲列表 */
    vox_list_t free_list;

    /* 统计 */
    size_t total_created;
    size_t total_acquired;
    size_t total_released;
    size_t current_in_use;
    size_t peak_in_use;

    /* 线程安全 */
    vox_mutex_t mutex;
};

/* 默认配置 */
#define DEFAULT_INITIAL_COUNT   64
#define DEFAULT_MAX_COUNT       0       /* 无限制 */
#define DEFAULT_STACK_SIZE      (64 * 1024)
#define DEFAULT_USE_GUARD_PAGES true
#define DEFAULT_THREAD_SAFE     false

/* 获取系统页大小 */
static size_t get_page_size(void) {
#ifdef VOX_OS_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

/* 分配带 guard page 的栈 */
static void* alloc_stack_with_guard(size_t stack_size, void** guard_page_out) {
    size_t page_size = get_page_size();
    size_t total_size = stack_size + page_size;

#ifdef VOX_OS_WINDOWS
    /* Windows: VirtualAlloc */
    void* mem = VirtualAlloc(NULL, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        return NULL;
    }

    /* 设置 guard page (栈底) */
    DWORD old_protect;
    if (!VirtualProtect(mem, page_size, PAGE_NOACCESS, &old_protect)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return NULL;
    }

    *guard_page_out = mem;
    return (char*)mem + page_size;
#else
    /* Unix: mmap */
    void* mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        return NULL;
    }

    /* 设置 guard page (栈底) */
    if (mprotect(mem, page_size, PROT_NONE) != 0) {
        munmap(mem, total_size);
        return NULL;
    }

    *guard_page_out = mem;
    return (char*)mem + page_size;
#endif
}

/* 释放带 guard page 的栈 */
static void free_stack_with_guard(void* guard_page, size_t stack_size) {
#ifdef VOX_OS_WINDOWS
    VOX_UNUSED(stack_size);
    VirtualFree(guard_page, 0, MEM_RELEASE);
#else
    size_t page_size = get_page_size();
    size_t total_size = stack_size + page_size;
    munmap(guard_page, total_size);
#endif
}

/* 创建槽 */
static vox_coroutine_slot_t* create_slot(vox_coroutine_pool_t* pool) {
    vox_coroutine_slot_t* slot = (vox_coroutine_slot_t*)vox_mpool_alloc(
        pool->mpool, sizeof(vox_coroutine_slot_t));
    if (!slot) {
        return NULL;
    }

    memset(slot, 0, sizeof(vox_coroutine_slot_t));
    vox_list_node_init(&slot->node);
    slot->stack_size = pool->config.stack_size;

    /* 分配栈 */
    if (pool->config.use_guard_pages) {
        slot->stack = alloc_stack_with_guard(slot->stack_size, &slot->guard_page);
    } else {
        slot->stack = vox_mpool_alloc(pool->mpool, slot->stack_size);
        slot->guard_page = NULL;
    }

    if (!slot->stack) {
        vox_mpool_free(pool->mpool, slot);
        return NULL;
    }

    pool->total_created++;
    return slot;
}

/* 销毁槽 */
static void destroy_slot(vox_coroutine_pool_t* pool, vox_coroutine_slot_t* slot) {
    if (!slot) return;

    if (slot->guard_page) {
        free_stack_with_guard(slot->guard_page, slot->stack_size);
    } else if (slot->stack) {
        vox_mpool_free(pool->mpool, slot->stack);
    }

    vox_mpool_free(pool->mpool, slot);
}

/* 锁定池 */
static inline void pool_lock(vox_coroutine_pool_t* pool) {
    if (pool->config.thread_safe) {
        vox_mutex_lock(&pool->mutex);
    }
}

/* 解锁池 */
static inline void pool_unlock(vox_coroutine_pool_t* pool) {
    if (pool->config.thread_safe) {
        vox_mutex_unlock(&pool->mutex);
    }
}

/* 获取默认配置 */
void vox_coroutine_pool_config_default(vox_coroutine_pool_config_t* config) {
    if (!config) return;
    config->initial_count = DEFAULT_INITIAL_COUNT;
    config->max_count = DEFAULT_MAX_COUNT;
    config->stack_size = DEFAULT_STACK_SIZE;
    config->use_guard_pages = DEFAULT_USE_GUARD_PAGES;
    config->thread_safe = DEFAULT_THREAD_SAFE;
}

/* 创建协程池 */
vox_coroutine_pool_t* vox_coroutine_pool_create(
    vox_loop_t* loop,
    const vox_coroutine_pool_config_t* config) {

    if (!loop) {
        return NULL;
    }

    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    if (!mpool) {
        return NULL;
    }

    vox_coroutine_pool_t* pool = (vox_coroutine_pool_t*)vox_mpool_alloc(
        mpool, sizeof(vox_coroutine_pool_t));
    if (!pool) {
        return NULL;
    }

    memset(pool, 0, sizeof(vox_coroutine_pool_t));
    pool->loop = loop;
    pool->mpool = mpool;

    /* 应用配置 */
    if (config) {
        pool->config = *config;
    } else {
        vox_coroutine_pool_config_default(&pool->config);
    }

    /* 初始化空闲列表 */
    vox_list_init(&pool->free_list);

    /* 创建互斥锁 (如果需要) */
    if (pool->config.thread_safe) {
        if (vox_mutex_create(&pool->mutex) != 0) {
            vox_mpool_free(mpool, pool);
            return NULL;
        }
    }

    /* 预分配初始槽 */
    if (pool->config.initial_count > 0) {
        vox_coroutine_pool_warmup(pool, pool->config.initial_count);
    }

    return pool;
}

/* 销毁协程池 */
void vox_coroutine_pool_destroy(vox_coroutine_pool_t* pool) {
    if (!pool) return;

    /* 销毁所有空闲槽 */
    vox_list_node_t* node;
    vox_list_node_t* next;
    vox_list_for_each_safe(node, next, &pool->free_list) {
        vox_coroutine_slot_t* slot = vox_container_of(node, vox_coroutine_slot_t, node);
        vox_list_remove(&pool->free_list, node);
        destroy_slot(pool, slot);
    }

    /* 仅当创建过互斥锁时才销毁 */
    if (pool->config.thread_safe) {
        vox_mutex_destroy(&pool->mutex);
    }

    vox_mpool_free(pool->mpool, pool);
}

/* 从池中获取槽 */
vox_coroutine_slot_t* vox_coroutine_pool_acquire(vox_coroutine_pool_t* pool) {
    if (!pool) return NULL;

    pool_lock(pool);

    vox_coroutine_slot_t* slot = NULL;

    /* 尝试从空闲列表获取 */
    if (!vox_list_empty(&pool->free_list)) {
        vox_list_node_t* node = vox_list_pop_front(&pool->free_list);
        slot = vox_container_of(node, vox_coroutine_slot_t, node);
    } else {
        /* 检查是否达到最大数量 */
        if (pool->config.max_count > 0 &&
            pool->total_created >= pool->config.max_count) {
            pool_unlock(pool);
            return NULL;
        }

        /* 创建新槽 */
        slot = create_slot(pool);
    }

    if (slot) {
        slot->in_use = true;
        pool->total_acquired++;
        pool->current_in_use++;
        if (pool->current_in_use > pool->peak_in_use) {
            pool->peak_in_use = pool->current_in_use;
        }
    }

    pool_unlock(pool);
    return slot;
}

/* 归还槽到池 */
void vox_coroutine_pool_release(vox_coroutine_pool_t* pool,
                                 vox_coroutine_slot_t* slot) {
    if (!pool || !slot) return;

    pool_lock(pool);

    slot->in_use = false;
    vox_list_push_back(&pool->free_list, &slot->node);
    pool->total_released++;
    pool->current_in_use--;

    pool_unlock(pool);
}

/* 获取统计信息 */
void vox_coroutine_pool_get_stats(const vox_coroutine_pool_t* pool,
                                   vox_coroutine_pool_stats_t* stats) {
    if (!pool || !stats) return;

    stats->total_created = pool->total_created;
    stats->total_acquired = pool->total_acquired;
    stats->total_released = pool->total_released;
    stats->current_free = vox_list_size(&pool->free_list);
    stats->current_in_use = pool->current_in_use;
    stats->peak_in_use = pool->peak_in_use;
    stats->stack_size = pool->config.stack_size;
}

/* 预热池 */
int vox_coroutine_pool_warmup(vox_coroutine_pool_t* pool, size_t count) {
    if (!pool) return -1;

    pool_lock(pool);

    for (size_t i = 0; i < count; i++) {
        if (pool->config.max_count > 0 &&
            pool->total_created >= pool->config.max_count) {
            break;
        }

        vox_coroutine_slot_t* slot = create_slot(pool);
        if (!slot) {
            pool_unlock(pool);
            return -1;
        }

        vox_list_push_back(&pool->free_list, &slot->node);
    }

    pool_unlock(pool);
    return 0;
}

/* 收缩池 */
size_t vox_coroutine_pool_shrink(vox_coroutine_pool_t* pool, size_t keep_count) {
    if (!pool) return 0;

    pool_lock(pool);

    size_t freed = 0;
    while (vox_list_size(&pool->free_list) > keep_count) {
        vox_list_node_t* node = vox_list_pop_back(&pool->free_list);
        if (!node) break;

        vox_coroutine_slot_t* slot = vox_container_of(node, vox_coroutine_slot_t, node);
        destroy_slot(pool, slot);
        freed++;
    }

    pool_unlock(pool);
    return freed;
}
