/*
 * vox_vector.c - 高性能动态数组实现
 * 使用 vox_mpool 内存池管理所有内存分配
 */

#include "vox_vector.h"
#include "vox_mpool.h"
#include <stdlib.h>
#include <string.h>

/* 默认初始容量 */
#define VOX_VECTOR_DEFAULT_INITIAL_CAPACITY 16

/* 动态数组结构 */
struct vox_vector {
    vox_mpool_t* mpool;      /* 内存池 */
    void** elements;         /* 元素数组 */
    size_t size;             /* 当前元素数量 */
    size_t capacity;         /* 数组容量 */
    vox_vector_free_func_t elem_free; /* 元素释放函数 */
};

/* 扩容数组 */
static int expand_vector(vox_vector_t* vec) {
    size_t new_capacity = vec->capacity * 2;
    if (new_capacity < vec->capacity) {
        return -1;  /* 溢出 */
    }
    
    void** new_elements = (void**)vox_mpool_alloc(vec->mpool, new_capacity * sizeof(void*));
    if (!new_elements) {
        return -1;
    }
    
    /* 复制旧元素 */
    memcpy(new_elements, vec->elements, vec->size * sizeof(void*));
    
    /* 释放旧数组 */
    vox_mpool_free(vec->mpool, vec->elements);
    
    vec->elements = new_elements;
    vec->capacity = new_capacity;
    
    return 0;
}

/* 创建动态数组 */
vox_vector_t* vox_vector_create(vox_mpool_t* mpool) {
    return vox_vector_create_with_config(mpool, NULL);
}

/* 使用配置创建动态数组 */
vox_vector_t* vox_vector_create_with_config(vox_mpool_t* mpool, const vox_vector_config_t* config) {
    if (!mpool) return NULL;
    
    /* 使用内存池分配数组结构 */
    vox_vector_t* vec = (vox_vector_t*)vox_mpool_alloc(mpool, sizeof(vox_vector_t));
    if (!vec) {
        return NULL;
    }
    
    /* 初始化结构 */
    memset(vec, 0, sizeof(vox_vector_t));
    vec->mpool = mpool;
    
    /* 设置默认值 */
    size_t initial_capacity = VOX_VECTOR_DEFAULT_INITIAL_CAPACITY;
    
    /* 应用配置 */
    if (config) {
        if (config->initial_capacity > 0) {
            initial_capacity = config->initial_capacity;
        }
        if (config->elem_free) {
            vec->elem_free = config->elem_free;
        }
    }
    
    vec->capacity = initial_capacity;
    vec->size = 0;
    
    /* 分配元素数组 */
    vec->elements = (void**)vox_mpool_alloc(vec->mpool, vec->capacity * sizeof(void*));
    if (!vec->elements) {
        vox_mpool_free(vec->mpool, vec);
        return NULL;
    }
    
    return vec;
}

/* 在数组末尾添加元素 */
int vox_vector_push(vox_vector_t* vec, void* elem) {
    if (!vec || !elem) return -1;
    
    /* 检查是否需要扩容 */
    if (vec->size >= vec->capacity) {
        if (expand_vector(vec) != 0) {
            return -1;
        }
    }
    
    /* 添加元素 */
    vec->elements[vec->size] = elem;
    vec->size++;
    
    return 0;
}

/* 移除并返回数组末尾的元素 */
void* vox_vector_pop(vox_vector_t* vec) {
    if (!vec || vec->size == 0) return NULL;
    
    vec->size--;
    return vec->elements[vec->size];
}

/* 在指定位置插入元素 */
int vox_vector_insert(vox_vector_t* vec, size_t index, void* elem) {
    if (!vec || !elem) return -1;
    
    /* 检查索引范围 */
    if (index > vec->size) return -1;
    
    /* 检查是否需要扩容 */
    if (vec->size >= vec->capacity) {
        if (expand_vector(vec) != 0) {
            return -1;
        }
    }
    
    /* 将插入位置之后的元素向后移动 */
    if (index < vec->size) {
        memmove(&vec->elements[index + 1], 
                &vec->elements[index], 
                (vec->size - index) * sizeof(void*));
    }
    
    /* 插入新元素 */
    vec->elements[index] = elem;
    vec->size++;
    
    return 0;
}

/* 移除指定位置的元素 */
void* vox_vector_remove(vox_vector_t* vec, size_t index) {
    if (!vec || index >= vec->size) return NULL;
    
    /* 保存要移除的元素 */
    void* elem = vec->elements[index];
    
    /* 将后面的元素向前移动 */
    if (index < vec->size - 1) {
        memmove(&vec->elements[index], 
                &vec->elements[index + 1], 
                (vec->size - index - 1) * sizeof(void*));
    }
    
    vec->size--;
    
    return elem;
}

