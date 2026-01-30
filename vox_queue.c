/*
 * vox_queue.c - 高性能队列实现
 * 使用循环数组（circular buffer）实现，O(1) 入队和出队操作
 * 使用 vox_mpool 内存池管理所有内存分配
 */

#include "vox_queue.h"
#include "vox_mpool.h"
#include "vox_atomic.h"
#include "vox_thread.h"
#include "vox_os.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* 默认初始容量 */
#define VOX_QUEUE_DEFAULT_INITIAL_CAPACITY 16

/* 队列槽结构（用于 MPSC 队列的序列号方案） */
typedef struct {
    void* data;                      /* 元素数据 */
    vox_atomic_long_t* sequence;     /* 序列号（用于同步） */
} vox_queue_slot_t;

/* 队列结构 */
struct vox_queue {
    vox_mpool_t* mpool;      /* 内存池 */
    vox_queue_type_t type;   /* 队列类型 */
    void** elements;         /* 元素数组（普通队列和 SPSC） */
    vox_queue_slot_t* slots; /* 槽数组（MPSC 使用） */
    size_t capacity;         /* 数组容量（SPSC/MPSC为固定容量） */
    vox_queue_free_func_t elem_free; /* 元素释放函数 */

    /* 普通队列字段 */
    size_t size;             /* 当前元素数量 */
    size_t head;             /* 队首索引 */
    size_t tail;             /* 队尾索引 */

    /* 无锁队列字段（SPSC/MPSC） */
    vox_atomic_long_t* atomic_head;  /* 原子队首索引 */
    vox_atomic_long_t* atomic_tail;  /* 原子队尾索引 */
    size_t mask;             /* 容量掩码（capacity - 1，用于快速取模） */
};

/* 扩容队列 */
static int expand_queue(vox_queue_t* queue) {
    size_t new_capacity = queue->capacity * 2;
    
    /* 检查容量溢出 */
    if (new_capacity < queue->capacity) {
        return -1;  /* 溢出 */
    }
    
    void** new_elements = (void**)vox_mpool_alloc(queue->mpool, new_capacity * sizeof(void*));
    if (!new_elements) {
        return -1;
    }
    
    /* 复制元素（从head到tail，考虑循环） */
    if (queue->head < queue->tail) {
        /* 元素连续存储 */
        memcpy(new_elements, &queue->elements[queue->head], queue->size * sizeof(void*));
    } else if (queue->size > 0) {
        /* 元素跨越数组边界 */
        size_t first_part = queue->capacity - queue->head;
        memcpy(new_elements, &queue->elements[queue->head], first_part * sizeof(void*));
        memcpy(&new_elements[first_part], queue->elements, queue->tail * sizeof(void*));
    }
    
    /* 释放旧数组 */
    vox_mpool_free(queue->mpool, queue->elements);
    
    queue->elements = new_elements;
    queue->head = 0;
    queue->tail = queue->size;
    queue->capacity = new_capacity;
    
    return 0;
}

/* 创建队列 */
vox_queue_t* vox_queue_create(vox_mpool_t* mpool) {
    return vox_queue_create_with_config(mpool, NULL);
}

