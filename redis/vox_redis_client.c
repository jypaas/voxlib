/*
 * vox_redis_client.c - 异步 Redis 客户端实现
 */

#include "vox_redis_client.h"
#include "../vox_handle.h"
#include "../vox_list.h"
#include "../vox_os.h"
#include "../vox_log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* 定义 CONTAINING_RECORD 宏（如果未定义） */
#ifndef VOX_CONTAINING_RECORD
#define VOX_CONTAINING_RECORD(address, type, field) \
    vox_container_of(address, type, field)
#endif

/* ===== 内部结构 ===== */

/* 注意：vox_redis_command_t 已在 vox_redis_client.h 中前向声明 */
struct vox_redis_command {
    vox_list_node_t node;           /* 链表节点 */
    vox_string_t* command_str;       /* 序列化后的命令字符串 */
    vox_redis_response_cb cb;       /* 响应回调 */
    vox_redis_error_cb error_cb;     /* 错误回调 */
    void* user_data;                 /* 用户数据 */
    bool completed;                  /* 是否已完成 */
};

struct vox_redis_client {
    vox_loop_t* loop;
    vox_mpool_t* mpool;
    
    /* 连接状态 */
    vox_tcp_t* tcp;
    vox_dns_getaddrinfo_t* dns_req;
    bool connected;
    bool connecting;
    char* host;
    uint16_t port;
    vox_redis_connect_cb connect_cb;
    void* connect_user_data;
    
    /* 解析器 */
    vox_redis_parser_t* parser;
    
    /* 命令队列 */
    vox_list_t command_queue;        /* 待发送的命令队列 */
    vox_redis_command_t* current_cmd; /* 当前正在处理的命令 */
    
    /* 响应构建 */
    vox_redis_response_t* current_response; /* 当前正在构建的响应 */
    vox_redis_response_t* response_stack[64];   /* 固定大小的响应栈（嵌套数组） */
    size_t response_stack_size;              /* 响应栈大小 */

    /* Bulk string 解析状态：避免二次拷贝 */
    char* bulk_buf;
    size_t bulk_expected;
    size_t bulk_off;
};

/* ===== 前向声明 ===== */

static void tcp_connect_cb(vox_tcp_t* tcp, int status, void* user_data);
static void tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data);
static void tcp_write_cb(vox_tcp_t* tcp, int status, void* user_data);
static void dns_cb(vox_dns_getaddrinfo_t* dns, int status, const vox_dns_addrinfo_t* addrinfo, void* user_data);

/* ===== RESP 解析器回调 ===== */

static int on_simple_string(void* p, const char* data, size_t len);
static int on_error(void* p, const char* data, size_t len);
static int on_integer(void* p, int64_t value);
static int on_bulk_string_start(void* p, int64_t len);
static int on_bulk_string_data(void* p, const char* data, size_t len);
static int on_bulk_string_complete(void* p);
static int on_array_start(void* p, int64_t count);
static int on_array_element_start(void* p, size_t index);
static int on_array_element_complete(void* p, size_t index);
static int on_array_complete(void* p);
static int on_complete(void* p);
static int on_parse_error(void* p, const char* message);

/* ===== 辅助函数 ===== */

/* 递归释放响应数据 */
static void free_response_recursive(vox_mpool_t* mpool, vox_redis_response_t* response) {
    if (!response || !mpool) return;
    
    switch (response->type) {
        case VOX_REDIS_RESPONSE_SIMPLE_STRING:
            if (response->u.simple_string.data) {
                vox_mpool_free(mpool, (void*)response->u.simple_string.data);
            }
            break;
        case VOX_REDIS_RESPONSE_ERROR:
            if (response->u.error.message) {
                vox_mpool_free(mpool, (void*)response->u.error.message);
            }
            break;
        case VOX_REDIS_RESPONSE_BULK_STRING:
            if (response->u.bulk_string.data) {
                vox_mpool_free(mpool, (void*)response->u.bulk_string.data);
            }
            break;
        case VOX_REDIS_RESPONSE_ARRAY:
            if (response->u.array.elements) {
                /* 递归释放数组元素 */
                for (size_t i = 0; i < response->u.array.count; i++) {
                    free_response_recursive(mpool, &response->u.array.elements[i]);
                }
                vox_mpool_free(mpool, response->u.array.elements);
            }
            break;
        default:
            break;
    }
}

