# VoxLib Redis 模块

Redis 模块提供**基于 vox_loop 的异步 Redis 客户端**与**连接池**，支持 RESP 协议解析，可与协程适配器配合使用。

## 特性

- **异步客户端**：基于 `vox_tcp` 与 `vox_loop`，非阻塞连接与命令收发
- **RESP 协议**：内置 RESP 解析器（`vox_redis_parser`），支持流式、零拷贝解析
- **统一响应类型**：Simple String、Error、Integer、Bulk String、Array、NULL
- **常用命令封装**：PING、GET/SET/DEL、哈希/列表/集合等便捷接口，以及通用 `command`/`commandv`/`command_raw`
- **连接池**：初始连接数 + 最大连接数，acquire/release 管理，临时连接用完后自动关闭
- **协程适配**：配合 `coroutine/vox_coroutine_redis.h` 使用 `*_await` 接口

## 模块结构

```
redis/
├── vox_redis_client.h/c   # 异步客户端：连接、命令、响应回调
├── vox_redis_parser.h/c   # RESP 解析器（流式、零拷贝）
└── vox_redis_pool.h/c     # 连接池
```

## 客户端（vox_redis_client）

### 创建与连接

```c
vox_loop_t* loop = vox_loop_create();
vox_redis_client_t* client = vox_redis_client_create(loop);
if (!client) { /* 失败 */ }

vox_redis_client_connect(client, "127.0.0.1", 6379, on_connect, user_data);
/* 在 on_connect 中 status==0 表示成功，可开始发命令 */
```

- **vox_redis_client_create(loop)**：创建客户端
- **vox_redis_client_destroy(client)**：销毁客户端（会断开连接）
- **vox_redis_client_connect(client, host, port, connect_cb, user_data)**：异步连接
- **vox_redis_client_disconnect(client)**：断开连接
- **vox_redis_client_is_connected(client)**：是否已连接

### 命令执行

所有命令均为异步，通过回调返回响应；**响应数据仅在回调期间有效**，若需在回调外使用请拷贝或使用 `vox_redis_response_copy`。

**通用命令（推荐用数组版本，更安全）**：

```c
const char* argv[] = { "GET", "mykey" };
vox_redis_client_commandv(client, response_cb, error_cb, user_data, 2, argv);
```

- **vox_redis_client_command(client, cb, error_cb, user_data, format, ...)**：可变参数，参数须为字符串
- **vox_redis_client_commandv(client, cb, error_cb, user_data, argc, argv)**：数组版本
- **vox_redis_client_command_raw(client, buf, len, cb, error_cb, user_data)**：已序列化的 RESP 缓冲区

**便捷命令**（均带 response_cb + user_data）：

| 类别 | 命令示例 |
|------|----------|
| 连接 | PING |
| 字符串 | GET, SET, DEL, EXISTS, INCR, DECR |
| 哈希 | HSET, HGET, HDEL, HEXISTS |
| 列表 | LPUSH, RPUSH, LPOP, RPOP, LLEN |
| 集合 | SADD, SREM, SMEMBERS, SCARD, SISMEMBER |

### 响应类型（vox_redis_response_t）

- **VOX_REDIS_RESPONSE_SIMPLE_STRING**：`u.simple_string.data/len`
- **VOX_REDIS_RESPONSE_ERROR**：`u.error.message/len`
- **VOX_REDIS_RESPONSE_INTEGER**：`u.integer`
- **VOX_REDIS_RESPONSE_BULK_STRING**：`u.bulk_string.data/len/is_null`
- **VOX_REDIS_RESPONSE_ARRAY**：`u.array.count`、`u.array.elements`
- **VOX_REDIS_RESPONSE_NULL**：无数据

响应在回调外如需保留，可使用：

- **vox_redis_response_copy(mpool, src, dst)**：深拷贝到已分配的 `dst`
- **vox_redis_response_free(mpool, response)**：释放由解析器/客户端分配在 mpool 上的响应（如拷贝后不再需要原结构时，按实现约定使用；通常回调内无需手动 free）

## 连接池（vox_redis_pool）

### 创建与销毁

