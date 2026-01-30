/*
 * list_example.c - 链表示例程序
 * 演示 vox_list 的基本用法
 */

#include "../vox_list.h"
#include "../vox_mpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// 定义数据结构
typedef struct student {
    int id;
    char name[32];
    vox_list_node_t list_node;  // 嵌入链表节点
} student_t;

// 打印学生信息
static void print_student(const student_t* s) {
    printf("  学生 ID: %d, 姓名: %s\n", s->id, s->name);
}

// 打印链表信息
static void print_list_info(const vox_list_t* list) {
    printf("  链表大小: %zu, 是否为空: %s\n", 
           vox_list_size(list), 
           vox_list_empty(list) ? "是" : "否");
}

int main(void) {
    printf("=== 创建链表和内存池 ===\n");
    vox_list_t students;
    vox_list_init(&students);
    vox_mpool_t* pool = vox_mpool_create();
    if (!pool) {
        fprintf(stderr, "创建内存池失败\n");
        return 1;
    }
    print_list_info(&students);

    printf("\n=== 测试 push_back（尾部插入） ===\n");
    for (int i = 0; i < 5; i++) {
        student_t *s = (student_t*)vox_mpool_alloc(pool, sizeof(student_t));
        if (!s) {
            fprintf(stderr, "分配内存失败\n");
            continue;
        }
        s->id = i;
        snprintf(s->name, sizeof(s->name), "学生%d", i);
        vox_list_push_back(&students, &s->list_node);
        printf("添加: ");
        print_student(s);
    }
    print_list_info(&students);

    printf("\n=== 测试 push_front（头部插入） ===\n");
    student_t *s_front = (student_t*)vox_mpool_alloc(pool, sizeof(student_t));
    if (s_front) {
        s_front->id = 100;
        snprintf(s_front->name, sizeof(s_front->name), "头部学生");
        vox_list_push_front(&students, &s_front->list_node);
        printf("在头部添加: ");
        print_student(s_front);
    }
    print_list_info(&students);

    printf("\n=== 遍历链表（vox_list_for_each_entry） ===\n");
    student_t *pos;
    vox_list_for_each_entry(pos, &students, student_t, list_node) {
        print_student(pos);
    }

    printf("\n=== 测试获取第一个和最后一个节点 ===\n");
    vox_list_node_t *first_node = vox_list_first(&students);
    if (first_node) {
        student_t *first = vox_container_of(first_node, student_t, list_node);
        printf("第一个节点: ");
        print_student(first);
    }
    
    vox_list_node_t *last_node = vox_list_last(&students);
    if (last_node) {
        student_t *last = vox_container_of(last_node, student_t, list_node);
        printf("最后一个节点: ");
        print_student(last);
    }

    printf("\n=== 测试 insert_after（在指定节点后插入） ===\n");
    // 在第一个节点后插入
    if (first_node) {
        student_t *s_new = (student_t*)vox_mpool_alloc(pool, sizeof(student_t));
        if (s_new) {
            s_new->id = 200;
            snprintf(s_new->name, sizeof(s_new->name), "插入的学生");
            vox_list_insert_after(&students, first_node, &s_new->list_node);
            printf("在第一个节点后插入: ");
            print_student(s_new);
        }
    }
    print_list_info(&students);

    printf("\n=== 测试 insert_before（在指定节点前插入） ===\n");
    // 在最后一个节点前插入
    if (last_node) {
        student_t *s_before = (student_t*)vox_mpool_alloc(pool, sizeof(student_t));
        if (s_before) {
            s_before->id = 300;
            snprintf(s_before->name, sizeof(s_before->name), "前置插入的学生");
            vox_list_insert_before(&students, last_node, &s_before->list_node);
            printf("在最后一个节点前插入: ");
            print_student(s_before);
        }
    }
    print_list_info(&students);

    printf("\n=== 再次遍历链表 ===\n");
    vox_list_for_each_entry(pos, &students, student_t, list_node) {
        print_student(pos);
    }

    printf("\n=== 测试 pop_front（删除头部节点） ===\n");
    vox_list_node_t *popped_front = vox_list_pop_front(&students);
    if (popped_front) {
        student_t *s = vox_container_of(popped_front, student_t, list_node);
        printf("删除的头部节点: ");
        print_student(s);
    }
    print_list_info(&students);

    printf("\n=== 测试 pop_back（删除尾部节点） ===\n");
    vox_list_node_t *popped_back = vox_list_pop_back(&students);
    if (popped_back) {
        student_t *s = vox_container_of(popped_back, student_t, list_node);
        printf("删除的尾部节点: ");
        print_student(s);
    }
    print_list_info(&students);

    printf("\n=== 测试 remove（删除指定节点） ===\n");
    // 找到并删除 ID 为 2 的节点
    student_t *to_remove = NULL;
    vox_list_for_each_entry(pos, &students, student_t, list_node) {
        if (pos->id == 2) {
            to_remove = pos;
            break;
        }
    }
    if (to_remove) {
        printf("删除节点: ");
        print_student(to_remove);
        vox_list_remove(&students, &to_remove->list_node);
    }
    print_list_info(&students);

    printf("\n=== 删除后的链表 ===\n");
    vox_list_for_each_entry(pos, &students, student_t, list_node) {
        print_student(pos);
    }

    printf("\n=== 测试安全遍历和删除（vox_list_for_each_safe） ===\n");
    vox_list_node_t *node, *n;
    int removed_count = 0;
    vox_list_for_each_safe(node, n, &students) {
        student_t *s = vox_container_of(node, student_t, list_node);
        // 删除 ID 为奇数的节点
        if (s->id % 2 == 1) {
            printf("安全删除: ");
            print_student(s);
            vox_list_remove(&students, node);
            removed_count++;
        }
    }
    printf("共删除了 %d 个节点\n", removed_count);
    print_list_info(&students);

    printf("\n=== 删除后的链表 ===\n");
    vox_list_for_each_entry(pos, &students, student_t, list_node) {
        print_student(pos);
    }

    printf("\n=== 测试移动节点（vox_list_move_after） ===\n");
    // 创建第二个链表
    vox_list_t students2;
    vox_list_init(&students2);
    
    // 添加一些新学生到第二个链表
    for (int i = 10; i < 13; i++) {
        student_t *s = (student_t*)vox_mpool_alloc(pool, sizeof(student_t));
        if (s) {
            s->id = i;
            snprintf(s->name, sizeof(s->name), "学生%d", i);
            vox_list_push_back(&students2, &s->list_node);
        }
    }
    
    printf("第二个链表内容:\n");
    vox_list_for_each_entry(pos, &students2, student_t, list_node) {
        print_student(pos);
    }
    
    // 将第二个链表的第一个节点移动到第一个链表的末尾
    vox_list_node_t *node_to_move = vox_list_first(&students2);
    if (node_to_move) {
        vox_list_node_t *target = vox_list_last(&students);
        if (target) {
            vox_list_move_after(&students2, &students, target, node_to_move);
            printf("移动节点后，第一个链表:\n");
            vox_list_for_each_entry(pos, &students, student_t, list_node) {
                print_student(pos);
            }
            printf("第二个链表:\n");
            vox_list_for_each_entry(pos, &students2, student_t, list_node) {
                print_student(pos);
            }
        }
    }

    printf("\n=== 测试拼接链表（vox_list_splice） ===\n");
    printf("拼接前，第一个链表大小: %zu\n", vox_list_size(&students));
    printf("拼接前，第二个链表大小: %zu\n", vox_list_size(&students2));
    
    vox_list_splice(&students, &students2);
    
    printf("拼接后，第一个链表:\n");
    vox_list_for_each_entry(pos, &students, student_t, list_node) {
        print_student(pos);
    }
    printf("拼接后，第一个链表大小: %zu\n", vox_list_size(&students));
    printf("拼接后，第二个链表大小: %zu（应该为空）\n", vox_list_size(&students2));
    printf("第二个链表是否为空: %s\n", vox_list_empty(&students2) ? "是" : "否");

    printf("\n=== 测试清空链表（vox_list_clear） ===\n");
    printf("清空前大小: %zu\n", vox_list_size(&students));
    vox_list_clear(&students);
    printf("清空后大小: %zu\n", vox_list_size(&students));
    printf("是否为空: %s\n", vox_list_empty(&students) ? "是" : "否");

    printf("\n=== 测试空链表操作 ===\n");
    vox_list_node_t *empty_first = vox_list_first(&students);
    vox_list_node_t *empty_last = vox_list_last(&students);
    vox_list_node_t *empty_pop = vox_list_pop_front(&students);
    printf("空链表第一个节点: %s\n", empty_first ? "非NULL" : "NULL");
    printf("空链表最后一个节点: %s\n", empty_last ? "非NULL" : "NULL");
    printf("空链表pop_front: %s\n", empty_pop ? "非NULL" : "NULL");

    printf("\n=== 清理资源 ===\n");
    vox_mpool_destroy(pool);
    
    printf("\n所有测试完成！\n");
    return 0;
}