/* 递归复制响应数据 */
static int copy_response_recursive(vox_mpool_t* mpool,
                                   const vox_redis_response_t* src,
                                   vox_redis_response_t* dst) {
    if (!src || !dst || !mpool) return -1;
    
    dst->type = src->type;
    
    switch (src->type) {
        case VOX_REDIS_RESPONSE_SIMPLE_STRING:
            if (src->u.simple_string.data) {
                char* data = (char*)vox_mpool_alloc(mpool, src->u.simple_string.len + 1);
                if (!data) return -1;
                memcpy(data, src->u.simple_string.data, src->u.simple_string.len);
                data[src->u.simple_string.len] = '\0';
                dst->u.simple_string.data = data;
                dst->u.simple_string.len = src->u.simple_string.len;
            }
            break;
        case VOX_REDIS_RESPONSE_ERROR:
            if (src->u.error.message) {
                char* msg = (char*)vox_mpool_alloc(mpool, src->u.error.len + 1);
                if (!msg) return -1;
                memcpy(msg, src->u.error.message, src->u.error.len);
                msg[src->u.error.len] = '\0';
                dst->u.error.message = msg;
                dst->u.error.len = src->u.error.len;
            }
            break;
        case VOX_REDIS_RESPONSE_INTEGER:
            dst->u.integer = src->u.integer;
            break;
        case VOX_REDIS_RESPONSE_BULK_STRING:
            dst->u.bulk_string.is_null = src->u.bulk_string.is_null;
            if (src->u.bulk_string.data && !src->u.bulk_string.is_null) {
                char* data = (char*)vox_mpool_alloc(mpool, src->u.bulk_string.len + 1);
                if (!data) return -1;
                memcpy(data, src->u.bulk_string.data, src->u.bulk_string.len);
                data[src->u.bulk_string.len] = '\0';
                dst->u.bulk_string.data = data;
                dst->u.bulk_string.len = src->u.bulk_string.len;
            }
            break;
        case VOX_REDIS_RESPONSE_ARRAY:
            dst->u.array.count = src->u.array.count;
            if (src->u.array.count > 0 && src->u.array.elements) {
                dst->u.array.elements = (vox_redis_response_t*)vox_mpool_alloc(
                    mpool, sizeof(vox_redis_response_t) * src->u.array.count);
                if (!dst->u.array.elements) return -1;
                memset(dst->u.array.elements, 0, sizeof(vox_redis_response_t) * src->u.array.count);
                for (size_t i = 0; i < src->u.array.count; i++) {
                    if (copy_response_recursive(mpool, &src->u.array.elements[i], 
                                               &dst->u.array.elements[i]) != 0) {
                        return -1;
                    }
                }
            }
            break;
        default:
            break;
    }
    
    return 0;
}

static void client_fail(vox_redis_client_t* client, const char* msg) {
    if (!client) return;
    
    if (client->current_cmd) {
        if (client->current_cmd->error_cb) {
            client->current_cmd->error_cb(client, msg, client->current_cmd->user_data);
        }
        /* 清理当前命令 */
        if (client->current_cmd->command_str) {
            vox_string_destroy(client->current_cmd->command_str);
        }
        vox_mpool_free(client->mpool, client->current_cmd);
        client->current_cmd = NULL;
    }
    
    /* 清理队列中的命令 */
    vox_list_node_t* node;
    while ((node = vox_list_pop_front(&client->command_queue)) != NULL) {
        vox_redis_command_t* cmd = VOX_CONTAINING_RECORD(node, vox_redis_command_t, node);
        if (cmd->error_cb) {
            cmd->error_cb(client, msg, cmd->user_data);
        }
        if (cmd->command_str) {
            vox_string_destroy(cmd->command_str);
        }
        vox_mpool_free(client->mpool, cmd);
    }
}

static int build_command(vox_mpool_t* mpool, vox_string_t* out, const char* cmd, va_list args) {
    if (!mpool || !out || !cmd) return -1;
    
    vox_string_clear(out);
    
    /* 计算参数数量（直到遇到NULL） */
    va_list args_copy;
    va_copy(args_copy, args);
    int arg_count = 0;
    const char* arg;
    while ((arg = va_arg(args_copy, const char*)) != NULL) {
        arg_count++;
    }
    va_end(args_copy);
    
    /* 总元素数 = 命令名(1) + 参数数量 */
    int total_count = 1 + arg_count;
    
    /* 构建数组头部 */
    if (vox_string_append_format(out, "*%d\r\n", total_count) < 0) {
        return -1;
    }
    
    /* 添加命令名 */
    size_t cmd_len = strlen(cmd);
    if (vox_string_append_format(out, "$%zu\r\n%s\r\n", cmd_len, cmd) < 0) {
        return -1;
    }
    
    /* 添加参数 */
    va_list args_copy2;
    va_copy(args_copy2, args);
    while ((arg = va_arg(args_copy2, const char*)) != NULL) {
        size_t arg_len = strlen(arg);
        if (vox_string_append_format(out, "$%zu\r\n%s\r\n", arg_len, arg) < 0) {
            va_end(args_copy2);
            return -1;
        }
    }
    va_end(args_copy2);
    
    return 0;
}

static int build_command_from_argv(vox_mpool_t* mpool, vox_string_t* out, 
                                   int argc, const char** argv) {
    if (!mpool || !out || argc <= 0 || !argv) return -1;
    
    vox_string_clear(out);
    
    /* 构建数组头部 */
    if (vox_string_append_format(out, "*%d\r\n", argc) < 0) {
        return -1;
    }
    
    /* 添加所有参数 */
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) return -1;  /* NULL 参数错误 */
        size_t arg_len = strlen(argv[i]);
        if (vox_string_append_format(out, "$%zu\r\n%s\r\n", arg_len, argv[i]) < 0) {
            return -1;
        }
    }
    
    return 0;
}

static void send_next_command(vox_redis_client_t* client) {
    if (!client || !client->connected || client->current_cmd) return;
    
    vox_list_node_t* node = vox_list_pop_front(&client->command_queue);
    if (!node) return;
    
    vox_redis_command_t* cmd = VOX_CONTAINING_RECORD(node, vox_redis_command_t, node);
    client->current_cmd = cmd;
    
    const void* buf = vox_string_data(cmd->command_str);
    size_t len = vox_string_length(cmd->command_str);
    
    if (vox_tcp_write(client->tcp, buf, len, tcp_write_cb) != 0) {
        client_fail(client, "tcp write failed");
    }
}

/* ===== RESP 解析器回调实现 ===== */

