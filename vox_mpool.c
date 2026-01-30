/*
 * vox_mpool.c - 高性能内存池实现
 * 每个分配的块前面存储元数据（块大小信息）
 */

 #include "vox_mpool.h"
 #include "vox_mutex.h"
 #include <stdlib.h>
 #include <string.h>
 #include <stdio.h>
 #include <stdint.h>
 #include <limits.h>
 
 /* 支持的块大小配置 */
 #define VOX_MPOOL_MIN_BLOCK_SIZE 16
 #define VOX_MPOOL_BLOCK_SIZES 10
 static const size_t BLOCK_SIZES[VOX_MPOOL_BLOCK_SIZES] = {
     16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
 };
 
 /* 每个块大小对应的初始块数量 */
 #define INITIAL_BLOCK_COUNT 64
 
/* 大块内存节点（用于跟踪malloc的内存，使用双向链表优化删除） */
typedef struct vox_chunk {
    struct vox_chunk* next;
    struct vox_chunk* prev;  /* 添加前向指针，优化删除操作 */
    void* memory;
} vox_chunk_t;

/* 块元数据头部（存储在实际数据之前）
 * 注意：为了保证返回的指针是 8 字节对齐的，meta 结构必须是 8 字节
 */
typedef struct {
    uint8_t slot_idx;  /* 槽索引，255表示大块malloc分配 */
    uint8_t padding[7]; /* 填充到 8 字节，保证对齐 */
} vox_block_meta_t;

/* 大块分配的头部（在meta之前） */
typedef struct {
    size_t size;  /* 用户请求的大小 */
    vox_chunk_t* chunk;  /* 指向chunk节点 */
} vox_large_block_header_t;

/* 内存块头部（用于构建自由链表，仅在空闲时使用） */
typedef struct vox_block_header {
    struct vox_block_header* next;
} vox_block_header_t;
 
 /* 内存池槽（每个槽管理一种大小的块） */
 typedef struct {
     size_t block_size;              /* 块大小（包含元数据） */
     size_t user_size;                /* 用户可用大小 */
     vox_block_header_t* free_list;  /* 自由块链表 */
     void* chunks;                    /* 已分配的大块内存链表 */
     size_t total_blocks;             /* 总块数 */
     size_t free_blocks;              /* 空闲块数 */
 } vox_pool_slot_t;
 
/* 内存池结构 */
struct vox_mpool {
    vox_pool_slot_t slots[VOX_MPOOL_BLOCK_SIZES];
    size_t total_allocated;  /* 总分配字节数 */
    size_t total_used;       /* 实际使用字节数 */
    vox_chunk_t* large_chunks;  /* 大块分配链表（用于跟踪和释放） */
    int thread_safe;         /* 是否线程安全 */
    vox_mutex_t mutex;       /* 互斥锁（仅在thread_safe为真时使用） */
    size_t initial_block_count;  /* 每个块大小对应的初始块数量 */
};
 
 /* 获取块大小对应的槽索引（优化：使用位操作和查找表） */
 static inline int vox_mpool_get_slot_index(size_t size) {
     /* 快速路径：使用位操作和查找表 */
     if (size <= 16) return 0;
     if (size <= 32) return 1;
     if (size <= 64) return 2;
     if (size <= 128) return 3;
     if (size <= 256) return 4;
     if (size <= 512) return 5;
     if (size <= 1024) return 6;
     if (size <= 2048) return 7;
     if (size <= 4096) return 8;
     if (size <= 8192) return 9;
     return -1;  /* 超过最大块大小 */
 }
 
/* 从内存指针获取元数据 */
static inline vox_block_meta_t* vox_get_meta(void* ptr) {
    return (vox_block_meta_t*)((char*)ptr - sizeof(vox_block_meta_t));
}

/* 从元数据获取大块头部（仅用于大块分配） */
static inline vox_large_block_header_t* vox_get_large_header(vox_block_meta_t* meta) {
    return (vox_large_block_header_t*)((char*)meta - sizeof(vox_large_block_header_t));
}
 
 /* 从元数据指针获取用户指针 */
 static inline void* vox_get_user_ptr(vox_block_meta_t* meta) {
     return (void*)((char*)meta + sizeof(vox_block_meta_t));
 }

