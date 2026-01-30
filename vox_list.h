/*
 * vox_list.h - 侵入式双向链表
 * 提供高性能的双向链表实现，支持 O(1) 的插入和删除操作
 */

#ifndef VOX_LIST_H
#define VOX_LIST_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 类型定义 ===== */

/**
 * 链表节点结构
 * 用户数据结构需要嵌入此节点作为成员
 */
typedef struct vox_list_node {
    struct vox_list_node *prev;  /* 前驱节点指针 */
    struct vox_list_node *next;  /* 后继节点指针 */
} vox_list_node_t;

/**
 * 链表头结构
 */
typedef struct vox_list {
    vox_list_node_t head;  /* 哨兵节点 */
    size_t size;           /* 链表大小 */
} vox_list_t;

/* ===== 容器宏 ===== */

/**
 * 从成员指针获取包含该成员的结构体指针
 * @param ptr 成员指针
 * @param type 结构体类型
 * @param member 成员名称
 * @return 返回结构体指针
 */
#define vox_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* ===== 遍历宏 ===== */

/**
 * 遍历链表节点
 * @param pos 当前节点指针（vox_list_node_t*）
 * @param list 链表指针
 * 
 * 示例：
 * @code
 * vox_list_node_t *pos;
 * vox_list_for_each(pos, &list) {
 *     // 处理 pos
 * }
 * @endcode
 */
#define vox_list_for_each(pos, list) \
    for (pos = (list)->head.next; pos != &(list)->head; pos = pos->next)

/**
 * 安全遍历链表节点（支持在遍历时删除）
 * @param pos 当前节点指针（vox_list_node_t*）
 * @param n 下一个节点指针（vox_list_node_t*）
 * @param list 链表指针
 * 
 * 示例：
 * @code
 * vox_list_node_t *pos, *n;
 * vox_list_for_each_safe(pos, n, &list) {
 *     if (should_remove(pos)) {
 *         vox_list_remove(&list, pos);
 *     }
 * }
 * @endcode
 */
#define vox_list_for_each_safe(pos, n, list) \
    for (pos = (list)->head.next, n = pos->next; \
         pos != &(list)->head; \
         pos = n, n = pos->next)

/**
 * 遍历链表并获取容器对象
 * @param pos 当前容器对象指针
 * @param list 链表指针
 * @param type 容器类型
 * @param member 链表节点成员名称
 * 
 * 示例：
 * @code
 * typedef struct {
 *     int data;
 *     vox_list_node_t node;
 * } item_t;
 * 
 * item_t *pos;
 * vox_list_for_each_entry(pos, &list, item_t, node) {
 *     printf("%d\n", pos->data);
 * }
 * @endcode
 */
#define vox_list_for_each_entry(pos, list, type, member) \
    for (pos = vox_container_of((list)->head.next, type, member); \
         &pos->member != &(list)->head; \
         pos = vox_container_of(pos->member.next, type, member))

/* ===== 初始化函数 ===== */

/**
 * 初始化链表
 * @param list 链表指针
 */
static inline void vox_list_init(vox_list_t *list) {
    list->head.prev = &list->head;
    list->head.next = &list->head;
    list->size = 0;
}

/**
 * 初始化链表节点
 * @param node 节点指针
 */
static inline void vox_list_node_init(vox_list_node_t *node) {
    node->prev = node;
    node->next = node;
}

/* ===== 查询函数 ===== */

/**
 * 判断链表是否为空
 * @param list 链表指针
 * @return 为空返回 true，否则返回 false
 */
static inline bool vox_list_empty(const vox_list_t *list) {
    return list->head.next == &list->head;
}

/**
 * 获取链表大小
 * @param list 链表指针
 * @return 返回链表中的节点数量
 */
static inline size_t vox_list_size(const vox_list_t *list) {
    return list->size;
}

/**
 * 获取第一个节点
 * @param list 链表指针
 * @return 返回第一个节点指针，链表为空时返回 NULL
 */
static inline vox_list_node_t* vox_list_first(const vox_list_t *list) {
    return vox_list_empty(list) ? NULL : list->head.next;
}

/**
 * 获取最后一个节点
 * @param list 链表指针
 * @return 返回最后一个节点指针，链表为空时返回 NULL
 */
static inline vox_list_node_t* vox_list_last(const vox_list_t *list) {
    return vox_list_empty(list) ? NULL : list->head.prev;
}

/* ===== 内部辅助函数 ===== */

/**
 * 在两个节点之间插入新节点（内部函数）
 * @param node 要插入的节点
 * @param prev 前驱节点
 * @param next 后继节点
 */