/* 使用配置创建队列 */
vox_queue_t* vox_queue_create_with_config(vox_mpool_t* mpool, const vox_queue_config_t* config) {
    if (!mpool) return NULL;

    /* 使用内存池分配队列结构 */
    vox_queue_t* queue = (vox_queue_t*)vox_mpool_alloc(mpool, sizeof(vox_queue_t));
    if (!queue) {
        return NULL;
    }

    /* 初始化结构 */
    memset(queue, 0, sizeof(vox_queue_t));
    queue->mpool = mpool;

    /* 设置默认值 */
    vox_queue_type_t type = VOX_QUEUE_TYPE_NORMAL;
    size_t initial_capacity = VOX_QUEUE_DEFAULT_INITIAL_CAPACITY;

    /* 应用配置 */
    if (config) {
        type = config->type;
        if (config->initial_capacity > 0) {
            initial_capacity = config->initial_capacity;
        }
        if (config->elem_free) {
            queue->elem_free = config->elem_free;
        }
    }

    queue->type = type;

    /* SPSC/MPSC 需要容量为2的幂，且必须指定 */
    if ((type == VOX_QUEUE_TYPE_SPSC) || (type == VOX_QUEUE_TYPE_MPSC)) {
        if (config && config->initial_capacity == 0) {
            vox_mpool_free(mpool, queue);
            return NULL;  /* SPSC/MPSC 必须指定容量 */
        }

        /* 确保容量是2的幂 */
        size_t cap = initial_capacity;
        if ((cap & (cap - 1)) != 0) {
            /* 向上取到最近的2的幂 */
            cap--;
            cap |= cap >> 1;
            cap |= cap >> 2;
            cap |= cap >> 4;
            cap |= cap >> 8;
            cap |= cap >> 16;
            #if SIZE_MAX > 0xFFFFFFFFUL
            cap |= cap >> 32;
            #endif
            cap++;
        }
        initial_capacity = cap;
        queue->mask = cap - 1;  /* 用于快速取模 */

        /* 创建原子变量 */
        queue->atomic_head = vox_atomic_long_create(mpool, 0);
        queue->atomic_tail = vox_atomic_long_create(mpool, 0);
        if (!queue->atomic_head || !queue->atomic_tail) {
            if (queue->atomic_head) vox_atomic_long_destroy(queue->atomic_head);
            if (queue->atomic_tail) vox_atomic_long_destroy(queue->atomic_tail);
            vox_mpool_free(mpool, queue);
            return NULL;
        }
    } else {
        queue->size = 0;
        queue->head = 0;
        queue->tail = 0;
    }

    queue->capacity = initial_capacity;

    /* 分配元素数组或槽数组 */
    if (type == VOX_QUEUE_TYPE_MPSC) {
        /* MPSC 使用槽数组（带序列号） */
        queue->slots = (vox_queue_slot_t*)vox_mpool_alloc(mpool,
            initial_capacity * sizeof(vox_queue_slot_t));
        if (!queue->slots) {
            vox_atomic_long_destroy(queue->atomic_head);
            vox_atomic_long_destroy(queue->atomic_tail);
            vox_mpool_free(mpool, queue);
            return NULL;
        }

        /* 初始化每个槽的序列号 */
        for (size_t i = 0; i < initial_capacity; i++) {
            queue->slots[i].data = NULL;
            queue->slots[i].sequence = vox_atomic_long_create(mpool, (int64_t)i);
            if (!queue->slots[i].sequence) {
                /* 清理已创建的序列号 */
                for (size_t j = 0; j < i; j++) {
                    vox_atomic_long_destroy(queue->slots[j].sequence);
                }
                vox_mpool_free(mpool, queue->slots);
                vox_atomic_long_destroy(queue->atomic_head);
                vox_atomic_long_destroy(queue->atomic_tail);
                vox_mpool_free(mpool, queue);
                return NULL;
            }
        }
        queue->elements = NULL;
    } else {
        /* 普通队列和 SPSC 使用元素数组 */
        queue->elements = (void**)vox_mpool_alloc(mpool, initial_capacity * sizeof(void*));
        if (!queue->elements) {
            if (queue->atomic_head) vox_atomic_long_destroy(queue->atomic_head);
            if (queue->atomic_tail) vox_atomic_long_destroy(queue->atomic_tail);
            vox_mpool_free(mpool, queue);
            return NULL;
        }
        memset(queue->elements, 0, initial_capacity * sizeof(void*));
        queue->slots = NULL;
    }

    return queue;
}

