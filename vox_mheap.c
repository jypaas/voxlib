/*
 * vox_mheap.c - 高性能最小堆实现
 * 使用 vox_mpool 内存池管理所有内存分配
 */

#include "vox_mheap.h"
#include "vox_mpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* 默认初始容量 */
#define VOX_MHEAP_DEFAULT_INITIAL_CAPACITY 16

/* 最小堆结构 */
struct vox_mheap {
    vox_mpool_t* mpool;      /* 内存池 */
    void** elements;         /* 元素数组 */
    size_t size;             /* 当前元素数量 */
    size_t capacity;         /* 数组容量 */
    vox_mheap_cmp_func_t cmp_func;  /* 元素比较函数 */
    vox_mheap_free_func_t elem_free; /* 元素释放函数 */
};

/* 默认比较函数（指针比较） */
static int default_cmp(const void* elem1, const void* elem2) {
    if (elem1 < elem2) return -1;
    if (elem1 > elem2) return 1;
    return 0;
}

/* 获取父节点索引 */
static inline size_t parent(size_t i) {
    return (i - 1) / 2;
}

/* 获取左子节点索引 */
static inline size_t left_child(size_t i) {
    return 2 * i + 1;
}

/* 获取右子节点索引 */
static inline size_t right_child(size_t i) {
    return 2 * i + 2;
}

/* 向上调整（上浮） */
static void heapify_up(vox_mheap_t* heap, size_t index) {
    while (index > 0) {
        size_t p = parent(index);
        if (heap->cmp_func(heap->elements[index], heap->elements[p]) >= 0) {
            break;  /* 已经满足堆性质 */
        }
        
        /* 交换 */
        void* temp = heap->elements[index];
        heap->elements[index] = heap->elements[p];
        heap->elements[p] = temp;
        
        index = p;
    }
}

/* 向下调整（下沉） */
static void heapify_down(vox_mheap_t* heap, size_t index) {
    while (true) {
        size_t smallest = index;
        size_t left = left_child(index);
        size_t right = right_child(index);
        
        /* 找到父节点和两个子节点中的最小值 */
        if (left < heap->size && 
            heap->cmp_func(heap->elements[left], heap->elements[smallest]) < 0) {
            smallest = left;
        }
        
        if (right < heap->size && 
            heap->cmp_func(heap->elements[right], heap->elements[smallest]) < 0) {
            smallest = right;
        }
        
        if (smallest == index) {
            break;  /* 已经满足堆性质 */
        }
        
        /* 交换 */
        void* temp = heap->elements[index];
        heap->elements[index] = heap->elements[smallest];
        heap->elements[smallest] = temp;
        
        index = smallest;
    }
}

/* 扩容堆 */
static int expand_heap(vox_mheap_t* heap) {
    size_t new_capacity = heap->capacity * 2;
    
    /* 检查容量溢出 */
    if (new_capacity < heap->capacity) {
        return -1;  /* 溢出 */
    }
    
    void** new_elements = (void**)vox_mpool_alloc(heap->mpool, new_capacity * sizeof(void*));
    if (!new_elements) {
        return -1;
    }
    
    /* 复制旧元素 */
    memcpy(new_elements, heap->elements, heap->size * sizeof(void*));
    
    /* 释放旧数组 */
    vox_mpool_free(heap->mpool, heap->elements);
    
    heap->elements = new_elements;
    heap->capacity = new_capacity;
    
    return 0;
}

/* 创建最小堆 */
vox_mheap_t* vox_mheap_create(vox_mpool_t* mpool) {
    return vox_mheap_create_with_config(mpool, NULL);
}

/* 使用配置创建最小堆 */
vox_mheap_t* vox_mheap_create_with_config(vox_mpool_t* mpool, const vox_mheap_config_t* config) {
    if (!mpool) return NULL;
    
    /* 使用内存池分配堆结构 */
    vox_mheap_t* heap = (vox_mheap_t*)vox_mpool_alloc(mpool, sizeof(vox_mheap_t));
    if (!heap) {
        return NULL;
    }
    
    /* 初始化结构 */
    memset(heap, 0, sizeof(vox_mheap_t));
    heap->mpool = mpool;
    heap->cmp_func = default_cmp;
    
    /* 设置默认值 */
    size_t initial_capacity = VOX_MHEAP_DEFAULT_INITIAL_CAPACITY;
    
    /* 应用配置 */
    if (config) {
        if (config->initial_capacity > 0) {
            initial_capacity = config->initial_capacity;
        }
        if (config->cmp_func) {
            heap->cmp_func = config->cmp_func;
        }
        if (config->elem_free) {
            heap->elem_free = config->elem_free;
        }
    }
    
    heap->capacity = initial_capacity;
    heap->size = 0;
    
    /* 分配元素数组 */
    heap->elements = (void**)vox_mpool_alloc(heap->mpool, heap->capacity * sizeof(void*));
    if (!heap->elements) {
        vox_mpool_free(heap->mpool, heap);
        return NULL;
    }
    
    return heap;
}