static inline void vox_list_add_internal(vox_list_node_t *node, 
                                          vox_list_node_t *prev, 
                                          vox_list_node_t *next) {
    node->next = next;
    node->prev = prev;
    next->prev = node;
    prev->next = node;
}

/**
 * 删除两个节点之间的连接（内部函数）
 * @param prev 前驱节点
 * @param next 后继节点
 */
static inline void vox_list_del_internal(vox_list_node_t *prev, 
                                         vox_list_node_t *next) {
    next->prev = prev;
    prev->next = next;
}

/* ===== 插入函数 ===== */

/**
 * 在指定节点后插入新节点
 * @param list 链表指针
 * @param pos 位置节点指针
 * @param node 要插入的节点指针
 */
static inline void vox_list_insert_after(vox_list_t *list, 
                                         vox_list_node_t *pos, 
                                         vox_list_node_t *node) {
    vox_list_add_internal(node, pos, pos->next);
    list->size++;
}

/**
 * 在指定节点前插入新节点
 * @param list 链表指针
 * @param pos 位置节点指针
 * @param node 要插入的节点指针
 */
static inline void vox_list_insert_before(vox_list_t *list, 
                                          vox_list_node_t *pos, 
                                          vox_list_node_t *node) {
    vox_list_add_internal(node, pos->prev, pos);
    list->size++;
}

/**
 * 在链表头部插入节点
 * @param list 链表指针
 * @param node 要插入的节点指针
 */
static inline void vox_list_push_front(vox_list_t *list, vox_list_node_t *node) {
    vox_list_add_internal(node, &list->head, list->head.next);
    list->size++;
}

/**
 * 在链表尾部插入节点
 * @param list 链表指针
 * @param node 要插入的节点指针
 */
static inline void vox_list_push_back(vox_list_t *list, vox_list_node_t *node) {
    vox_list_add_internal(node, list->head.prev, &list->head);
    list->size++;
}

/* ===== 删除函数 ===== */

/**
 * 删除指定节点
 * @param list 链表指针
 * @param node 要删除的节点指针
 */
static inline void vox_list_remove(vox_list_t *list, vox_list_node_t *node) {
    vox_list_del_internal(node->prev, node->next);
    vox_list_node_init(node);
    list->size--;
}

/**
 * 删除并返回头部节点
 * @param list 链表指针
 * @return 返回被删除的节点指针，链表为空时返回 NULL
 */
static inline vox_list_node_t* vox_list_pop_front(vox_list_t *list) {
    if (vox_list_empty(list)) {
        return NULL;
    }
    vox_list_node_t *node = list->head.next;
    vox_list_remove(list, node);
    return node;
}

/**
 * 删除并返回尾部节点
 * @param list 链表指针
 * @return 返回被删除的节点指针，链表为空时返回 NULL
 */
static inline vox_list_node_t* vox_list_pop_back(vox_list_t *list) {
    if (vox_list_empty(list)) {
        return NULL;
    }
    vox_list_node_t *node = list->head.prev;
    vox_list_remove(list, node);
    return node;
}

/* ===== 高级操作 ===== */

/**
 * 将节点从一个链表移动到另一个链表的指定位置
 * @param from_list 源链表指针
 * @param to_list 目标链表指针
 * @param pos 目标位置节点指针（新节点将插入到此节点之后）
 * @param node 要移动的节点指针
 */
static inline void vox_list_move_after(vox_list_t *from_list,
                                       vox_list_t *to_list,
                                       vox_list_node_t *pos,
                                       vox_list_node_t *node) {
    vox_list_del_internal(node->prev, node->next);
    from_list->size--;
    vox_list_add_internal(node, pos, pos->next);
    to_list->size++;
}

/**
 * 拼接两个链表
 * 将 other 链表的所有节点追加到 list 链表的尾部，并清空 other 链表
 * @param list 目标链表指针
 * @param other 源链表指针（拼接后将被清空）
 */
static inline void vox_list_splice(vox_list_t *list, vox_list_t *other) {
    if (vox_list_empty(other)) {
        return;
    }
    
    vox_list_node_t *first = other->head.next;
    vox_list_node_t *last = other->head.prev;
    vox_list_node_t *at = list->head.prev;
    
    first->prev = at;
    at->next = first;
    
    last->next = &list->head;
    list->head.prev = last;
    
    list->size += other->size;
    vox_list_init(other);
}

/**
 * 清空链表（不释放节点内存）
 * @param list 链表指针
 */
static inline void vox_list_clear(vox_list_t *list) {
    vox_list_init(list);
}

#ifdef __cplusplus
}
#endif

#endif /* VOX_LIST_H */