static int on_simple_string(void* p, const char* data, size_t len) {
    vox_redis_parser_t* parser = (vox_redis_parser_t*)p;
    vox_redis_client_t* client = (vox_redis_client_t*)vox_redis_parser_get_user_data(parser);
    if (!client || !client->current_cmd) return 0;
    
    /* 创建响应（如果不存在） */
    if (!client->current_response) {
        client->current_response = (vox_redis_response_t*)vox_mpool_alloc(client->mpool, sizeof(vox_redis_response_t));
        if (!client->current_response) return -1;
        memset(client->current_response, 0, sizeof(vox_redis_response_t));
    }
    
    client->current_response->type = VOX_REDIS_RESPONSE_SIMPLE_STRING;
    /* 数据在回调期间有效，需要复制 */
    char* data_copy = (char*)vox_mpool_alloc(client->mpool, len + 1);
    if (!data_copy) return -1;
    memcpy(data_copy, data, len);
    data_copy[len] = '\0';
    client->current_response->u.simple_string.data = data_copy;
    client->current_response->u.simple_string.len = len;
    
    return 0;
}

static int on_error(void* p, const char* data, size_t len) {
    vox_redis_parser_t* parser = (vox_redis_parser_t*)p;
    vox_redis_client_t* client = (vox_redis_client_t*)vox_redis_parser_get_user_data(parser);
    if (!client || !client->current_cmd) return 0;
    
    if (!client->current_response) {
        client->current_response = (vox_redis_response_t*)vox_mpool_alloc(client->mpool, sizeof(vox_redis_response_t));
        if (!client->current_response) return -1;
        memset(client->current_response, 0, sizeof(vox_redis_response_t));
    }
    
    client->current_response->type = VOX_REDIS_RESPONSE_ERROR;
    /* 数据在回调期间有效，需要复制 */
    char* msg_copy = (char*)vox_mpool_alloc(client->mpool, len + 1);
    if (!msg_copy) return -1;
    memcpy(msg_copy, data, len);
    msg_copy[len] = '\0';
    client->current_response->u.error.message = msg_copy;
    client->current_response->u.error.len = len;
    
    return 0;
}

static int on_integer(void* p, int64_t value) {
    vox_redis_parser_t* parser = (vox_redis_parser_t*)p;
    vox_redis_client_t* client = (vox_redis_client_t*)vox_redis_parser_get_user_data(parser);
    if (!client || !client->current_cmd) return 0;
    
    if (!client->current_response) {
        client->current_response = (vox_redis_response_t*)vox_mpool_alloc(client->mpool, sizeof(vox_redis_response_t));
        if (!client->current_response) return -1;
        memset(client->current_response, 0, sizeof(vox_redis_response_t));
    }
    
    client->current_response->type = VOX_REDIS_RESPONSE_INTEGER;
    client->current_response->u.integer = value;
    
    return 0;
}

static int on_bulk_string_start(void* p, int64_t len) {
    vox_redis_parser_t* parser = (vox_redis_parser_t*)p;
    vox_redis_client_t* client = (vox_redis_client_t*)vox_redis_parser_get_user_data(parser);
    if (!client || !client->current_cmd) return 0;
    
    if (!client->current_response) {
        client->current_response = (vox_redis_response_t*)vox_mpool_alloc(client->mpool, sizeof(vox_redis_response_t));
        if (!client->current_response) return -1;
        memset(client->current_response, 0, sizeof(vox_redis_response_t));
    }
    
    client->current_response->type = VOX_REDIS_RESPONSE_BULK_STRING;
    client->current_response->u.bulk_string.is_null = (len == -1);
    client->current_response->u.bulk_string.len = (len == -1) ? 0 : (size_t)len;
    client->current_response->u.bulk_string.data = NULL;

    /* 重置 bulk 状态 */
    client->bulk_buf = NULL;
    client->bulk_expected = 0;
    client->bulk_off = 0;

    if (len == -1) {
        return 0;
    }

    /* 为 bulk string 一次性分配最终缓冲区（包含 '\0'） */
    size_t n = (size_t)len;
    client->bulk_buf = (char*)vox_mpool_alloc(client->mpool, n + 1);
    if (!client->bulk_buf) return -1;
    client->bulk_expected = n;
    client->bulk_off = 0;
    client->bulk_buf[0] = '\0';
    client->current_response->u.bulk_string.data = client->bulk_buf;
    
    return 0;
}

static int on_bulk_string_data(void* p, const char* data, size_t len) {
    vox_redis_parser_t* parser = (vox_redis_parser_t*)p;
    vox_redis_client_t* client = (vox_redis_client_t*)vox_redis_parser_get_user_data(parser);
    if (!client || !client->current_cmd || !client->current_response) return 0;
    
    if (!client->bulk_buf || client->bulk_expected == 0 || len == 0) return 0;

    if (client->bulk_off + len > client->bulk_expected) {
        /* 解析器输入与声明长度不一致 */
        return -1;
    }
    memcpy(client->bulk_buf + client->bulk_off, data, len);
    client->bulk_off += len;
    
    return 0;
}