```c
vox_redis_pool_t* pool = vox_redis_pool_create(loop, "127.0.0.1", 6379,
    initial_size,   /* 常驻连接数 */
    max_size,       /* 最大连接数（含临时连接） */
    connect_cb,     /* 初始连接建完后的回调，可为 NULL */
    user_data);
/* ... 使用 ... */
vox_redis_pool_destroy(pool);
```

要求 `initial_size > 0` 且 `initial_size <= max_size`。

### 获取与归还

```c
vox_redis_pool_acquire_async(pool, acquire_cb, user_data);
/* acquire_cb(pool, client, status, user_data)：成功时 client 非 NULL、status==0 */
/* 用完后必须归还 */
vox_redis_pool_release(pool, client);
```

归还时：若该连接是临时连接则关闭并移除；否则标记为空闲供后续 acquire 使用。

### 池状态

- **vox_redis_pool_initial_size(pool)** / **vox_redis_pool_max_size(pool)**：创建时参数
- **vox_redis_pool_current_size(pool)**：当前总连接数（常驻已建立 + 临时）
- **vox_redis_pool_available(pool)**：当前空闲连接数（仅常驻中的空闲）

## RESP 解析器（vox_redis_parser）

用于底层流式解析 RESP 协议，客户端内部已使用；若需自定义协议处理可直接使用解析器。

- **vox_redis_parser_create(mpool, config, callbacks)**：创建解析器
- **vox_redis_parser_execute(parser, data, len)**：喂入数据，返回已解析字节数
- **vox_redis_parser_reset(parser)**：重置状态，解析新的 RESP 对象
- **vox_redis_parser_is_complete(parser)** / **vox_redis_parser_has_error(parser)** / **vox_redis_parser_get_error(parser)**：状态与错误

配置可限制 `max_bulk_string_size`、`max_array_size`、`max_nesting_depth`（0 表示不限制）。

## 协程适配

配合 `coroutine/vox_coroutine_redis.h` 可在协程内用 await 风格写 Redis 逻辑：

- 连接：`vox_coroutine_redis_connect_await`
- 通用：`vox_coroutine_redis_command_await`
- 便捷：`vox_coroutine_redis_ping_await`、`vox_coroutine_redis_get_await`、`vox_coroutine_redis_set_await` 等
- 连接池：`vox_coroutine_redis_pool_acquire_await`、`vox_coroutine_redis_pool_command_await`、`vox_coroutine_redis_pool_get_await` 等

响应在协程内通过 `vox_redis_response_t` 输出参数返回；使用完毕后按头文件说明释放（如 `vox_redis_response_free(loop_mpool, response)`）。详见 `coroutine/README.md` 与示例 `coroutine_clients_example.c`。

## 示例程序

在项目根目录构建后，可运行（具体目标名以 CMake 为准）：

| 示例 | 说明 |
|------|------|
| `redis_client_example` | 单客户端连接、PING/SET/GET/HSET/HGET/LPUSH/SADD 等 |
| `redis_pool_dynamic_example` | 连接池 acquire/release，并发发命令 |
| `redis_pool_comparison` | 池与单连接对比示例 |
| `coroutine_clients_example` | 协程内使用 Redis 客户端与池的 `*_await` |

## 注意事项

1. **响应生命周期**：命令回调中的 `vox_redis_response_t*` 及其内部指针仅在该回调内有效；需在回调外使用请用 `vox_redis_response_copy` 或自行拷贝数据，并按需 `vox_redis_response_free`。
2. **单连接不并发**：同一 `vox_redis_client_t` 不宜并发发起多条命令（请求/响应需串行匹配）；高并发请用连接池。
3. **回调线程**：连接与命令回调均在 `vox_loop` 所在线程执行，可直接操作 loop 绑定对象。
4. **错误处理**：连接失败或命令错误通过 connect_cb 的 status、error_cb 或 response 的 `VOX_REDIS_RESPONSE_ERROR` 传递，请统一检查。
5. **依赖**：需 `vox_loop`、`vox_tcp`、`vox_dns`、`vox_mpool`、`vox_string` 等（见主库 CMake）；无额外第三方 Redis 库。
