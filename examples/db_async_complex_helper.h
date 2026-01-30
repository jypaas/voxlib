/*
 * db_async_complex_helper.h - 复杂异步操作的辅助工具
 *
 * 提供通用的异步操作管理结构，简化多个异步操作的编排
 */

#ifndef VOX_DB_ASYNC_COMPLEX_HELPER_H
#define VOX_DB_ASYNC_COMPLEX_HELPER_H

#include "../vox_loop.h"
#include "../vox_mutex.h"
#include "../db/vox_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 异步操作序列管理器 ===== */

typedef struct vox_async_sequence vox_async_sequence_t;

/* 操作步骤回调 */
typedef void (*vox_async_step_fn)(vox_async_sequence_t* seq, int step_index, void* user_data);
/* 完成回调 */
typedef void (*vox_async_complete_fn)(vox_async_sequence_t* seq, int status, void* user_data);
/* 错误回调 */
typedef void (*vox_async_error_fn)(vox_async_sequence_t* seq, int error_code, const char* error_msg, void* user_data);

struct vox_async_sequence {
    vox_loop_t* loop;
    vox_db_conn_t* db;
    
    /* 步骤配置 */
    vox_async_step_fn* step_fns;  /* 步骤函数数组 */
    size_t step_count;  /* 步骤总数 */
    int current_step;  /* 当前步骤索引 */
    
    /* 回调 */
    vox_async_complete_fn on_complete;
    vox_async_error_fn on_error;
    void* user_data;
    
    /* 状态 */
    int status;
    const char* error_msg;
    vox_mutex_t mutex;  /* 保护状态 */
};

/**
 * 创建异步操作序列
 * @param loop 事件循环
 * @param db 数据库连接
 * @param step_count 步骤数量
 * @param on_complete 完成回调
 * @param on_error 错误回调
 * @param user_data 用户数据
 * @return 序列对象，失败返回NULL
 */
vox_async_sequence_t* vox_async_sequence_create(vox_loop_t* loop,
                                                 vox_db_conn_t* db,
                                                 size_t step_count,
                                                 vox_async_complete_fn on_complete,
                                                 vox_async_error_fn on_error,
                                                 void* user_data);

/**
 * 设置步骤函数
 * @param seq 序列对象
 * @param step_index 步骤索引（0-based）
 * @param step_fn 步骤函数
 */
void vox_async_sequence_set_step(vox_async_sequence_t* seq, size_t step_index, vox_async_step_fn step_fn);

/**
 * 启动序列
 * @param seq 序列对象
 */
void vox_async_sequence_start(vox_async_sequence_t* seq);

/**
 * 进入下一步
 * @param seq 序列对象
 */
void vox_async_sequence_next(vox_async_sequence_t* seq);

/**
 * 标记当前步骤完成
 * @param seq 序列对象
 * @param status 状态（0表示成功）
 */
void vox_async_sequence_step_done(vox_async_sequence_t* seq, int status);

/**
 * 标记序列完成
 * @param seq 序列对象
 * @param status 状态（0表示成功）
 */
void vox_async_sequence_complete(vox_async_sequence_t* seq, int status);

/**
 * 标记序列错误
 * @param seq 序列对象
 * @param error_code 错误码
 * @param error_msg 错误消息
 */
void vox_async_sequence_error(vox_async_sequence_t* seq, int error_code, const char* error_msg);

/**
 * 销毁序列
 * @param seq 序列对象
 */
void vox_async_sequence_destroy(vox_async_sequence_t* seq);

/* ===== 并行操作管理器 ===== */

typedef struct vox_async_parallel vox_async_parallel_t;

/* 操作完成回调 */
typedef void (*vox_async_parallel_done_fn)(vox_async_parallel_t* parallel, int success_count, int error_count, void* user_data);

struct vox_async_parallel {
    vox_loop_t* loop;
    vox_db_conn_t* db;
    
    /* 状态 */
    int pending_count;  /* 待完成的操作数 */
    int completed_count;  /* 已完成的操作数 */
    int success_count;  /* 成功的操作数 */
    int error_count;  /* 失败的操作数 */
    
    /* 回调 */
    vox_async_parallel_done_fn on_done;
    void* user_data;
    
    vox_mutex_t mutex;  /* 保护计数器 */
};

/**
 * 创建并行操作管理器
 * @param loop 事件循环
 * @param db 数据库连接
 * @param on_done 完成回调
 * @param user_data 用户数据
 * @return 管理器对象，失败返回NULL
 */
vox_async_parallel_t* vox_async_parallel_create(vox_loop_t* loop,
                                                vox_db_conn_t* db,
                                                vox_async_parallel_done_fn on_done,
                                                void* user_data);

/**
 * 添加一个操作
 * @param parallel 管理器对象
 * @param sql SQL语句
 * @param params 参数
 * @param nparams 参数数量
 * @param exec_cb 执行回调（用于exec操作）
 * @param query_row_cb 行回调（用于query操作，可为NULL）
 * @param query_done_cb 查询完成回调（用于query操作，可为NULL）
 */
void vox_async_parallel_add_exec(vox_async_parallel_t* parallel,
                                 const char* sql,
                                 const vox_db_value_t* params,
                                 size_t nparams,
                                 vox_db_exec_cb exec_cb);

void vox_async_parallel_add_query(vox_async_parallel_t* parallel,
                                 const char* sql,
                                 const vox_db_value_t* params,
                                 size_t nparams,
                                 vox_db_row_cb row_cb,
                                 vox_db_done_cb done_cb);

/**
 * 开始执行所有操作
 * @param parallel 管理器对象
 */
void vox_async_parallel_start(vox_async_parallel_t* parallel);

/**
 * 标记一个操作完成
 * @param parallel 管理器对象
 * @param success 是否成功
 */
void vox_async_parallel_op_done(vox_async_parallel_t* parallel, bool success);

/**
 * 销毁管理器
 * @param parallel 管理器对象
 */
void vox_async_parallel_destroy(vox_async_parallel_t* parallel);

#ifdef __cplusplus
}
#endif

#endif /* VOX_DB_ASYNC_COMPLEX_HELPER_H */