/* 线程安全辅助宏 */
#define VOX_MPOOL_LOCK(pool) \
    do { \
        if ((pool) && (pool)->thread_safe) { \
            vox_mutex_lock(&(pool)->mutex); \
        } \
    } while (0)

#define VOX_MPOOL_UNLOCK(pool) \
    do { \
        if ((pool) && (pool)->thread_safe) { \
            vox_mutex_unlock(&(pool)->mutex); \
        } \
    } while (0)
 
 /* 为特定槽分配新的内存块（优化：减少内存访问，提高缓存友好性） */
 static int vox_mpool_expand_slot(vox_pool_slot_t* slot, size_t block_count) {
     size_t block_size = slot->block_size;
     size_t chunk_size = block_size * block_count;
     
     /* 分配大块内存（malloc已经保证对齐，不需要额外对齐处理） */
     void* memory = malloc(chunk_size + sizeof(vox_chunk_t));
     if (!memory) return -1;
     
     /* 设置chunk节点 */
     vox_chunk_t* chunk = (vox_chunk_t*)memory;
     chunk->memory = (char*)memory + sizeof(vox_chunk_t);
     chunk->next = (vox_chunk_t*)slot->chunks;
     slot->chunks = chunk;
     
     /* 将新内存块加入自由链表（优化：减少指针解引用，批量构建链表） */
     char* ptr = (char*)chunk->memory;
     vox_block_header_t* first = NULL;
     vox_block_header_t* last = NULL;
     
     for (size_t i = 0; i < block_count; i++) {
         vox_block_header_t* header = (vox_block_header_t*)ptr;
         if (i == 0) {
             first = header;
         } else {
             last->next = header;
         }
         last = header;
         ptr += block_size;
     }
     
     /* 将整个链表连接到现有自由链表 */
     if (last) {
         last->next = slot->free_list;
         slot->free_list = first;
     }
     
     slot->total_blocks += block_count;
     slot->free_blocks += block_count;
     
     return 0;
 }
 
 /* 创建内存池 */
 vox_mpool_t* vox_mpool_create(void) {
     vox_mpool_config_t config = {0};  /* 默认非线程安全 */
     return vox_mpool_create_with_config(&config);
 }