/* 在队列末尾添加元素（入队） */
int vox_queue_enqueue(vox_queue_t* queue, void* elem) {
    if (!queue || !elem) return -1;
    
    if (queue->type == VOX_QUEUE_TYPE_SPSC) {
        /* SPSC 无锁入队（单生产者） */
        int64_t tail = vox_atomic_long_load(queue->atomic_tail);
        int64_t head = vox_atomic_long_load_acquire(queue->atomic_head);
        int64_t next_tail = (tail + 1) & queue->mask;

        /* 检查队列是否已满 */
        if (next_tail == head) {
            return -1;  /* 队列已满 */
        }

        /* 写入元素（普通写入，因为只有单生产者） */
        queue->elements[tail] = elem;

        /* 更新 tail（使用 release 语义，确保元素写入对消费者可见） */
        vox_atomic_long_store_release(queue->atomic_tail, next_tail);

        return 0;
    } else if (queue->type == VOX_QUEUE_TYPE_MPSC) {
        /* MPSC 无锁入队（多生产者，使用序列号方案） */
        int64_t pos;
        vox_queue_slot_t* slot;

        while (true) {
            pos = vox_atomic_long_load(queue->atomic_tail);
            slot = &queue->slots[pos & queue->mask];
            int64_t seq = vox_atomic_long_load_acquire(slot->sequence);
            int64_t diff = seq - pos;

            if (diff == 0) {
                /* 槽位可用，尝试原子性地预留 */
                int64_t expected = pos;
                if (vox_atomic_long_compare_exchange(queue->atomic_tail, &expected, pos + 1)) {
                    break;  /* 成功预留槽位 */
                }
                /* CAS 失败，其他生产者已经更新了 tail，重试 */
                continue;
            } else if (diff < 0) {
                /* diff < 0 说明 seq < pos，意味着：
                 * 1. 槽位还在被消费者使用（序列号为 pos - capacity + ...）
                 * 2. 或者队列已满（tail 已经超前 head 一圈）
                 * 在标准的序列号队列中，这表示队列已满 */
                return -1;  /* 队列已满 */
            } else {
                /* diff > 0 说明 seq > pos，意味着：
                 * 1. seq == pos + 1: 其他生产者刚写入数据到此槽位
                 * 2. seq > pos + 1: 异常状态（不应该发生）
                 * 在任何情况下，都需要等待 tail 前进或重新加载 */
                vox_thread_yield();
                continue;
            }
        }

        /* 写入数据（普通写入，因为此槽位已被当前生产者独占） */
        slot->data = elem;

        /* 更新序列号，标记槽位已就绪（使用 release 确保数据写入对消费者可见） */
        vox_atomic_long_store_release(slot->sequence, pos + 1);

        return 0;
    } else {
        /* 普通队列入队 */
        /* 检查是否需要扩容 */
        if (queue->size >= queue->capacity) {
            if (expand_queue(queue) != 0) {
                return -1;
            }
        }
        
        /* 在队尾添加元素 */
        queue->elements[queue->tail] = elem;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->size++;
        
        return 0;
    }
}

