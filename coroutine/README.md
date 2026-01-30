# VoxLib 协程模块

协程模块提供 **async/await 风格** 的 C 协程 API，与事件循环（`vox_loop`）集成，用于将回调式异步操作写成线性流程，减少回调嵌套。

## 特性

- **栈式协程**：每个协程有独立栈（默认 64KB，可配置）
- **Promise 机制**：通过 `vox_coroutine_await()` 挂起直到异步操作完成
- **跨平台上下文切换**：
  - **Windows**：Fiber API
  - **Linux/macOS x86_64**：汇编实现
  - **Linux/macOS ARM64**：汇编实现
  - **其他 Unix**：ucontext 回退
- **协程池**：复用协程槽与栈，降低高并发下的分配开销
- **调度器**：就绪队列调度，可控制每 tick 恢复数量
- **多种适配器**：数据库、文件系统、DNS、Redis、HTTP、WebSocket 等均有协程版 `*_await` 接口

## 模块结构

```
coroutine/
├── vox_coroutine.h/c          # 协程核心：创建、恢复、挂起、await
├── vox_coroutine_promise.h/c  # Promise：完成/等待、状态与结果
├── vox_coroutine_pool.h/c     # 协程池：预分配槽与栈、acquire/release
├── vox_coroutine_scheduler.h/c# 调度器：就绪队列、tick 执行
├── vox_coroutine_context.h    # 上下文切换抽象
├── vox_coroutine_context_win.c      # Windows Fiber
├── vox_coroutine_context_asm.c     # Unix ucontext/汇编封装
├── vox_coroutine_context_x64.S     # x86_64 汇编
├── vox_coroutine_context_arm64.S   # ARM64 汇编
├── vox_coroutine_db.h/c       # 数据库协程适配器
├── vox_coroutine_fs.h/c       # 文件系统协程适配器
├── vox_coroutine_dns.h/c      # DNS 协程适配器
├── vox_coroutine_redis.h/c    # Redis 协程适配器
├── vox_coroutine_http.h/c    # HTTP 客户端协程适配器
└── vox_coroutine_ws.h/c      # WebSocket 客户端协程适配器
```

## 快速开始

### 基本用法

1. 用 `VOX_COROUTINE_ENTRY(name, user_data_type)` 定义协程入口函数。
2. 在协程内用各模块的 `*_await` 接口执行异步操作（内部会挂起直到完成）。
3. 用 `VOX_COROUTINE_START(loop, entry, user_data)` 创建并首次恢复协程，然后运行事件循环。

```c
#include "vox_loop.h"
#include "coroutine/vox_coroutine.h"
#include "coroutine/vox_coroutine_fs.h"

VOX_COROUTINE_ENTRY(my_task, vox_loop_t*) {
    vox_loop_t* loop = (vox_loop_t*)user_data;
    void* data = NULL;
    size_t size = 0;
    if (vox_coroutine_fs_read_file_await(co, "config.json", &data, &size) != 0)
        return;
    /* 使用 data/size ... */
    vox_coroutine_fs_free_file_data(co, data);
    vox_loop_stop(loop);
}

int main(void) {
    vox_loop_t* loop = vox_loop_create();
    VOX_COROUTINE_START(loop, my_task, loop);
    vox_loop_run(loop, VOX_RUN_DEFAULT);
    vox_loop_destroy(loop);
    return 0;
}
```

### 协程状态

- `VOX_COROUTINE_READY`：已创建，未运行
- `VOX_COROUTINE_RUNNING`：正在执行
- `VOX_COROUTINE_SUSPENDED`：在 await 上挂起
- `VOX_COROUTINE_COMPLETED`：正常结束
- `VOX_COROUTINE_ERROR`：出错

可通过 `vox_coroutine_get_state(co)` 查询。

### 常用 API 摘要

| 功能           | API |
|----------------|-----|
| 创建协程       | `vox_coroutine_create()`, `vox_coroutine_create_ex()` |
| 池化创建       | `vox_coroutine_create_pooled()`, `VOX_COROUTINE_START_POOLED()` |
| 恢复/挂起      | `vox_coroutine_resume()`, `vox_coroutine_yield()` |
| 等待 Promise   | `vox_coroutine_await()`, `VOX_COROUTINE_AWAIT()` |
| Promise 创建/完成 | `vox_coroutine_promise_create()`, `vox_coroutine_promise_complete()` |
| 当前协程       | `vox_coroutine_current()` |
| 销毁           | `vox_coroutine_destroy()`, `vox_coroutine_promise_destroy()` |

## Promise 机制