/* 使用配置创建内存池 */
vox_mpool_t* vox_mpool_create_with_config(const vox_mpool_config_t* config) {
    vox_mpool_t* pool = (vox_mpool_t*)malloc(sizeof(vox_mpool_t));
    if (!pool) return NULL;
    
    memset(pool, 0, sizeof(vox_mpool_t));
    
    /* 设置线程安全选项 */
    if (config && config->thread_safe) {
        pool->thread_safe = 1;
        /* 初始化互斥锁 */
        if (vox_mutex_create(&pool->mutex) != 0) {
            free(pool);
            return NULL;
        }
    } else {
        pool->thread_safe = 0;
    }
    
    /* 设置初始块数量 */
    if (config && config->initial_block_count > 0) {
        pool->initial_block_count = config->initial_block_count;
    } else {
        pool->initial_block_count = INITIAL_BLOCK_COUNT;  /* 默认值 */
    }
    
    /* 初始化每个槽，每个块需要额外的元数据空间 */
    for (int i = 0; i < VOX_MPOOL_BLOCK_SIZES; i++) {
        pool->slots[i].user_size = BLOCK_SIZES[i];
        pool->slots[i].block_size = BLOCK_SIZES[i] + sizeof(vox_block_meta_t);
        pool->slots[i].free_list = NULL;
        pool->slots[i].chunks = NULL;
        pool->slots[i].total_blocks = 0;
        pool->slots[i].free_blocks = 0;
    }
    
    pool->large_chunks = NULL;
    
    return pool;
}
 
 /* 从内存池分配内存（内部版本，不加锁） */
 static void* vox_mpool_alloc_internal(vox_mpool_t* pool, size_t size) {
     if (!pool || size == 0) return NULL;
     
     /* 获取对应的槽索引（快速路径） */
     int slot_idx = vox_mpool_get_slot_index(size);
     
    if (slot_idx < 0) {
        /* 超过最大块大小，直接使用malloc */
        /* 分配：chunk节点 + large_header + meta + 用户数据 */
        /* 检查整数溢出 */
        size_t header_size = sizeof(vox_chunk_t) + sizeof(vox_large_block_header_t) + 
                            sizeof(vox_block_meta_t);
        if (size > SIZE_MAX - header_size) {
            return NULL;  /* 整数溢出 */
        }
        size_t total_size = header_size + size;
        void* memory = malloc(total_size);
        if (!memory) {
            return NULL;
        }
        
        /* 设置chunk节点用于跟踪 */
        vox_chunk_t* chunk = (vox_chunk_t*)memory;
        char* ptr = (char*)memory + sizeof(vox_chunk_t);
        
        /* 设置large header */
        vox_large_block_header_t* large_header = (vox_large_block_header_t*)ptr;
        large_header->size = size;
        large_header->chunk = chunk;
        ptr += sizeof(vox_large_block_header_t);
        
        /* 设置meta */
        vox_block_meta_t* meta = (vox_block_meta_t*)ptr;
        meta->slot_idx = 255;  /* 标记为大块分配 */
        
        /* 将chunk加入链表（双向链表） */
        chunk->memory = (void*)large_header;  /* 存储large_header指针 */
        chunk->next = pool->large_chunks;
        chunk->prev = NULL;
        if (pool->large_chunks) {
            pool->large_chunks->prev = chunk;
        }
        pool->large_chunks = chunk;
        
        pool->total_used += size;
        return vox_get_user_ptr(meta);
    }
     
     vox_pool_slot_t* slot = &pool->slots[slot_idx];
     
     /* 如果自由链表为空，扩展槽（优化：减少分支预测失败） */
     vox_block_header_t* block = slot->free_list;
     if (block == NULL) {
         if (vox_mpool_expand_slot(slot, pool->initial_block_count) != 0) {
             return NULL;
         }
         block = slot->free_list;
     }
     
     /* 从自由链表中取出一个块（优化：减少内存访问） */
     slot->free_list = block->next;
     slot->free_blocks--;
     
    /* 设置元数据（在同一内存位置，提高缓存局部性） */
    vox_block_meta_t* meta = (vox_block_meta_t*)block;
    meta->slot_idx = (uint8_t)slot_idx;
    
    /* 使用实际分配的user_size进行统计，而不是请求的size */
    pool->total_used += slot->user_size;
    
    return vox_get_user_ptr(meta);
 }

 /* 从内存池分配内存（优化：减少分支，提高缓存友好性） */
 void* vox_mpool_alloc(vox_mpool_t* pool, size_t size) {
     if (!pool || size == 0) return NULL;
     
     VOX_MPOOL_LOCK(pool);
     void* result = vox_mpool_alloc_internal(pool, size);
     VOX_MPOOL_UNLOCK(pool);
     return result;
 }
 
 /* 释放内存回内存池（内部版本，不加锁） */
 static void vox_mpool_free_internal(vox_mpool_t* pool, void* ptr) {
     if (!pool || !ptr) return;
     
     /* 获取元数据（优化：减少指针解引用） */
     vox_block_meta_t* meta = vox_get_meta(ptr);
     uint8_t slot_idx = meta->slot_idx;
     
    if (slot_idx == 255) {
        /* 大块分配，获取large header */
        vox_large_block_header_t* large_header = vox_get_large_header(meta);
        size_t size = large_header->size;
        vox_chunk_t* chunk = large_header->chunk;
        
        /* 从双向链表中移除chunk（O(1)操作） */
        if (chunk->prev) {
            chunk->prev->next = chunk->next;
        } else {
            pool->large_chunks = chunk->next;
        }
        if (chunk->next) {
            chunk->next->prev = chunk->prev;
        }
        
        /* 释放整个内存块 */
        free((char*)large_header - sizeof(vox_chunk_t));
        
        /* 更新统计 */
        pool->total_used -= size;
        return;
    }
     
     if (slot_idx >= VOX_MPOOL_BLOCK_SIZES) {
         /* 无效的槽索引 */
         return;
     }
     
     vox_pool_slot_t* slot = &pool->slots[slot_idx];
     
     /* 将块加入自由链表（优化：减少内存访问，提高缓存局部性） */
     vox_block_header_t* header = (vox_block_header_t*)meta;
     header->next = slot->free_list;
     slot->free_list = header;
     slot->free_blocks++;
     
     /* 更新统计（优化：减少内存访问） */
     pool->total_used -= slot->user_size;
 }

 /* 释放内存回内存池（优化：减少分支，提高缓存友好性） */
 void vox_mpool_free(vox_mpool_t* pool, void* ptr) {
     if (!pool || !ptr) return;
     
     VOX_MPOOL_LOCK(pool);
     vox_mpool_free_internal(pool, ptr);
     VOX_MPOOL_UNLOCK(pool);
 }
 
 /* 获取已分配内存块的大小 */
 size_t vox_mpool_get_size(vox_mpool_t* pool, void* ptr) {
     if (!pool || !ptr) return 0;
     
     VOX_MPOOL_LOCK(pool);
     
     vox_block_meta_t* meta = vox_get_meta(ptr);
     uint8_t slot_idx = meta->slot_idx;
     
    if (slot_idx == 255) {
        /* 大块分配，从large header获取大小 */
        vox_large_block_header_t* large_header = vox_get_large_header(meta);
        size_t result = large_header->size;
        VOX_MPOOL_UNLOCK(pool);
        return result;
    }
     
     if (slot_idx >= VOX_MPOOL_BLOCK_SIZES) {
         VOX_MPOOL_UNLOCK(pool);
         return 0;
     }
     
     size_t result = pool->slots[slot_idx].user_size;
     VOX_MPOOL_UNLOCK(pool);
     return result;
 }
 
 /* 重新分配内存 */
 void* vox_mpool_realloc(vox_mpool_t* pool, void* ptr, size_t new_size) {
     if (!pool) return NULL;
     
     /* 如果ptr为NULL，相当于alloc */
     if (!ptr) {
         return vox_mpool_alloc(pool, new_size);
     }
     
     /* 如果new_size为0，相当于free */
     if (new_size == 0) {
         vox_mpool_free(pool, ptr);
         return NULL;
     }
     
     VOX_MPOOL_LOCK(pool);
     
     /* 获取原大小 */
     vox_block_meta_t* meta = vox_get_meta(ptr);
     uint8_t old_slot_idx = meta->slot_idx;
     size_t old_size;
     
    if (old_slot_idx == 255) {
        /* 大块分配，从large header获取大小 */
        vox_large_block_header_t* large_header = vox_get_large_header(meta);
        old_size = large_header->size;
     } else if (old_slot_idx >= VOX_MPOOL_BLOCK_SIZES) {
         VOX_MPOOL_UNLOCK(pool);
         return NULL;  /* 无效的槽索引 */
     } else {
         old_size = pool->slots[old_slot_idx].user_size;
     }
     
     /* 获取新大小对应的槽索引 */
     int new_slot_idx = vox_mpool_get_slot_index(new_size);
     
    /* 如果两者在同一个槽中，直接返回原指针（无需重新分配） */
    if (old_slot_idx == new_slot_idx && old_slot_idx < VOX_MPOOL_BLOCK_SIZES) {
        /* 在同一槽内，实际分配大小相同，不需要更新统计 */
        VOX_MPOOL_UNLOCK(pool);
        return ptr;
    }
     
    /* 需要重新分配，使用内部版本（不加锁） */
    void* new_ptr = vox_mpool_alloc_internal(pool, new_size);
    if (!new_ptr) {
        VOX_MPOOL_UNLOCK(pool);
        return NULL;  /* 分配失败，原内存保持不变 */
    }
    
    /* 拷贝数据（拷贝较小的大小） */
    size_t copy_size = old_size;
    if (old_size == 0 || new_size < old_size) {
        copy_size = new_size;
    }
    
    if (copy_size > 0) {
        memcpy(new_ptr, ptr, copy_size);
    }
    
    /* 释放旧内存（使用内部版本，不加锁） */
    vox_mpool_free_internal(pool, ptr);
    
    VOX_MPOOL_UNLOCK(pool);
    return new_ptr;
 }
 