static int on_bulk_string_complete(void* p) {
    vox_redis_parser_t* parser = (vox_redis_parser_t*)p;
    vox_redis_client_t* client = (vox_redis_client_t*)vox_redis_parser_get_user_data(parser);
    if (!client || !client->current_response) return 0;
    
    if (client->current_response->type != VOX_REDIS_RESPONSE_BULK_STRING) return 0;
    if (client->current_response->u.bulk_string.is_null) return 0;

    /* 写入 '\0' 并校验长度 */
    if (client->bulk_buf) {
        if (client->bulk_off != client->bulk_expected) {
            return -1;
        }
        client->bulk_buf[client->bulk_expected] = '\0';
        client->current_response->u.bulk_string.data = client->bulk_buf;
        client->current_response->u.bulk_string.len = client->bulk_expected;
    } else {
        client->current_response->u.bulk_string.data = NULL;
        client->current_response->u.bulk_string.len = 0;
    }

    /* 清理 bulk 状态（缓冲区由 response 生命周期管理） */
    client->bulk_buf = NULL;
    client->bulk_expected = 0;
    client->bulk_off = 0;
    
    return 0;
}

static int on_array_start(void* p, int64_t count) {
    vox_redis_parser_t* parser = (vox_redis_parser_t*)p;
    vox_redis_client_t* client = (vox_redis_client_t*)vox_redis_parser_get_user_data(parser);
    if (!client || !client->current_cmd) return 0;
    
    /* 为数组创建新的响应对象 */
    vox_redis_response_t* response = (vox_redis_response_t*)vox_mpool_alloc(client->mpool, sizeof(vox_redis_response_t));
    if (!response) return -1;
    memset(response, 0, sizeof(vox_redis_response_t));
    
    response->type = VOX_REDIS_RESPONSE_ARRAY;
    response->u.array.count = (count == -1) ? 0 : (size_t)count;
    response->u.array.elements = NULL;
    
    if (count > 0) {
        response->u.array.elements = (vox_redis_response_t*)vox_mpool_alloc(
            client->mpool, sizeof(vox_redis_response_t) * (size_t)count);
        if (!response->u.array.elements) {
            vox_mpool_free(client->mpool, response);
            return -1;
        }
        memset(response->u.array.elements, 0, sizeof(vox_redis_response_t) * (size_t)count);
    }
    
    /* 将数组推入响应栈 */
    if (client->response_stack_size >= 64) {
        /* 栈溢出 - 嵌套深度超过限制 */
        if (response->u.array.elements) {
            vox_mpool_free(client->mpool, response->u.array.elements);
        }
        vox_mpool_free(client->mpool, response);
        return -1;
    }
    client->response_stack[client->response_stack_size++] = response;
    
    /* 如果这是顶层数组，设置为current_response */
    if (client->response_stack_size == 1) {
        client->current_response = response;
    }
    
    return 0;
}

static int on_array_element_start(void* p, size_t index) {
    /* 数组元素开始，当前响应已经是数组，不需要特殊处理 */
    (void)p;
    (void)index;
    return 0;
}

static int on_array_element_complete(void* p, size_t index) {
    vox_redis_parser_t* parser = (vox_redis_parser_t*)p;
    vox_redis_client_t* client = (vox_redis_client_t*)vox_redis_parser_get_user_data(parser);
    if (!client || !client->current_cmd) return 0;
    
    /* 如果当前响应存在且响应栈不为空，将当前响应添加到数组元素中 */
    if (client->current_response && client->response_stack_size > 0) {
        vox_redis_response_t* array_resp = client->response_stack[client->response_stack_size - 1];
        if (array_resp && array_resp->type == VOX_REDIS_RESPONSE_ARRAY && 
            index < array_resp->u.array.count) {
            /* 深度复制当前响应到数组元素（避免浅拷贝问题） */
            if (copy_response_recursive(client->mpool, client->current_response, 
                                       &array_resp->u.array.elements[index]) != 0) {
                return -1;
            }
            /* 释放原始当前响应（避免内存泄漏） */
            free_response_recursive(client->mpool, client->current_response);
            vox_mpool_free(client->mpool, client->current_response);
            client->current_response = NULL;
        }
    }
    
    return 0;
}

static int on_array_complete(void* p) {
    vox_redis_parser_t* parser = (vox_redis_parser_t*)p;
    vox_redis_client_t* client = (vox_redis_client_t*)vox_redis_parser_get_user_data(parser);
    if (!client) return 0;
    
    /* 弹出响应栈 */
    if (client->response_stack_size > 0) {
        /* 保存当前数组响应 */
        vox_redis_response_t* array_resp = client->response_stack[client->response_stack_size - 1];
        client->response_stack_size--;
        
        /* 如果栈为空，数组响应成为当前响应 */
        if (client->response_stack_size == 0) {
            client->current_response = array_resp;
        }
    }
    
    return 0;
}

static int on_complete(void* p) {
    vox_redis_parser_t* parser = (vox_redis_parser_t*)p;
    vox_redis_client_t* client = (vox_redis_client_t*)vox_redis_parser_get_user_data(parser);
    if (!client || !client->current_cmd) return 0;
    
    vox_redis_command_t* cmd = client->current_cmd;
    
    if (cmd->cb && client->current_response) {
        cmd->cb(client, client->current_response, cmd->user_data);
    }
    
    /* 释放响应数据 */
    if (client->current_response) {
        free_response_recursive(client->mpool, client->current_response);
        vox_mpool_free(client->mpool, client->current_response);
    }
    
    /* 清理当前命令 */
    if (cmd->command_str) {
        vox_string_destroy(cmd->command_str);
    }
    vox_mpool_free(client->mpool, cmd);
    client->current_cmd = NULL;
    client->current_response = NULL;
    
    /* 清理响应栈 */
    client->response_stack_size = 0;
    
    /* 重置解析器 */
    vox_redis_parser_reset(client->parser);
    
    /* 发送下一个命令 */
    send_next_command(client);
    
    return 0;
}