异步操作在内部创建一个 Promise，在完成时调用 `vox_coroutine_promise_complete(promise, status, result)`，等待方通过 `vox_coroutine_await(co, promise)` 挂起，完成后被唤醒并得到 status/result。  
各 `*_await` 适配器已封装这一流程，一般只需在协程内调用适配器 API 并检查返回值即可。

## 协程池

高并发时可用协程池复用栈与协程槽，减少分配与碎片：

```c
vox_coroutine_pool_config_t pool_cfg;
vox_coroutine_pool_config_default(&pool_cfg);
pool_cfg.initial_count = 64;
pool_cfg.stack_size = 65536;

vox_coroutine_pool_t* pool = vox_coroutine_pool_create(loop, &pool_cfg);
vox_coroutine_t* co = vox_coroutine_create_pooled(loop, pool, my_entry, user_data);
vox_coroutine_resume(co);
// 协程结束后由框架归还到池，无需手动 release 协程对象
vox_coroutine_pool_destroy(pool);
```

也可使用宏 `VOX_COROUTINE_START_POOLED(loop, pool, entry, user_data)`。

## 调度器

需要显式“按 tick”调度协程时，可使用调度器：

- `vox_coroutine_scheduler_create(loop, config)` 创建调度器
- `vox_coroutine_schedule(sched, co)` 将协程放入就绪队列
- 在合适的时机（例如 loop 的 idle 或定时器）调用 `vox_coroutine_scheduler_tick(sched)` 执行一批就绪协程

配置项包括就绪队列容量、每 tick 最大恢复数、是否使用 MPSC 队列等。

## 协程适配器一览

| 适配器   | 头文件               | 典型接口示例 |
|----------|----------------------|--------------|
| 数据库   | `vox_coroutine_db.h` | `vox_coroutine_db_exec_await`, `vox_coroutine_db_query_await`, `vox_coroutine_db_pool_*_await` |
| 文件系统 | `vox_coroutine_fs.h` | `vox_coroutine_fs_open_await`, `vox_coroutine_fs_read_await`, `vox_coroutine_fs_read_file_await` |
| DNS      | `vox_coroutine_dns.h`| `vox_coroutine_dns_getaddrinfo_await`, `vox_coroutine_dns_getnameinfo_await` |
| Redis    | `vox_coroutine_redis.h` | `vox_coroutine_redis_connect_await`, `vox_coroutine_redis_get_await`, `vox_coroutine_redis_pool_*_await` |
| HTTP     | `vox_coroutine_http.h` | `vox_coroutine_http_get_await`, `vox_coroutine_http_post_await` |
| WebSocket| `vox_coroutine_ws.h` | `vox_coroutine_ws_connect_await`, `vox_coroutine_ws_recv_await`, `vox_coroutine_ws_send_text_await` |

具体参数与返回值以头文件注释为准；多数 `*_await` 返回 0 表示成功，非 0 表示错误。

## 配置与扩展

- **栈大小**：`vox_coroutine_create(..., stack_size)` 或 `vox_coroutine_config_t.stack_size`，默认 64KB。
- **扩展配置**：`vox_coroutine_create_ex(loop, entry, user_data, &config)`，可同时指定栈大小、是否使用池、池指针等。

## 示例程序

在仓库根目录构建后，可运行（具体名称以 CMake 配置为准）：

- **协程池与调度**：`coroutine_pool_example` — 基本协程、池化、调度器与简单性能测试
- **多客户端协程**：`coroutine_clients_example` — 文件系统、Redis、HTTP、WebSocket 的协程用法
- **数据库协程**：`db_coroutine_example` — 需启用数据库驱动，展示 DB 与连接池的 `*_await` 用法

## 注意事项

1. **栈大小**：协程栈默认 64KB，深调用或大局部变量时需适当增大。
2. **生命周期**：协程和 Promise 在未再被引用后应及时销毁；从池创建的协程在结束后由实现归还池，无需用户释放协程对象本身。
3. **线程**：协程运行在创建它的 `vox_loop` 所在线程，不要在其它线程 resume 同一协程。
4. **适配器资源**：各适配器返回的缓冲区/结果（如 `out_rows`、`out_response`、`out_data`）通常需按头文件说明释放（如 `vox_coroutine_fs_free_file_data`、`vox_coroutine_http_response_free` 等）。
5. **与回调混用**：可与现有基于回调的异步 API 混用；在回调里完成 Promise 即可在协程里 await。

## 依赖

- 核心依赖：`vox_loop`、`vox_handle`、`vox_mpool`、`vox_mutex` 等（见主库 CMake）
- 各适配器依赖对应模块（如 `vox_db`、`vox_fs`、`vox_redis_client`、`vox_http_client` 等）

构建时协程源会随主库一起编译；若关闭某适配器对应模块，可排除相应 `vox_coroutine_*.c` 或通过宏控制。