/* 重置内存池 */
void vox_mpool_reset(vox_mpool_t* pool) {
    if (!pool) return;

    VOX_MPOOL_LOCK(pool);

    /* 释放所有大块分配（reset 应该释放所有内存） */
    vox_chunk_t* large_chunk = pool->large_chunks;
    while (large_chunk) {
        vox_chunk_t* next = large_chunk->next;
        free(large_chunk);  /* chunk 节点在分配的开头 */
        large_chunk = next;
    }
    pool->large_chunks = NULL;

    /* 重置每个槽 */
    for (int i = 0; i < VOX_MPOOL_BLOCK_SIZES; i++) {
        vox_pool_slot_t* slot = &pool->slots[i];

        /* 清空当前自由链表 */
        slot->free_list = NULL;
        slot->free_blocks = 0;

        /* 计算每个 chunk 中的块数量 */
        size_t block_size = slot->block_size;
        size_t blocks_per_chunk = pool->initial_block_count;

        /* 将所有chunk中的块重新加入自由链表 */
        vox_chunk_t* chunk = (vox_chunk_t*)slot->chunks;

        while (chunk) {
            char* ptr = (char*)chunk->memory;

            /* 将chunk中的所有块加入自由链表 */
            for (size_t j = 0; j < blocks_per_chunk; j++) {
                vox_block_header_t* header = (vox_block_header_t*)ptr;
                header->next = slot->free_list;
                slot->free_list = header;
                ptr += block_size;
            }

            chunk = chunk->next;
        }

        /* 设置空闲块数等于总块数 */
        slot->free_blocks = slot->total_blocks;
    }

    /* 重置总使用量 */
    pool->total_used = 0;

    VOX_MPOOL_UNLOCK(pool);
}