/* 移除并返回队列首部的元素（出队） */
void* vox_queue_dequeue(vox_queue_t* queue) {
    if (!queue) return NULL;
    
    if (queue->type == VOX_QUEUE_TYPE_SPSC) {
        /* SPSC 无锁出队（单消费者） */
        int64_t head = vox_atomic_long_load(queue->atomic_head);
        int64_t tail = vox_atomic_long_load_acquire(queue->atomic_tail);

        /* 检查队列是否为空 */
        if (head == tail) {
            return NULL;  /* 队列为空 */
        }

        /* 读取元素（普通读取，因为只有单消费者，且 acquire 已确保可见性） */
        void* elem = queue->elements[head];

        /* 更新 head（使用 release 语义，让生产者能看到空出的槽位） */
        int64_t next_head = (head + 1) & queue->mask;
        vox_atomic_long_store_release(queue->atomic_head, next_head);

        return elem;
    } else if (queue->type == VOX_QUEUE_TYPE_MPSC) {
        /* MPSC 无锁出队（支持多消费者，使用序列号方案和 CAS） */
        while (true) {
            int64_t pos = vox_atomic_long_load(queue->atomic_head);
            int64_t tail = vox_atomic_long_load(queue->atomic_tail);

            /* 检查队列是否为空 */
            if (pos == tail) {
                return NULL;  /* 队列为空 */
            }

            /* 获取槽位引用 */
            vox_queue_slot_t* slot = &queue->slots[pos & queue->mask];
            int64_t seq = vox_atomic_long_load_acquire(slot->sequence);
            int64_t diff = seq - (pos + 1);

            if (diff < 0) {
                /* 槽位尚未就绪（生产者还没写完），等待并重试 */
                vox_thread_yield();
                continue;
            }

            /* 原子性地更新 head */
            int64_t expected = pos;
            if (!vox_atomic_long_compare_exchange(queue->atomic_head, &expected, pos + 1)) {
                /* CAS 失败，其他消费者已经取走了这个元素，重试 */
                continue;
            }

            /* 读取数据（acquire 已确保数据可见性） */
            void* elem = slot->data;

            /* 更新序列号，标记槽位可重用（使用 release 让生产者能看到） */
            vox_atomic_long_store_release(slot->sequence, pos + (int64_t)queue->capacity);

            return elem;
        }
    } else {
        /* 普通队列出队 */
        if (queue->size == 0) return NULL;
        
        /* 获取队首元素 */
        void* elem = queue->elements[queue->head];
        
        /* 移动队首指针 */
        queue->head = (queue->head + 1) % queue->capacity;
        queue->size--;
        
        return elem;
    }
}

/* 获取队列首部元素（不移除） */
void* vox_queue_peek(const vox_queue_t* queue) {
    if (!queue) return NULL;

    if (queue->type == VOX_QUEUE_TYPE_SPSC) {
        /* SPSC 无锁查看 */
        int64_t head = vox_atomic_long_load(queue->atomic_head);
        int64_t tail = vox_atomic_long_load_acquire(queue->atomic_tail);

        if (head == tail) {
            return NULL;  /* 队列为空 */
        }

        return queue->elements[head];
    } else if (queue->type == VOX_QUEUE_TYPE_MPSC) {
        /* MPSC 无锁查看 */
        int64_t pos = vox_atomic_long_load(queue->atomic_head);
        int64_t tail = vox_atomic_long_load(queue->atomic_tail);

        /* 检查队列是否为空 */
        if (pos == tail) {
            return NULL;  /* 队列为空 */
        }

        vox_queue_slot_t* slot = &queue->slots[pos & queue->mask];
        int64_t seq = vox_atomic_long_load_acquire(slot->sequence);

        /* 使用与 dequeue 相同的逻辑来检查槽位是否就绪 */
        int64_t diff = seq - (pos + 1);
        if (diff < 0) {
            return NULL;  /* 槽位尚未就绪 */
        }

        return slot->data;
    } else {
        /* 普通队列查看 */
        if (queue->size == 0) return NULL;
        return queue->elements[queue->head];
    }
}

/* 获取队列中的元素数量 */
size_t vox_queue_size(const vox_queue_t* queue) {
    if (!queue) return 0;
    
    if (queue->type == VOX_QUEUE_TYPE_SPSC || queue->type == VOX_QUEUE_TYPE_MPSC) {
        /* SPSC/MPSC 无锁获取大小 */
        int64_t head = vox_atomic_long_load(queue->atomic_head);
        int64_t tail = vox_atomic_long_load(queue->atomic_tail);
        
        int64_t size = tail - head;
        
        /* 修复负数情况：对于MPSC队列，由于并发操作，理论上不应该出现负数，
         * 但如果出现则说明有同步问题，此时返回0作为安全值 */
        if (size < 0) {
            return 0;
        }
        
        /* 确保大小不超过队列容量 */
        if (size > (int64_t)queue->capacity) {
            return queue->capacity;
        }
        
        return (size_t)size;
    } else {
        return queue->size;
    }
}

