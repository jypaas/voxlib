/*
 * vox_htable.c - 高性能哈希表实现
 * 使用 wyhash 哈希函数和开放寻址法（线性探测）
 * 使用 vox_mpool 内存池管理所有内存分配
 */

#include "vox_htable.h"
#include "vox_mpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* 默认初始容量（必须是2的幂） */
#define VOX_HTABLE_DEFAULT_INITIAL_CAPACITY 16
#define VOX_HTABLE_DEFAULT_LOAD_FACTOR 0.75

/* 标记：空槽、已删除槽 */
#define VOX_HTABLE_EMPTY 0
#define VOX_HTABLE_DELETED 1
#define VOX_HTABLE_OCCUPIED 2

/* 哈希表条目 */
typedef struct {
    uint8_t status;      /* 状态：EMPTY/DELETED/OCCUPIED */
    void* key;           /* 键指针 */
    size_t key_len;      /* 键长度 */
    void* value;         /* 值指针 */
} vox_htable_entry_t;

/* 哈希表结构 */
struct vox_htable {
    vox_mpool_t* mpool;           /* 内存池 */
    vox_htable_entry_t* entries;  /* 条目数组 */
    size_t capacity;              /* 容量（必须是2的幂） */
    size_t size;                  /* 当前元素数量 */
    size_t deleted_count;         /* 已删除的条目数量 */
    double load_factor_threshold;  /* 负载因子阈值 */
    vox_hash_func_t hash_func;    /* 哈希函数 */
    vox_key_cmp_func_t key_cmp;   /* 键比较函数 */
    vox_key_free_func_t key_free; /* 键释放函数 */
    vox_value_free_func_t value_free; /* 值释放函数 */
};

/* ===== wyhash 哈希函数实现 ===== */

/* wyhash 常量 */
static const uint64_t _wyp0 = 0xa0761d6478bd642full;
static const uint64_t _wyp1 = 0xe7037ed1a0b428dbull;
static const uint64_t _wyp2 = 0x8ebc6af09c88c6e3ull;
static const uint64_t _wyp3 = 0x589965cc75374cc3ull;
static const uint64_t _wyp4 = 0x1d8e4e27c47d124full;