static int on_parse_error(void* p, const char* message) {
    vox_redis_parser_t* parser = (vox_redis_parser_t*)p;
    vox_redis_client_t* client = (vox_redis_client_t*)vox_redis_parser_get_user_data(parser);
    if (!client) return -1;
    
    client_fail(client, message);
    return -1;
}

/* ===== TCP 回调 ===== */

static void tcp_connect_cb(vox_tcp_t* tcp, int status, void* user_data) {
    (void)tcp;
    vox_redis_client_t* client = (vox_redis_client_t*)user_data;
    if (!client) return;
    
    VOX_LOG_DEBUG("[redis] tcp_connect_cb called, status=%d", status);
    
    client->connecting = false;
    
    if (status != 0) {
        VOX_LOG_ERROR("[redis] TCP connect failed: status=%d", status);
        client->connected = false;
        if (client->connect_cb) {
            vox_redis_connect_cb saved_cb = client->connect_cb;
            void* saved_ud = client->connect_user_data;
            client->connect_cb = NULL;
            client->connect_user_data = NULL;
            saved_cb(client, status, saved_ud);
        }
        return;
    }
    
    VOX_LOG_DEBUG("[redis] TCP connected successfully");
    client->connected = true;
    
    /* 启用 TCP keepalive 以防止长时间空闲连接被中间设备关闭 */
    vox_tcp_keepalive(client->tcp, true);
    
    /* 启动读取 */
    if (vox_tcp_read_start(client->tcp, NULL, tcp_read_cb) != 0) {
        VOX_LOG_ERROR("[redis] vox_tcp_read_start failed");
        client->connected = false;
        if (client->connect_cb) {
            vox_redis_connect_cb saved_cb = client->connect_cb;
            void* saved_ud = client->connect_user_data;
            client->connect_cb = NULL;
            client->connect_user_data = NULL;
            saved_cb(client, -1, saved_ud);
        }
        return;
    }
    
    VOX_LOG_DEBUG("[redis] calling connect callback");
    if (client->connect_cb) {
        vox_redis_connect_cb saved_cb = client->connect_cb;
        void* saved_ud = client->connect_user_data;
        client->connect_cb = NULL;
        client->connect_user_data = NULL;
        saved_cb(client, 0, saved_ud);
    }
    
    /* 发送队列中的命令 */
    send_next_command(client);
}

static void tcp_write_cb(vox_tcp_t* tcp, int status, void* user_data) {
    (void)tcp;
    vox_redis_client_t* client = (vox_redis_client_t*)user_data;
    if (!client) return;
    
    if (status != 0) {
        client_fail(client, "tcp write failed");
    }
}

static void tcp_read_cb(vox_tcp_t* tcp, ssize_t nread, const void* buf, void* user_data) {
    (void)tcp;
    vox_redis_client_t* client = (vox_redis_client_t*)user_data;
    if (!client) return;
    
    if (nread < 0) {
        client_fail(client, "tcp read error");
        return;
    }
    
    if (nread == 0) {
        /* EOF */
        client->connected = false;
        client_fail(client, "connection closed");
        return;
    }
    
    /*  feed 解析器 */
    size_t off = 0;
    while (off < (size_t)nread) {
        ssize_t n = vox_redis_parser_execute(client->parser, 
                                            (const char*)buf + off, 
                                            (size_t)nread - off);
        if (n < 0) {
            client_fail(client, vox_redis_parser_get_error(client->parser));
            return;
        }
        if (n == 0) break;
        off += (size_t)n;
        if (vox_redis_parser_is_complete(client->parser)) {
            break;
        }
    }
}

static void dns_cb(vox_dns_getaddrinfo_t* dns, int status, const vox_dns_addrinfo_t* addrinfo, void* user_data) {
    (void)dns;
    vox_redis_client_t* client = (vox_redis_client_t*)user_data;
    if (!client) return;
    
    VOX_LOG_DEBUG("[redis] dns_cb called, status=%d", status);
    
    /* 清除 DNS 请求引用（DNS 请求会在回调完成后自动清理） */
    client->dns_req = NULL;
    
    if (status != 0 || !addrinfo || addrinfo->count == 0) {
        VOX_LOG_ERROR("[redis] DNS resolution failed: status=%d, addrinfo=%p, count=%zu", 
                     status, addrinfo, addrinfo ? addrinfo->count : 0);
        client->connecting = false;
        if (client->connect_cb) {
            VOX_LOG_DEBUG("[redis] calling connect_cb with error status -1");
            client->connect_cb(client, -1, client->connect_user_data);
        } else {
            VOX_LOG_ERROR("[redis] DNS failed but no connect_cb set!");
        }
        return;
    }
    
    VOX_LOG_DEBUG("[redis] DNS resolution succeeded, count=%zu", addrinfo->count);
    
    /* 优先选择 IPv4（Windows 上 localhost 常优先解析到 ::1，若服务仅监听 127.0.0.1 会连接失败） */
    vox_socket_addr_t addr = addrinfo->addrs[0];
    for (size_t i = 0; i < addrinfo->count; i++) {
        if (addrinfo->addrs[i].family == VOX_AF_INET) {
            addr = addrinfo->addrs[i];
            break;
        }
    }
    
    VOX_LOG_DEBUG("[redis] connecting to resolved address");
    if (vox_tcp_connect(client->tcp, &addr, tcp_connect_cb) != 0) {
        VOX_LOG_ERROR("[redis] vox_tcp_connect failed");
        client->connecting = false;
        if (client->connect_cb) {
            client->connect_cb(client, -1, client->connect_user_data);
        }
    }
}

