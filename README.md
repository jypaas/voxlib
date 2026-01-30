# VoxLib

高性能、跨平台 C 工具库，提供类似 libuv 的异步 I/O 框架及丰富实用功能。外部只需包含 **`vox.h`** 并调用 **`vox_init()`** 即可使用全部 API。

## 特性

### 核心功能

- **统一入口**
  - 单头文件：`#include "vox.h"` 即可使用整个库
  - 库级 API：`vox_init()` / `vox_fini()` / `vox_version()`，其中 `vox_init()` 已包含 Socket 初始化（Windows 下 WSAStartup）

- **异步 I/O 事件循环**
  - Windows: IOCP（I/O Completion Ports）
  - Linux: epoll / io_uring（需 liburing）
  - macOS/BSD: kqueue
  - 自动选择最佳后端，回退为 select

- **网络支持**
  - TCP/UDP 异步操作
  - DNS 异步解析
  - TLS/SSL 加密通信（HTTPS、WSS、DTLS）
  - HTTP/HTTPS 服务器框架、HTTP 客户端
  - WebSocket 支持（WS 和 WSS）

- **数据库抽象层**
  - 统一异步数据库 API
  - 支持 SQLite3、DuckDB、PostgreSQL、MySQL（按需启用）
  - 连接池、行流式处理、回调/协程两种用法

- **协程系统**
  - 栈式协程（Stackful Coroutines）
  - async/await 风格 API
  - 适配器支持 DB、TCP、UDP、FS、DNS、Redis、HTTP、WebSocket 等

- **内存与数据结构**
  - 固定大小内存池（10 个大小类别：16–8192 字节），可选线程安全
  - 动态数组、哈希表、红黑树、优先队列、队列、字符串、链表

- **解析与序列化**
  - JSON / XML / TOML v1.0.0 / INI
  - 正则引擎（NFA）、HTTP 消息解析、多部分表单解析

- **其他**
  - Redis 客户端与连接池
  - 线程池、异步文件系统、加密工具、日志、跨平台线程与同步原语

## 快速开始

### 构建要求

- CMake 3.10+
- C99 编译器（GCC、Clang、MSVC）
- 可选：OpenSSL（TLS/SSL）、zlib（gzip）、liburing（Linux io_uring）、各数据库驱动

### 基本构建

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

可执行文件与库输出到项目根目录下的 **`bin/`**。

### 构建选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `VOX_BUILD_TESTS` | OFF | 构建单元测试 |
| `VOX_BUILD_EXAMPLES` | OFF | 构建示例程序 |
| `VOX_BUILD_SHARED` | OFF | 构建动态库 |
| `VOX_USE_IOURING` | ON | Linux 使用 io_uring（需 liburing） |
| `VOX_ENABLE_LOG` | ON | 启用日志 |
| `VOX_ENABLE_ASSERT` | ON | 启用断言 |
| `VOX_USE_OPENSSL` | ON | TLS 使用 OpenSSL |
| `VOX_USE_ZLIB` | ON | gzip 使用 zlib |
| `VOX_USE_SQLITE3` | ON | SQLite3 驱动 |
| `VOX_USE_DUCKDB` | OFF | DuckDB 驱动 |
| `VOX_USE_PGSQL` | OFF | PostgreSQL 驱动 |
| `VOX_USE_MYSQL` | OFF | MySQL 驱动 |

**构建类型示例：**

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake -DCMAKE_BUILD_TYPE=Release ..
```

**启用示例与测试：**

```bash
cmake -DVOX_BUILD_EXAMPLES=ON -DVOX_BUILD_TESTS=ON ..
cmake --build .
```

### Windows 上 OpenSSL（vcpkg）

```bash
# 安装 vcpkg 并安装 OpenSSL
.\vcpkg install openssl:x64-windows

# 使用 vcpkg 工具链配置
cmake -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake -DVOX_USE_OPENSSL=ON ..
```

## 使用方式

### 统一头文件与库级 API

将 VoxLib 的**项目根目录**加入包含路径，程序中只需包含 `vox.h`，并在使用网络等能力前调用 `vox_init()`，退出前配对调用 `vox_fini()`：

```c
#include "vox.h"

int main(void)
{
    vox_init();   /* 初始化库（含 Socket，可多次调用，内部引用计数） */

    printf("VoxLib %s\n", vox_version());

    /* 使用事件循环、TCP、HTTP 等任意 API... */

    vox_fini();   /* 反初始化（与 vox_init 配对） */
    return 0;
}
```

- **`void vox_init(void)`**：初始化 VoxLib（含 Socket 初始化）；可多次调用，内部引用计数。
- **`void vox_fini(void)`**：反初始化；与 `vox_init` 配对，引用计数归零时执行清理（含 Socket 清理）。
- **`const char* vox_version(void)`**：返回版本字符串，如 `"1.0.0"`。

## 主要模块示例

### 异步 I/O（TCP 服务）

```c
#include "vox.h"

void on_connection(vox_tcp_t* client, int status) {
    (void)client;
    (void)status;
    /* 处理新连接 */
}