/* wyhash 辅助函数：读取8字节 */
static inline uint64_t _wyr8(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* wyhash 辅助函数：读取4字节 */
static inline uint32_t _wyr4(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* wyhash 辅助函数：读取1-3字节 */
static inline uint64_t _wyr3(const uint8_t* p, size_t k) {
    return (((uint64_t)p[0]) << 16) | (((uint64_t)p[k >> 1]) << 8) | p[k - 1];
}

/* wyhash 主函数 */
static uint64_t wyhash(const void* key, size_t len, uint64_t seed) {
    const uint8_t* p = (const uint8_t*)key;
    uint64_t seed64 = seed;
    
    uint64_t a, b;
    
    if (len <= 16) {
        if (len >= 4) {
            size_t offset = (len >= 8) ? ((len >> 3) << 2) : 0;
            uint64_t v1 = (uint64_t)_wyr4(p);
            uint64_t v2 = (uint64_t)_wyr4(p + offset);
            a = (v1 << 32) | v2;
            uint64_t v3 = (uint64_t)_wyr4(p + len - 4);
            uint64_t v4 = (uint64_t)_wyr4(p + len - 4 - offset);
            b = (v3 << 32) | v4;
        } else if (len > 0) {
            a = _wyr3(p, len);
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        size_t i = len;
        if (i > 48) {
            uint64_t see1 = seed64, see2 = seed64;
            do {
                seed64 = (seed64 ^ _wyr8(p) * _wyp0) * _wyp1;
                see1 = (see1 ^ _wyr8(p + 8) * _wyp0) * _wyp1;
                see2 = (see2 ^ _wyr8(p + 16) * _wyp0) * _wyp1;
                p += 24;
                i -= 24;
            } while (i > 48);
            seed64 ^= see1 ^ see2;
        }
        while (i > 16) {
            seed64 = (seed64 ^ _wyr8(p) * _wyp0) * _wyp1;
            i -= 8;
            p += 8;
        }
        a = _wyr8(p + i - 16);
        b = _wyr8(p + i - 8);
    }
    
    a ^= _wyp2;
    b ^= _wyp3;
    a *= _wyp0;
    b *= _wyp1;
    a = ((a << 32) | (a >> 32)) ^ b;
    a *= _wyp0;
    seed64 ^= a;
    seed64 *= _wyp0;
    seed64 ^= (seed64 >> 32);
    seed64 *= _wyp1;
    seed64 ^= (seed64 >> 32);
    seed64 *= _wyp4;
    seed64 ^= (seed64 >> 32);
    
    return seed64;
}

/* 默认哈希函数（使用wyhash） */
static uint64_t default_hash_func(const void* key, size_t key_len) {
    return wyhash(key, key_len, 0);
}

/* 默认键比较函数 */
static int default_key_cmp(const void* key1, const void* key2, size_t key_len) {
    return memcmp(key1, key2, key_len);
}

/* 计算下一个2的幂（用于容量） */
static size_t next_power_of_two(size_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

/* 计算哈希索引 */
static inline size_t hash_index(const vox_htable_t* htable, const void* key, size_t key_len) {
    uint64_t hash = htable->hash_func(key, key_len);
    return hash & (htable->capacity - 1);  /* capacity是2的幂，所以可以用位运算 */
}

/* 查找条目（内部函数，前向声明） */
static vox_htable_entry_t* find_entry(vox_htable_t* htable, const void* key, size_t key_len, bool for_insert);

/* 检查是否需要扩容 */
static bool needs_resize(const vox_htable_t* htable) {
    if (htable->capacity == 0) return false;  /* 防止除零 */
    size_t total_used = htable->size + htable->deleted_count;
    double load_factor = (double)total_used / htable->capacity;
    return load_factor >= htable->load_factor_threshold;
}

/* 扩容哈希表 */
static int vox_htable_resize(vox_htable_t* htable, size_t new_capacity) {
    if (new_capacity < htable->size) {
        return -1;  /* 新容量不能小于当前元素数量 */
    }
    
    /* 检查容量溢出 */
    if (new_capacity < htable->capacity) {
        return -1;  /* 溢出 */
    }
    
    /* 保存旧数据 */
    vox_htable_entry_t* old_entries = htable->entries;
    size_t old_capacity = htable->capacity;
    
    /* 分配新数组（使用内存池） */
    htable->entries = (vox_htable_entry_t*)vox_mpool_alloc(htable->mpool, new_capacity * sizeof(vox_htable_entry_t));
    if (!htable->entries) {
        htable->entries = old_entries;  /* 恢复 */
        return -1;
    }
    /* 清零新数组 */
    memset(htable->entries, 0, new_capacity * sizeof(vox_htable_entry_t));
    
    htable->capacity = new_capacity;
    size_t old_size = htable->size;  /* 保存旧的大小，用于验证 */
    htable->size = 0;
    htable->deleted_count = 0;
    
    /* 重新插入所有元素（直接插入，不检查扩容，因为新容量已经足够） */
    for (size_t i = 0; i < old_capacity; i++) {
        if (old_entries[i].status == VOX_HTABLE_OCCUPIED) {
            /* 直接查找并插入，跳过扩容检查 */
            vox_htable_entry_t* entry = find_entry(htable, old_entries[i].key, old_entries[i].key_len, true);
            if (!entry) {
                /* 如果找不到插入位置（理论上不应该发生），恢复并返回错误 */
                vox_mpool_free(htable->mpool, htable->entries);
                htable->entries = old_entries;
                htable->capacity = old_capacity;
                htable->size = old_size;
                return -1;
            }
            entry->status = VOX_HTABLE_OCCUPIED;
            entry->key = old_entries[i].key;  /* 复用旧键，不需要重新分配 */
            entry->key_len = old_entries[i].key_len;
            entry->value = old_entries[i].value;
            htable->size++;
        }
    }
    
    /* 释放旧数组（使用内存池） */
    vox_mpool_free(htable->mpool, old_entries);
    
    return 0;
}

/* 查找条目（内部函数） */
static vox_htable_entry_t* find_entry(vox_htable_t* htable, const void* key, size_t key_len, bool for_insert) {
    size_t index = hash_index(htable, key, key_len);
    size_t start_index = index;
    vox_htable_entry_t* first_deleted = NULL;
    
    /* 线性探测 */
    while (true) {
        vox_htable_entry_t* entry = &htable->entries[index];
        
        if (entry->status == VOX_HTABLE_EMPTY) {
            /* 找到空槽 */
            if (for_insert && first_deleted) {
                /* 如果有已删除的槽，优先使用它 */
                return first_deleted;
            }
            return entry;
        }
        
        if (entry->status == VOX_HTABLE_DELETED) {
            /* 记录第一个已删除的槽 */
            if (!first_deleted) {
                first_deleted = entry;
            }
        } else if (entry->status == VOX_HTABLE_OCCUPIED) {
            /* 检查键是否匹配 */
            if (entry->key_len == key_len && 
                htable->key_cmp(entry->key, key, key_len) == 0) {
                return entry;
            }
        }
        
        /* 继续探测下一个位置 */
        index = (index + 1) & (htable->capacity - 1);
        
        /* 防止无限循环（理论上不应该发生，但为了安全） */
        if (index == start_index) {
            break;
        }
    }
    
    return NULL;
}

/* 创建哈希表 */
vox_htable_t* vox_htable_create(vox_mpool_t* mpool) {
    return vox_htable_create_with_config(mpool, NULL);
}

/* 使用配置创建哈希表 */
vox_htable_t* vox_htable_create_with_config(vox_mpool_t* mpool, const vox_htable_config_t* config) {
    if (!mpool) return NULL;
    
    /* 使用内存池分配哈希表结构 */
    vox_htable_t* htable = (vox_htable_t*)vox_mpool_alloc(mpool, sizeof(vox_htable_t));
    if (!htable) {
        return NULL;
    }
    
    /* 初始化结构 */
    memset(htable, 0, sizeof(vox_htable_t));
    htable->mpool = mpool;
    
    /* 设置默认值 */
    size_t initial_capacity = VOX_HTABLE_DEFAULT_INITIAL_CAPACITY;
    double load_factor = VOX_HTABLE_DEFAULT_LOAD_FACTOR;
    vox_hash_func_t hash_func = default_hash_func;
    vox_key_cmp_func_t key_cmp = default_key_cmp;
    vox_key_free_func_t key_free = NULL;
    vox_value_free_func_t value_free = NULL;
    
    /* 应用配置 */
    if (config) {
        if (config->initial_capacity > 0) {
            initial_capacity = next_power_of_two(config->initial_capacity);
        }
        if (config->load_factor > 0.0 && config->load_factor <= 1.0) {
            load_factor = config->load_factor;
        }
        if (config->hash_func) {
            hash_func = config->hash_func;
        }
        if (config->key_cmp) {
            key_cmp = config->key_cmp;
        }
        if (config->key_free) {
            key_free = config->key_free;
        }
        if (config->value_free) {
            value_free = config->value_free;
        }
    }
    
    htable->capacity = initial_capacity;
    htable->size = 0;
    htable->deleted_count = 0;
    htable->load_factor_threshold = load_factor;
    htable->hash_func = hash_func;
    htable->key_cmp = key_cmp;
    htable->key_free = key_free;
    htable->value_free = value_free;
    
    /* 分配条目数组（使用内存池） */
    htable->entries = (vox_htable_entry_t*)vox_mpool_alloc(htable->mpool, htable->capacity * sizeof(vox_htable_entry_t));
    if (!htable->entries) {
        vox_mpool_free(htable->mpool, htable);
        return NULL;
    }
    /* 清零数组 */
    memset(htable->entries, 0, htable->capacity * sizeof(vox_htable_entry_t));
    
    return htable;
}

/* 插入或更新键值对 */
int vox_htable_set(vox_htable_t* htable, const void* key, size_t key_len, void* value) {
    if (!htable || !key || key_len == 0) return -1;
    
    /* 检查是否需要扩容 */
    if (needs_resize(htable)) {
        size_t new_capacity = htable->capacity * 2;
        if (vox_htable_resize(htable, new_capacity) != 0) {
            return -1;
        }
    }
    
    /* 查找条目 */
    vox_htable_entry_t* entry = find_entry(htable, key, key_len, true);
    if (!entry) {
        return -1;  /* 不应该发生 */
    }
    
    /* 如果是更新现有值 */
    if (entry->status == VOX_HTABLE_OCCUPIED) {
        /* 释放旧值（如果配置了释放函数） */
        if (htable->value_free && entry->value) {
            htable->value_free(entry->value);
        }
        entry->value = value;
        return 0;
    }
    
    /* 如果是新插入 */
    /* 如果使用的是已删除的槽，减少deleted_count */
    if (entry->status == VOX_HTABLE_DELETED) {
        /* 释放旧键（如果配置了释放函数） */
        if (htable->key_free && entry->key) {
            htable->key_free(entry->key);
        }
        if (htable->value_free && entry->value) {
            htable->value_free(entry->value);
        }
        htable->deleted_count--;
    }
    
    /* 分配并复制键（使用内存池） */
    void* key_copy = vox_mpool_alloc(htable->mpool, key_len);
    if (!key_copy) {
        return -1;
    }
    memcpy(key_copy, key, key_len);
    
    entry->status = VOX_HTABLE_OCCUPIED;
    entry->key = key_copy;
    entry->key_len = key_len;
    entry->value = value;
    htable->size++;
    
    return 0;
}

/* 获取值 */
void* vox_htable_get(const vox_htable_t* htable, const void* key, size_t key_len) {
    if (!htable || !key || key_len == 0) return NULL;
    
    vox_htable_entry_t* entry = find_entry((vox_htable_t*)htable, key, key_len, false);
    if (entry && entry->status == VOX_HTABLE_OCCUPIED) {
        return entry->value;
    }
    
    return NULL;
}

/* 删除键值对 */
int vox_htable_delete(vox_htable_t* htable, const void* key, size_t key_len) {
    if (!htable || !key || key_len == 0) return -1;
    
    vox_htable_entry_t* entry = find_entry(htable, key, key_len, false);
    if (!entry || entry->status != VOX_HTABLE_OCCUPIED) {
        return -1;  /* 不存在 */
    }
    
    /* 释放键和值 */
    if (htable->key_free && entry->key) {
        htable->key_free(entry->key);
    }
    if (htable->value_free && entry->value) {
        htable->value_free(entry->value);
    }
    
    entry->status = VOX_HTABLE_DELETED;
    entry->key = NULL;
    entry->key_len = 0;
    entry->value = NULL;
    htable->size--;
    htable->deleted_count++;
    
    return 0;
}

/* 检查键是否存在 */
bool vox_htable_contains(const vox_htable_t* htable, const void* key, size_t key_len) {
    return vox_htable_get(htable, key, key_len) != NULL;
}

/* 获取元素数量 */
size_t vox_htable_size(const vox_htable_t* htable) {
    return htable ? htable->size : 0;
}

/* 检查是否为空 */
bool vox_htable_empty(const vox_htable_t* htable) {
    return htable ? (htable->size == 0) : true;
}

/* 清空哈希表 */
void vox_htable_clear(vox_htable_t* htable) {
    if (!htable) return;
    
    for (size_t i = 0; i < htable->capacity; i++) {
        vox_htable_entry_t* entry = &htable->entries[i];
        if (entry->status == VOX_HTABLE_OCCUPIED) {
            if (htable->key_free && entry->key) {
                htable->key_free(entry->key);
            }
            if (htable->value_free && entry->value) {
                htable->value_free(entry->value);
            }
            entry->status = VOX_HTABLE_EMPTY;
            entry->key = NULL;
            entry->key_len = 0;
            entry->value = NULL;
        } else if (entry->status == VOX_HTABLE_DELETED) {
            entry->status = VOX_HTABLE_EMPTY;
            entry->key = NULL;
            entry->key_len = 0;
            entry->value = NULL;
        }
    }
    
    htable->size = 0;
    htable->deleted_count = 0;
}

/* 销毁哈希表 */
void vox_htable_destroy(vox_htable_t* htable) {
    if (!htable) return;
    
    /* 清空所有条目 */
    for (size_t i = 0; i < htable->capacity; i++) {
        vox_htable_entry_t* entry = &htable->entries[i];
        if (entry->status == VOX_HTABLE_OCCUPIED || entry->status == VOX_HTABLE_DELETED) {
            if (htable->key_free && entry->key) {
                htable->key_free(entry->key);
            }
            if (htable->value_free && entry->value) {
                htable->value_free(entry->value);
            }
        }
    }
    
    /* 保存内存池指针 */
    vox_mpool_t* mpool = htable->mpool;
    
    /* 释放条目数组（使用内存池） */
    vox_mpool_free(mpool, htable->entries);
    
    /* 释放哈希表结构（使用内存池） */
    vox_mpool_free(mpool, htable);
}

/* 遍历哈希表 */
size_t vox_htable_foreach(vox_htable_t* htable, 
                          void (*callback)(const void* key, size_t key_len, void* value, void* user_data),
                          void* user_data) {
    if (!htable || !callback) return 0;
    
    size_t count = 0;
    for (size_t i = 0; i < htable->capacity; i++) {
        vox_htable_entry_t* entry = &htable->entries[i];
        if (entry->status == VOX_HTABLE_OCCUPIED) {
            callback(entry->key, entry->key_len, entry->value, user_data);
            count++;
        }
    }
    
    return count;
}

/* 获取统计信息 */
void vox_htable_stats(const vox_htable_t* htable, size_t* capacity, size_t* size, double* load_factor) {
    if (!htable) {
        if (capacity) *capacity = 0;
        if (size) *size = 0;
        if (load_factor) *load_factor = 0.0;
        return;
    }
    
    if (capacity) *capacity = htable->capacity;
    if (size) *size = htable->size;
    if (load_factor) {
        *load_factor = htable->capacity > 0 ? (double)htable->size / htable->capacity : 0.0;
    }
}