/* ===== 公共接口实现 ===== */

vox_redis_client_t* vox_redis_client_create(vox_loop_t* loop) {
    if (!loop) return NULL;
    
    vox_mpool_t* mpool = vox_loop_get_mpool(loop);
    vox_redis_client_t* client = (vox_redis_client_t*)vox_mpool_alloc(mpool, sizeof(vox_redis_client_t));
    if (!client) return NULL;
    
    memset(client, 0, sizeof(vox_redis_client_t));
    client->loop = loop;
    client->mpool = mpool;
    
    vox_list_init(&client->command_queue);
    
    /* 初始化响应栈（固定大小） */
    client->response_stack_size = 0;
    
    /* 创建TCP句柄 */
    client->tcp = vox_tcp_create(loop);
    if (!client->tcp) {
        vox_mpool_free(mpool, client);
        return NULL;
    }
    
    if (vox_tcp_init(client->tcp, loop) != 0) {
        vox_tcp_destroy(client->tcp);
        vox_mpool_free(mpool, client);
        return NULL;
    }
    
    /* 设置 TCP 句柄的 user_data，以便回调函数能获取到 client */
    vox_handle_set_data((vox_handle_t*)client->tcp, client);
    
    /* 创建RESP解析器 */
    vox_redis_parser_config_t parser_config = {0};
    vox_redis_parser_callbacks_t parser_callbacks = {0};
    parser_callbacks.on_simple_string = on_simple_string;
    parser_callbacks.on_error = on_error;
    parser_callbacks.on_integer = on_integer;
    parser_callbacks.on_bulk_string_start = on_bulk_string_start;
    parser_callbacks.on_bulk_string_data = on_bulk_string_data;
    parser_callbacks.on_bulk_string_complete = on_bulk_string_complete;
    parser_callbacks.on_array_start = on_array_start;
    parser_callbacks.on_array_element_start = on_array_element_start;
    parser_callbacks.on_array_element_complete = on_array_element_complete;
    parser_callbacks.on_array_complete = on_array_complete;
    parser_callbacks.on_complete = on_complete;
    parser_callbacks.on_error_parse = on_parse_error;
    parser_callbacks.user_data = client;
    
    client->parser = vox_redis_parser_create(mpool, &parser_config, &parser_callbacks);
    if (!client->parser) {
        vox_tcp_destroy(client->tcp);
        vox_mpool_free(mpool, client);
        return NULL;
    }
    
    return client;
}

void vox_redis_client_destroy(vox_redis_client_t* client) {
    if (!client) return;
    
    vox_redis_client_disconnect(client);
    
    if (client->parser) {
        vox_redis_parser_destroy(client->parser);
    }
    
    if (client->tcp) {
        vox_tcp_destroy(client->tcp);
    }
    
    /* 清理命令队列 */
    client_fail(client, "client destroyed");
    
    if (client->host) {
        vox_mpool_free(client->mpool, client->host);
    }
    
    vox_mpool_free(client->mpool, client);
}

int vox_redis_client_connect(vox_redis_client_t* client,
                             const char* host,
                             uint16_t port,
                             vox_redis_connect_cb cb,
                             void* user_data) {
    if (!client || !host) return -1;
    
    if (client->connected || client->connecting) {
        VOX_LOG_ERROR("[redis] already connected or connecting");
        return -1;  /* 已经连接或正在连接 */
    }
    
    VOX_LOG_DEBUG("[redis] vox_redis_client_connect: host=%s, port=%u", host, port);
    
    client->connecting = true;
    client->port = port;
    client->connect_cb = cb;
    client->connect_user_data = user_data;
    
    /* 复制host字符串 */
    size_t host_len = strlen(host);
    client->host = (char*)vox_mpool_alloc(client->mpool, host_len + 1);
    if (!client->host) {
        VOX_LOG_ERROR("[redis] failed to allocate host string");
        client->connecting = false;
        return -1;
    }
    memcpy(client->host, host, host_len + 1);
    
    /* 尝试直接解析 IP 地址（避免不必要的 DNS 解析） */
    vox_socket_addr_t addr;
    if (vox_socket_parse_address(host, port, &addr) == 0) {
        /* 是有效的 IP 地址，直接连接 */
        VOX_LOG_DEBUG("[redis] host is IP address, skipping DNS resolution");
        if (vox_tcp_connect(client->tcp, &addr, tcp_connect_cb) != 0) {
            VOX_LOG_ERROR("[redis] vox_tcp_connect failed");
            client->connecting = false;
            return -1;
        }
        return 0;
    }
    
    /* 需要 DNS 解析 */
    char port_str[16];
    (void)snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    
    VOX_LOG_DEBUG("[redis] creating DNS request");
    client->dns_req = vox_dns_getaddrinfo_create(client->loop);
    if (!client->dns_req) {
        VOX_LOG_ERROR("[redis] failed to create DNS request");
        client->connecting = false;
        return -1;
    }
    
    VOX_LOG_DEBUG("[redis] starting DNS resolution: host=%s, port=%s", host, port_str);
    if (vox_dns_getaddrinfo(client->dns_req, host, port_str, 0, dns_cb, client, 5000) != 0) {
        VOX_LOG_ERROR("[redis] vox_dns_getaddrinfo failed");
        vox_dns_getaddrinfo_destroy(client->dns_req);
        client->dns_req = NULL;
        client->connecting = false;
        return -1;
    }
    
    VOX_LOG_DEBUG("[redis] DNS resolution started");
    return 0;
}