/* 销毁内存池 */
void vox_mpool_destroy(vox_mpool_t* pool) {
    if (!pool) return;
    
    /* 如果启用了线程安全，先加锁 */
    if (pool->thread_safe) {
        vox_mutex_lock(&pool->mutex);
    }
    
    /* 释放每个槽的所有chunk */
    for (int i = 0; i < VOX_MPOOL_BLOCK_SIZES; i++) {
        vox_chunk_t* chunk = (vox_chunk_t*)pool->slots[i].chunks;
        while (chunk) {
            vox_chunk_t* next = chunk->next;
            free(chunk);
            chunk = next;
        }
    }
    
    /* 释放所有大块分配 */
    vox_chunk_t* large_chunk = pool->large_chunks;
    while (large_chunk) {
        vox_chunk_t* next = large_chunk->next;
        /* large_chunk->memory 指向 large_header，需要释放包含 chunk 节点的原始内存 */
        /* 内存布局：chunk节点 + large_header + meta + 用户数据 */
        /* 所以释放原始分配的内存（chunk节点在开头） */
        free((char*)large_chunk->memory - sizeof(vox_chunk_t));
        large_chunk = next;
    }
    
    /* 清理互斥锁 */
    if (pool->thread_safe) {
        vox_mutex_unlock(&pool->mutex);
        vox_mutex_destroy(&pool->mutex);
    }
    
    free(pool);
}
 
 /* 打印内存池统计信息 */
 void vox_mpool_stats(vox_mpool_t* pool) {
     if (!pool) return;
     
     VOX_MPOOL_LOCK(pool);
     
     printf("=== Memory Pool Statistics ===\n");
     printf("Total used: %zu bytes\n", pool->total_used);
     printf("\nPer-slot statistics:\n");
     
     for (int i = 0; i < VOX_MPOOL_BLOCK_SIZES; i++) {
         vox_pool_slot_t* slot = &pool->slots[i];
         if (slot->total_blocks > 0) {
             printf("Block size %4zu: %zu/%zu blocks free (%.1f%% utilization)\n",
                    slot->user_size,
                    slot->free_blocks,
                    slot->total_blocks,
                    100.0 * (slot->total_blocks - slot->free_blocks) / slot->total_blocks);
         }
     }
     
     VOX_MPOOL_UNLOCK(pool);
 }