/* 获取指定位置的元素 */
void* vox_vector_get(const vox_vector_t* vec, size_t index) {
    if (!vec || index >= vec->size) return NULL;
    return vec->elements[index];
}

/* 设置指定位置的元素 */
int vox_vector_set(vox_vector_t* vec, size_t index, void* elem) {
    if (!vec || index >= vec->size) return -1;
    
    /* 如果配置了释放函数，释放旧元素 */
    if (vec->elem_free && vec->elements[index]) {
        vec->elem_free(vec->elements[index]);
    }
    
    vec->elements[index] = elem;
    return 0;
}

/* 获取数组中的元素数量 */
size_t vox_vector_size(const vox_vector_t* vec) {
    return vec ? vec->size : 0;
}

/* 获取数组的容量 */
size_t vox_vector_capacity(const vox_vector_t* vec) {
    return vec ? vec->capacity : 0;
}

/* 检查数组是否为空 */
bool vox_vector_empty(const vox_vector_t* vec) {
    return vec ? (vec->size == 0) : true;
}

/* 清空数组 */
void vox_vector_clear(vox_vector_t* vec) {
    if (!vec) return;
    
    /* 释放所有元素（如果配置了释放函数） */
    if (vec->elem_free) {
        for (size_t i = 0; i < vec->size; i++) {
            if (vec->elements[i]) {
                vec->elem_free(vec->elements[i]);
            }
        }
    }
    
    vec->size = 0;
}

/* 调整数组大小 */
int vox_vector_resize(vox_vector_t* vec, size_t new_size) {
    if (!vec) return -1;
    
    /* 如果新大小小于当前大小，释放多余元素 */
    if (new_size < vec->size) {
        if (vec->elem_free) {
            for (size_t i = new_size; i < vec->size; i++) {
                if (vec->elements[i]) {
                    vec->elem_free(vec->elements[i]);
                }
            }
        }
        vec->size = new_size;
        return 0;
    }
    
    /* 如果新大小大于当前大小，需要扩容或初始化新元素 */
    if (new_size > vec->capacity) {
        size_t new_capacity = vec->capacity;
        while (new_capacity < new_size) {
            new_capacity *= 2;
            if (new_capacity < vec->capacity) {
                return -1;  /* 溢出 */
            }
        }
        
        void** new_elements = (void**)vox_mpool_alloc(vec->mpool, new_capacity * sizeof(void*));
        if (!new_elements) {
            return -1;
        }
        
        /* 复制旧元素 */
        memcpy(new_elements, vec->elements, vec->size * sizeof(void*));
        
        /* 初始化新元素为 NULL */
        memset(&new_elements[vec->size], 0, (new_capacity - vec->size) * sizeof(void*));
        
        /* 释放旧数组 */
        vox_mpool_free(vec->mpool, vec->elements);
        
        vec->elements = new_elements;
        vec->capacity = new_capacity;
    } else {
        /* 新大小在容量范围内，只需初始化新位置为 NULL */
        memset(&vec->elements[vec->size], 0, (new_size - vec->size) * sizeof(void*));
    }
    
    vec->size = new_size;
    return 0;
}

/* 预留容量 */
int vox_vector_reserve(vox_vector_t* vec, size_t capacity) {
    if (!vec || capacity <= vec->capacity) return 0;
    
    void** new_elements = (void**)vox_mpool_alloc(vec->mpool, capacity * sizeof(void*));
    if (!new_elements) {
        return -1;
    }
    
    /* 复制旧元素 */
    memcpy(new_elements, vec->elements, vec->size * sizeof(void*));
    
    /* 释放旧数组 */
    vox_mpool_free(vec->mpool, vec->elements);
    
    vec->elements = new_elements;
    vec->capacity = capacity;
    
    return 0;
}

/* 遍历数组 */
size_t vox_vector_foreach(vox_vector_t* vec, vox_vector_visit_func_t visit, void* user_data) {
    if (!vec || !visit) return 0;
    
    size_t count = 0;
    for (size_t i = 0; i < vec->size; i++) {
        visit(vec->elements[i], i, user_data);
        count++;
    }
    
    return count;
}

/* 销毁动态数组 */
void vox_vector_destroy(vox_vector_t* vec) {
    if (!vec) return;
    
    /* 清空所有元素 */
    if (vec->elem_free) {
        for (size_t i = 0; i < vec->size; i++) {
            if (vec->elements[i]) {
                vec->elem_free(vec->elements[i]);
            }
        }
    }
    
    /* 保存内存池指针 */
    vox_mpool_t* mpool = vec->mpool;
    
    /* 释放元素数组 */
    vox_mpool_free(mpool, vec->elements);
    
    /* 释放数组结构 */
    vox_mpool_free(mpool, vec);
}