/* 获取队列的容量 */
size_t vox_queue_capacity(const vox_queue_t* queue) {
    return queue ? queue->capacity : 0;
}

/* 检查队列是否为空 */
bool vox_queue_empty(const vox_queue_t* queue) {
    if (!queue) return true;
    
    if (queue->type == VOX_QUEUE_TYPE_SPSC || queue->type == VOX_QUEUE_TYPE_MPSC) {
        /* SPSC/MPSC 无锁检查 */
        int64_t head = vox_atomic_long_load(queue->atomic_head);
        int64_t tail = vox_atomic_long_load(queue->atomic_tail);
        
        /* 对于MPSC队列，可能存在元素已经在槽位中但还没有被正确访问的情况 */
        /* 但我们仍然使用基本的 head == tail 检查，这是无锁队列的标准做法 */
        if (queue->type == VOX_QUEUE_TYPE_MPSC) {
            return head == tail;
        } else {
            /* SPSC队列使用简单的head==tail检查 */
            return head == tail;
        }
    } else {
        return queue->size == 0;
    }
}

/* 检查队列是否已满 */
bool vox_queue_full(const vox_queue_t* queue) {
    if (!queue) return false;

    if (queue->type == VOX_QUEUE_TYPE_SPSC) {
        /* SPSC 无锁检查 */
        int64_t head = vox_atomic_long_load(queue->atomic_head);
        int64_t tail = vox_atomic_long_load(queue->atomic_tail);
        int64_t next_tail = (tail + 1) & queue->mask;
        return next_tail == head;
    } else if (queue->type == VOX_QUEUE_TYPE_MPSC) {
        /* MPSC 检查队列是否已满 */
        int64_t head = vox_atomic_long_load(queue->atomic_head);
        int64_t tail = vox_atomic_long_load(queue->atomic_tail);
        /* tail 和 head 是序列号，它们的差值表示队列中的元素数 */
        int64_t size = tail - head;
        /* 防止整数溢出，如果计算出的大小为负数，则认为队列未满 */
        if (size < 0) {
            return false;  /* 队列未满 */
        }
        return size >= (int64_t)queue->capacity;
    } else {
        return queue->size >= queue->capacity;
    }
}

/* 清空队列 */
void vox_queue_clear(vox_queue_t* queue) {
    if (!queue) return;

    if (queue->type == VOX_QUEUE_TYPE_SPSC) {
        /* SPSC 无锁清空（注意：应该由消费者或在无并发操作时调用） */
        if (queue->elem_free) {
            int64_t head = vox_atomic_long_load(queue->atomic_head);
            int64_t tail = vox_atomic_long_load_acquire(queue->atomic_tail);

            while (head != tail) {
                if (queue->elements[head]) {
                    queue->elem_free(queue->elements[head]);
                }
                head = (head + 1) & queue->mask;
            }
        }

        vox_atomic_long_store(queue->atomic_head, 0);
        vox_atomic_long_store(queue->atomic_tail, 0);
    } else if (queue->type == VOX_QUEUE_TYPE_MPSC) {
        /* MPSC 清空（注意：调用者必须确保在清空期间没有并发的入队操作）*/
        /* 对于 MPSC 队列，如果在有活跃生产者的情况下调用 clear，会导致未定义行为 */
        /* 正确的使用方式是：先停止所有生产者，然后调用 clear */

        int64_t head = vox_atomic_long_load(queue->atomic_head);
        int64_t tail = vox_atomic_long_load(queue->atomic_tail);

        /* 遍历所有可能包含数据的槽位并清理 */
        if (queue->elem_free) {
            for (int64_t pos = head; pos < tail; pos++) {
                vox_queue_slot_t* slot = &queue->slots[pos & queue->mask];
                int64_t seq = vox_atomic_long_load(slot->sequence);
                /* 检查槽位是否已就绪（序列号 == pos + 1） */
                if (seq == pos + 1 && slot->data) {
                    queue->elem_free(slot->data);
                    slot->data = NULL;
                }
            }
        }

        /* 重置所有序列号为初始值 */
        for (size_t i = 0; i < queue->capacity; i++) {
            vox_atomic_long_store(queue->slots[i].sequence, (int64_t)i);
            queue->slots[i].data = NULL;
        }

        /* 重置 head 和 tail 为 0 */
        vox_atomic_long_store(queue->atomic_head, 0);
        vox_atomic_long_store(queue->atomic_tail, 0);
    } else {
        /* 普通队列清空 */
        if (queue->elem_free) {
            size_t count = queue->size;
            size_t idx = queue->head;
            for (size_t i = 0; i < count; i++) {
                if (queue->elements[idx]) {
                    queue->elem_free(queue->elements[idx]);
                }
                idx = (idx + 1) % queue->capacity;
            }
        }

        queue->size = 0;
        queue->head = 0;
        queue->tail = 0;
    }
}