void vox_redis_client_disconnect(vox_redis_client_t* client) {
    if (!client) return;
    
    if (client->dns_req) {
        vox_dns_getaddrinfo_cancel(client->dns_req);
        vox_dns_getaddrinfo_destroy(client->dns_req);
        client->dns_req = NULL;
    }
    
    if (client->tcp && client->connected) {
        vox_handle_close((vox_handle_t*)client->tcp, NULL);
    }
    
    client->connected = false;
    client->connecting = false;
}

bool vox_redis_client_is_connected(vox_redis_client_t* client) {
    return client && client->connected;
}

int vox_redis_client_command_raw(vox_redis_client_t* client,
                                 const char* buf,
                                 size_t len,
                                 vox_redis_response_cb cb,
                                 vox_redis_error_cb error_cb,
                                 void* user_data) {
    if (!client || !buf || !cb) return -1;
    if (!client->connected) {
        if (error_cb) error_cb(client, "not connected", user_data);
        return -1;
    }
    vox_redis_command_t* cmd = (vox_redis_command_t*)vox_mpool_alloc(client->mpool, sizeof(vox_redis_command_t));
    if (!cmd) return -1;
    memset(cmd, 0, sizeof(vox_redis_command_t));
    cmd->cb = cb;
    cmd->error_cb = error_cb;
    cmd->user_data = user_data;
    cmd->command_str = vox_string_create(client->mpool);
    if (!cmd->command_str) {
        vox_mpool_free(client->mpool, cmd);
        return -1;
    }
    if (vox_string_append_data(cmd->command_str, buf, len) != 0) {
        vox_string_destroy(cmd->command_str);
        vox_mpool_free(client->mpool, cmd);
        return -1;
    }
    vox_list_push_back(&client->command_queue, &cmd->node);
    if (!client->current_cmd) send_next_command(client);
    return 0;
}

int vox_redis_client_command(vox_redis_client_t* client,
                             vox_redis_response_cb cb,
                             vox_redis_error_cb error_cb,
                             void* user_data,
                             const char* format, ...) {
    if (!client || !format || !cb) return -1;
    
    if (!client->connected) {
        if (error_cb) {
            error_cb(client, "not connected", user_data);
        }
        return -1;
    }
    
    /* 创建命令 */
    vox_redis_command_t* cmd = (vox_redis_command_t*)vox_mpool_alloc(client->mpool, sizeof(vox_redis_command_t));
    if (!cmd) return -1;
    
    memset(cmd, 0, sizeof(vox_redis_command_t));
    cmd->cb = cb;
    cmd->error_cb = error_cb;
    cmd->user_data = user_data;
    
    cmd->command_str = vox_string_create(client->mpool);
    if (!cmd->command_str) {
        vox_mpool_free(client->mpool, cmd);
        return -1;
    }
    
    /* 构建命令 */
    va_list args;
    va_start(args, format);
    if (build_command(client->mpool, cmd->command_str, format, args) != 0) {
        va_end(args);
        vox_string_destroy(cmd->command_str);
        vox_mpool_free(client->mpool, cmd);
        return -1;
    }
    va_end(args);
    
    /* 添加到队列 */
    vox_list_push_back(&client->command_queue, &cmd->node);
    
    /* 如果当前没有命令在处理，立即发送 */
    if (!client->current_cmd) {
        send_next_command(client);
    }
    
    return 0;
}

int vox_redis_client_command_va(vox_redis_client_t* client,
                                vox_redis_response_cb cb,
                                vox_redis_error_cb error_cb,
                                void* user_data,
                                const char* format,
                                va_list args) {
    if (!client || !format || !cb) return -1;
    
    if (!client->connected) {
        if (error_cb) {
            error_cb(client, "not connected", user_data);
        }
        return -1;
    }
    
    /* 创建命令 */
    vox_redis_command_t* cmd = (vox_redis_command_t*)vox_mpool_alloc(client->mpool, sizeof(vox_redis_command_t));
    if (!cmd) return -1;
    
    memset(cmd, 0, sizeof(vox_redis_command_t));
    cmd->cb = cb;
    cmd->error_cb = error_cb;
    cmd->user_data = user_data;
    
    cmd->command_str = vox_string_create(client->mpool);
    if (!cmd->command_str) {
        vox_mpool_free(client->mpool, cmd);
        return -1;
    }
    
    /* 构建命令 - 使用传入的 va_list */
    va_list args_copy;
    va_copy(args_copy, args);
    if (build_command(client->mpool, cmd->command_str, format, args_copy) != 0) {
        va_end(args_copy);
        vox_string_destroy(cmd->command_str);
        vox_mpool_free(client->mpool, cmd);
        return -1;
    }
    va_end(args_copy);
    
    /* 添加到队列 */
    vox_list_push_back(&client->command_queue, &cmd->node);
    
    /* 如果当前没有命令在处理，立即发送 */
    if (!client->current_cmd) {
        send_next_command(client);
    }
    
    return 0;
}