/* 插入元素 */
int vox_mheap_push(vox_mheap_t* heap, void* elem) {
    if (!heap || !elem) return -1;
    
    /* 检查是否需要扩容 */
    if (heap->size >= heap->capacity) {
        if (expand_heap(heap) != 0) {
            return -1;
        }
    }
    
    /* 将新元素添加到数组末尾 */
    heap->elements[heap->size] = elem;
    heap->size++;
    
    /* 向上调整 */
    heapify_up(heap, heap->size - 1);
    
    return 0;
}

/* 获取最小元素（不移除） */
void* vox_mheap_peek(const vox_mheap_t* heap) {
    if (!heap || heap->size == 0) return NULL;
    return heap->elements[0];
}

/* 移除并返回最小元素 */
void* vox_mheap_pop(vox_mheap_t* heap) {
    if (!heap || heap->size == 0) return NULL;
    
    /* 保存最小值 */
    void* min_elem = heap->elements[0];
    
    /* 将最后一个元素移到根节点 */
    heap->elements[0] = heap->elements[heap->size - 1];
    heap->size--;
    
    /* 如果堆不为空，向下调整 */
    if (heap->size > 0) {
        heapify_down(heap, 0);
    }
    
    return min_elem;
}

/* 获取元素数量 */
size_t vox_mheap_size(const vox_mheap_t* heap) {
    return heap ? heap->size : 0;
}

/* 检查是否为空 */
bool vox_mheap_empty(const vox_mheap_t* heap) {
    return heap ? (heap->size == 0) : true;
}

/* 清空堆 */
void vox_mheap_clear(vox_mheap_t* heap) {
    if (!heap) return;
    
    /* 释放所有元素（如果配置了释放函数） */
    if (heap->elem_free) {
        for (size_t i = 0; i < heap->size; i++) {
            if (heap->elements[i]) {
                heap->elem_free(heap->elements[i]);
            }
        }
    }
    
    heap->size = 0;
}

/* 销毁堆 */
void vox_mheap_destroy(vox_mheap_t* heap) {
    if (!heap) return;
    
    /* 清空所有元素 */
    if (heap->elem_free) {
        for (size_t i = 0; i < heap->size; i++) {
            if (heap->elements[i]) {
                heap->elem_free(heap->elements[i]);
            }
        }
    }
    
    /* 保存内存池指针 */
    vox_mpool_t* mpool = heap->mpool;
    
    /* 释放元素数组 */
    vox_mpool_free(mpool, heap->elements);
    
    /* 释放堆结构 */
    vox_mpool_free(mpool, heap);
}

/* 遍历堆 */
size_t vox_mheap_foreach(vox_mheap_t* heap,
                         void (*visit)(void* elem, void* user_data),
                         void* user_data) {
    if (!heap || !visit) return 0;

    size_t count = 0;
    for (size_t i = 0; i < heap->size; i++) {
        visit(heap->elements[i], user_data);
        count++;
    }

    return count;
}

/* 从堆中移除指定元素 */
int vox_mheap_remove(vox_mheap_t* heap, void* elem) {
    if (!heap || !elem || heap->size == 0) return -1;

    /* 查找元素位置 */
    size_t index = 0;
    bool found = false;
    for (size_t i = 0; i < heap->size; i++) {
        if (heap->elements[i] == elem) {
            index = i;
            found = true;
            break;
        }
    }

    if (!found) return -1;

    /* 如果是最后一个元素，直接移除 */
    if (index == heap->size - 1) {
        heap->size--;
        return 0;
    }

    /* 用最后一个元素替换要删除的元素 */
    heap->elements[index] = heap->elements[heap->size - 1];
    heap->size--;

    /* 调整堆：先尝试下沉，如果没有下沉则上浮 */
    size_t old_index = index;
    heapify_down(heap, index);

    /* 如果位置没变（没有下沉），尝试上浮 */
    if (index == old_index && index > 0) {
        heapify_up(heap, index);
    }

    return 0;
}