/* 遍历队列 */
size_t vox_queue_foreach(vox_queue_t* queue, vox_queue_visit_func_t visit, void* user_data) {
    if (!queue || !visit) return 0;

    if (queue->type == VOX_QUEUE_TYPE_SPSC) {
        /* SPSC 无锁遍历（注意：在并发环境下可能不准确） */
        int64_t head = vox_atomic_long_load(queue->atomic_head);
        int64_t tail = vox_atomic_long_load_acquire(queue->atomic_tail);
        size_t count = 0;
        size_t index = 0;

        while (head != tail) {
            visit(queue->elements[head], index, user_data);
            head = (head + 1) & queue->mask;
            count++;
            index++;
        }

        return count;
    } else if (queue->type == VOX_QUEUE_TYPE_MPSC) {
        /* MPSC 遍历（注意：在并发环境下可能不准确） */
        int64_t head = vox_atomic_long_load(queue->atomic_head);
        int64_t tail = vox_atomic_long_load(queue->atomic_tail);
        size_t count = 0;
        size_t index = 0;

        while (head != tail) {
            vox_queue_slot_t* slot = &queue->slots[head & queue->mask];
            int64_t seq = vox_atomic_long_load_acquire(slot->sequence);
            if (seq == head + 1) {
                visit(slot->data, index, user_data);
                count++;
            }
            head++;
            index++;
        }

        return count;
    } else {
        /* 普通队列遍历 */
        size_t count = 0;
        size_t idx = queue->head;
        for (size_t i = 0; i < queue->size; i++) {
            visit(queue->elements[idx], i, user_data);
            idx = (idx + 1) % queue->capacity;
            count++;
        }

        return count;
    }
}

/* 销毁队列 */
void vox_queue_destroy(vox_queue_t* queue) {
    if (!queue) return;

    /* 清空所有元素 */
    vox_queue_clear(queue);

    /* 保存内存池指针 */
    vox_mpool_t* mpool = queue->mpool;

    /* 销毁原子变量 */
    if (queue->atomic_head) {
        vox_atomic_long_destroy(queue->atomic_head);
    }
    if (queue->atomic_tail) {
        vox_atomic_long_destroy(queue->atomic_tail);
    }

    /* 释放槽数组（MPSC） */
    if (queue->slots) {
        for (size_t i = 0; i < queue->capacity; i++) {
            if (queue->slots[i].sequence) {
                vox_atomic_long_destroy(queue->slots[i].sequence);
            }
        }
        vox_mpool_free(mpool, queue->slots);
    }

    /* 释放元素数组 */
    if (queue->elements) {
        vox_mpool_free(mpool, queue->elements);
    }

    /* 释放队列结构 */
    vox_mpool_free(mpool, queue);
}