int main(void)
{
    vox_init();

    vox_loop_t* loop = vox_loop_create();
    vox_tcp_t* server = vox_tcp_create(loop);

    vox_socket_addr_t addr;
    vox_socket_parse_address("127.0.0.1", 8080, &addr);
    vox_tcp_bind(server, &addr, 0);
    vox_tcp_listen(server, on_connection);

    vox_loop_run(loop, VOX_RUN_DEFAULT);

    vox_fini();
    return 0;
}
```

### HTTP 服务器

```c
#include "vox.h"

void on_request(vox_http_context_t* ctx) {
    vox_http_context_send_json(ctx, 200, "{\"message\":\"Hello\"}");
}

int main(void)
{
    vox_init();

    vox_loop_t* loop = vox_loop_create();
    vox_http_engine_t* engine = vox_http_engine_create(loop);

    vox_http_router_add(engine->router, "GET", "/", on_request);

    vox_http_server_t* server = vox_http_server_create(engine);
    vox_socket_addr_t addr;
    vox_socket_parse_address("0.0.0.0", 8080, &addr);
    vox_http_server_listen_tcp(server, &addr, 128);

    vox_loop_run(loop, VOX_RUN_DEFAULT);

    vox_fini();
    return 0;
}
```

### 数据库（异步 + 回调）

```c
#include "vox.h"

static void on_row(vox_db_row_t* row, void* user_data) {
    (void)user_data;
    /* 处理每一行 */
}

static void on_connect(vox_db_conn_t* conn, int status, void* user_data) {
    if (status != 0) return;
    vox_db_query_async(conn, "SELECT * FROM users", NULL, 0, on_row, NULL, user_data);
}

int main(void)
{
    vox_init();

    vox_loop_t* loop = vox_loop_create();
    vox_db_connect("sqlite3", "test.db", loop, on_connect, NULL);

    vox_loop_run(loop, VOX_RUN_DEFAULT);

    vox_fini();
    return 0;
}
```

### 协程（async/await 风格）

```c
#include "vox.h"

VOX_COROUTINE_ENTRY(my_coroutine, vox_db_conn_t* db) {
    int64_t affected = 0;
    int status = vox_coroutine_db_exec_await(co, db, "CREATE TABLE t(id INT);", NULL, 0, &affected);
    if (status != 0) return;
    /* 继续线性写异步逻辑... */
}

int main(void)
{
    vox_init();

    vox_loop_t* loop = vox_loop_create();
    /* 创建 db 连接后 */
    VOX_COROUTINE_START(loop, my_coroutine, db);
    vox_loop_run(loop, VOX_RUN_DEFAULT);

    vox_fini();
    return 0;
}
```

## 运行示例与测试

构建时需启用 `VOX_BUILD_EXAMPLES=ON` 和（若需测试）`VOX_BUILD_TESTS=ON`，可执行文件在 **`bin/`** 目录：

```bash
# 核心数据结构
./bin/mpool_example
./bin/vector_example
./bin/htable_example
./bin/string_example
./bin/rbtree_example
./bin/queue_example
./bin/list_example

# 异步 I/O
./bin/loop_example
./bin/tcp_echo_test
./bin/udp_echo_test
./bin/fs_example
./bin/dns_example

# HTTP 服务器
./bin/http_server_example
./bin/ws_echo_example
./bin/http_middleware_example

# HTTPS / TLS（需 OpenSSL）
./bin/https_server_example
./bin/tls_echo_test
./bin/dtls_echo_test

# 数据库（需启用对应驱动）
./bin/db_example
./bin/db_async_loop_example
./bin/db_pool_async_example
./bin/http_server_db_async_example

# 协程
./bin/coroutine_pool_example
./bin/coroutine_clients_example
./bin/db_coroutine_example

# 解析器
./bin/json_example
./bin/xml_example
./bin/toml_example
./bin/regex_example
./bin/ini_example

# Redis
./bin/redis_client_example
./bin/redis_pool_dynamic_example

# 其他
./bin/crypto_example
./bin/tpool_example
./bin/thread_example
./bin/mutex_example
./bin/process_example
```

**运行测试：**

```bash
cmake --build build --target vox_test
./bin/vox_test
# 或
cd build && ctest
```

## 架构概览

- **核心层**：内存池、数据结构、文件、时间、日志
- **事件循环层**：跨平台 Backend（IOCP / epoll / io_uring / kqueue / select）
- **网络层**：Socket 抽象、TCP、UDP、DNS、TLS/DTLS
- **应用层**：HTTP、WebSocket、数据库、Redis、协程

**平台**：Windows（IOCP）、Linux（epoll/io_uring）、macOS/BSD（kqueue），回退为 select。

## 文档与示例

- [examples/](examples/) — 示例源码

## 性能与实现要点

- 内存池减少碎片与分配开销
- 批量事件处理（IOCP、io_uring）
- Release 可启用 LTO（见 CMakeLists 注释）
- 协程上下文切换约 50–200ns

## 参考

- [libuv](https://github.com/libuv/libuv)
