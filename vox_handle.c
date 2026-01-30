/*
 * vox_handle.c - 句柄系统实现
 */

#include "vox_handle.h"
#include "vox_loop.h"
#include "vox_mpool.h"
#include "vox_log.h"
#include <string.h>

/* 关闭请求结构 */
typedef struct {
    vox_handle_t* handle;
    vox_handle_close_cb close_cb;
} vox_close_request_t;

/* 初始化句柄 */
int vox_handle_init(vox_handle_t* handle, vox_handle_type_t type, vox_loop_t* loop) {
    if (!handle || !loop) {
        VOX_LOG_ERROR("Invalid handle or loop pointer");
        return -1;
    }
    
    if (type <= VOX_HANDLE_UNKNOWN || type >= VOX_HANDLE_MAX) {
        VOX_LOG_ERROR("Invalid handle type: %d", type);
        return -1;
    }
    
    memset(handle, 0, sizeof(vox_handle_t));
    
    handle->type = type;
    handle->loop = loop;
    handle->ref_count = 1;  /* 初始引用计数为1 */
    handle->closing = false;
    handle->active = false;
    handle->data = NULL;
    handle->close_cb = NULL;
    handle->flags = 0;
    
    /* 初始化链表节点 */
    vox_list_node_init(&handle->node);
    
    return 0;
}

/* 增加句柄引用计数 */
uint32_t vox_handle_ref(vox_handle_t* handle) {
    if (!handle) {
        return 0;
    }
    
    handle->ref_count++;
    return handle->ref_count;
}

/* 减少句柄引用计数 */
uint32_t vox_handle_unref(vox_handle_t* handle) {
    if (!handle) {
        return 0;
    }
    
    if (handle->ref_count == 0) {
        return 0;
    }
    
    handle->ref_count--;
    
    /* 如果引用计数为0且正在关闭，将其加入关闭队列等待处理 */
    if (handle->ref_count == 0 && handle->closing) {
        vox_loop_t* loop = handle->loop;
        vox_list_t* closing_handles = vox_loop_get_closing_handles(loop);
        if (closing_handles) {
            /* 确保不再参与活跃列表 */
            if (handle->active) {
                vox_handle_deactivate(handle);
            }
            /* 如果节点目前是隔离状态（不在任何链表中），则加入关闭列表 */
            if (handle->node.next == &handle->node) {
                vox_list_push_back(closing_handles, &handle->node);
            }
        }
    }
    
    return handle->ref_count;
}

/* 检查句柄是否活跃 */
bool vox_handle_is_active(const vox_handle_t* handle) {
    return handle ? handle->active : false;
}

/* 检查句柄是否正在关闭 */
bool vox_handle_is_closing(const vox_handle_t* handle) {
    return handle ? handle->closing : false;
}

/* 关闭句柄 */
int vox_handle_close(vox_handle_t* handle, vox_handle_close_cb close_cb) {
    if (!handle) {
        return -1;
    }
    
    if (handle->closing) {
        return 0;  /* 已经在关闭中 */
    }
    
    handle->closing = true;
    handle->close_cb = close_cb;
    
    /* 从活跃句柄列表移除（不再参与 loop 存活判定） */
    if (handle->active) {
        vox_handle_deactivate(handle);
    }
    
    /* 触发 unref。由于 init 时 ref_count 为 1，
     * 如果此时没有其他异步操作持有引用，ref_count 将变为 0，
     * 并在 unref 中被加入到 loop->closing_handles。 */
    vox_handle_unref(handle);
    
    return 0;
}

/* 将句柄添加到活跃句柄列表 */
int vox_handle_activate(vox_handle_t* handle) {
    if (!handle || !handle->loop) {
        return -1;
    }
    
    if (handle->active) {
        return 0;  /* 已经在列表中 */
    }
    
    if (handle->closing) {
        return -1;  /* 正在关闭，不能激活 */
    }
    
    /* 添加到活跃句柄列表 */
    vox_list_t* active_handles = vox_loop_get_active_handles(handle->loop);
    if (active_handles) {
        vox_list_push_back(active_handles, &handle->node);
        handle->active = true;
        vox_loop_increment_active_handles(handle->loop);
    }
    
    return 0;
}

/* 将句柄从活跃句柄列表移除 */
int vox_handle_deactivate(vox_handle_t* handle) {
    if (!handle || !handle->loop) {
        return -1;
    }
    
    if (!handle->active) {
        return 0;  /* 不在列表中 */
    }
    
    /* 从活跃句柄列表移除 */
    vox_list_t* active_handles = vox_loop_get_active_handles(handle->loop);
    if (active_handles) {
        vox_list_remove(active_handles, &handle->node);
        handle->active = false;
        vox_loop_decrement_active_handles(handle->loop);
    }
    
    return 0;
}

/* 获取句柄类型 */
vox_handle_type_t vox_handle_get_type(const vox_handle_t* handle) {
    return handle ? handle->type : VOX_HANDLE_UNKNOWN;
}

/* 获取句柄所属的事件循环 */
vox_loop_t* vox_handle_get_loop(const vox_handle_t* handle) {
    return handle ? handle->loop : NULL;
}

/* 设置句柄的用户数据 */
void vox_handle_set_data(vox_handle_t* handle, void* data) {
    if (handle) {
        handle->data = data;
    }
}

/* 获取句柄的用户数据 */
void* vox_handle_get_data(const vox_handle_t* handle) {
    return handle ? handle->data : NULL;
}

/* 获取句柄的引用计数 */
uint32_t vox_handle_get_ref_count(const vox_handle_t* handle) {
    return handle ? handle->ref_count : 0;
}

/* 处理关闭的句柄（在事件循环迭代结束时调用） */
void vox_handle_process_closing(vox_loop_t* loop) {
    if (!loop) {
        return;
    }
    
    /* 获取关闭队列 */
    vox_list_t* closing_handles = vox_loop_get_closing_handles(loop);
    if (!closing_handles || vox_list_empty(closing_handles)) {
        return;
    }
    
    /* 转移到临时队列，防止回调中又有新的关闭请求影响遍历 */
    vox_list_t tmp_queue;
    vox_list_init(&tmp_queue);
    vox_list_splice(&tmp_queue, closing_handles);
    
    vox_list_node_t* node;
    vox_list_node_t* next;
    
    vox_list_for_each_safe(node, next, &tmp_queue) {
        vox_handle_t* handle = vox_container_of(node, vox_handle_t, node);
        
        /* 调用关闭回调 */
        if (handle->close_cb) {
            handle->close_cb(handle);
        }
        
        /* 此时 handle 的生命周期已结束。
         * 如果 handle 是由 vox_mpool 分配的，子类的析构回调应该已经释放了内存。
         * 这里将节点恢复为隔离状态以防万一。 */
        vox_list_node_init(&handle->node);
    }
}