int vox_redis_client_commandv(vox_redis_client_t* client,
                              vox_redis_response_cb cb,
                              vox_redis_error_cb error_cb,
                              void* user_data,
                              int argc,
                              const char** argv) {
    if (!client || argc <= 0 || !argv || !cb) return -1;
    
    if (!client->connected) {
        if (error_cb) {
            error_cb(client, "not connected", user_data);
        }
        return -1;
    }
    
    /* 创建命令 */
    vox_redis_command_t* cmd = (vox_redis_command_t*)vox_mpool_alloc(client->mpool, sizeof(vox_redis_command_t));
    if (!cmd) return -1;
    
    memset(cmd, 0, sizeof(vox_redis_command_t));
    cmd->cb = cb;
    cmd->error_cb = error_cb;
    cmd->user_data = user_data;
    
    cmd->command_str = vox_string_create(client->mpool);
    if (!cmd->command_str) {
        vox_mpool_free(client->mpool, cmd);
        return -1;
    }
    
    /* 从数组构建命令 */
    if (build_command_from_argv(client->mpool, cmd->command_str, argc, argv) != 0) {
        vox_string_destroy(cmd->command_str);
        vox_mpool_free(client->mpool, cmd);
        return -1;
    }
    
    /* 添加到队列 */
    vox_list_push_back(&client->command_queue, &cmd->node);
    
    /* 如果当前没有命令在处理，立即发送 */
    if (!client->current_cmd) {
        send_next_command(client);
    }
    
    return 0;
}

/* ===== 命令封装 ===== */

int vox_redis_client_ping(vox_redis_client_t* client,
                          vox_redis_response_cb cb,
                          void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "PING", NULL);
}

int vox_redis_client_get(vox_redis_client_t* client,
                         const char* key,
                         vox_redis_response_cb cb,
                         void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "GET", key, NULL);
}

int vox_redis_client_set(vox_redis_client_t* client,
                         const char* key,
                         const char* value,
                         vox_redis_response_cb cb,
                         void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "SET", key, value, NULL);
}

int vox_redis_client_del(vox_redis_client_t* client,
                         const char* key,
                         vox_redis_response_cb cb,
                         void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "DEL", key, NULL);
}

int vox_redis_client_exists(vox_redis_client_t* client,
                            const char* key,
                            vox_redis_response_cb cb,
                            void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "EXISTS", key, NULL);
}

int vox_redis_client_incr(vox_redis_client_t* client,
                          const char* key,
                          vox_redis_response_cb cb,
                          void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "INCR", key, NULL);
}

int vox_redis_client_decr(vox_redis_client_t* client,
                          const char* key,
                          vox_redis_response_cb cb,
                          void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "DECR", key, NULL);
}

int vox_redis_client_hset(vox_redis_client_t* client,
                          const char* key,
                          const char* field,
                          const char* value,
                          vox_redis_response_cb cb,
                          void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "HSET", key, field, value, NULL);
}

int vox_redis_client_hget(vox_redis_client_t* client,
                          const char* key,
                          const char* field,
                          vox_redis_response_cb cb,
                          void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "HGET", key, field, NULL);
}

int vox_redis_client_hdel(vox_redis_client_t* client,
                          const char* key,
                          const char* field,
                          vox_redis_response_cb cb,
                          void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "HDEL", key, field, NULL);
}

int vox_redis_client_hexists(vox_redis_client_t* client,
                             const char* key,
                             const char* field,
                             vox_redis_response_cb cb,
                             void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "HEXISTS", key, field, NULL);
}

int vox_redis_client_lpush(vox_redis_client_t* client,
                          const char* key,
                          const char* value,
                          vox_redis_response_cb cb,
                          void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "LPUSH", key, value, NULL);
}

int vox_redis_client_rpush(vox_redis_client_t* client,
                          const char* key,
                          const char* value,
                          vox_redis_response_cb cb,
                          void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "RPUSH", key, value, NULL);
}

int vox_redis_client_lpop(vox_redis_client_t* client,
                         const char* key,
                         vox_redis_response_cb cb,
                         void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "LPOP", key, NULL);
}

int vox_redis_client_rpop(vox_redis_client_t* client,
                         const char* key,
                         vox_redis_response_cb cb,
                         void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "RPOP", key, NULL);
}

int vox_redis_client_llen(vox_redis_client_t* client,
                          const char* key,
                          vox_redis_response_cb cb,
                          void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "LLEN", key, NULL);
}

int vox_redis_client_sadd(vox_redis_client_t* client,
                          const char* key,
                          const char* member,
                          vox_redis_response_cb cb,
                          void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "SADD", key, member, NULL);
}

int vox_redis_client_srem(vox_redis_client_t* client,
                          const char* key,
                          const char* member,
                          vox_redis_response_cb cb,
                          void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "SREM", key, member, NULL);
}

int vox_redis_client_smembers(vox_redis_client_t* client,
                              const char* key,
                              vox_redis_response_cb cb,
                              void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "SMEMBERS", key, NULL);
}

int vox_redis_client_scard(vox_redis_client_t* client,
                           const char* key,
                           vox_redis_response_cb cb,
                           void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "SCARD", key, NULL);
}

int vox_redis_client_sismember(vox_redis_client_t* client,
                              const char* key,
                              const char* member,
                              vox_redis_response_cb cb,
                              void* user_data) {
    return vox_redis_client_command(client, cb, NULL, user_data, "SISMEMBER", key, member, NULL);
}

/* ===== 辅助函数实现 ===== */

void vox_redis_response_free(vox_mpool_t* mpool, vox_redis_response_t* response) {
    if (!response || !mpool) return;
    free_response_recursive(mpool, response);
}

int vox_redis_response_copy(vox_mpool_t* mpool, 
                            const vox_redis_response_t* src,
                            vox_redis_response_t* dst) {
    if (!mpool || !src || !dst) return -1;
    memset(dst, 0, sizeof(vox_redis_response_t));
    return copy_response_recursive(mpool, src, dst);
}
