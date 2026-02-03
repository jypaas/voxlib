# VoxLib 数据库模块

数据库模块提供**统一的高性能数据库抽象层**，支持多种后端驱动，提供同步/异步执行、逐行流式查询、连接池与事务能力。

## 特性

- **统一 API**：`connect` / `exec` / `query`，不直接依赖各驱动头文件
- **多驱动**：SQLite3、DuckDB、PostgreSQL（libpq）、MySQL（libmysqlclient），按 CMake 选项启用
- **逐行流式**：查询默认通过 `row_cb` 逐行回调，避免一次性物化大结果集
- **异步**：复用 `vox_loop` 内部线程池；可选回调在工作线程或 loop 线程触发
- **连接池**：`vox_db_pool` 管理初始/最大连接数，支持 acquire/release 与便捷的池内 exec/query
- **事务**：同步/异步的 begin/commit/rollback
- **协程适配**：配合 `coroutine/vox_coroutine_db.h` 使用 `*_await` 接口
- **ORM**：`vox_orm.h/c` 提供按实体描述符的 CRUD，屏蔽占位符与方言差异，便于后期更换数据库

## 模块结构

```
db/
├── vox_db.h              # 公共 API：连接、exec、query、事务
├── vox_db.c              # 抽象层实现与驱动分发
├── vox_db_internal.h     # 内部驱动表与连接结构（不对外）
├── vox_db_pool.h/c       # 连接池
├── vox_orm.h/c           # 薄 ORM：Insert/Update/Delete/Select 单行与多行，同步与异步
├── vox_db_sqlite3.c      # SQLite3 驱动（VOX_USE_SQLITE3）
├── vox_db_duckdb.c       # DuckDB 驱动（VOX_USE_DUCKDB）
├── vox_db_pgsql.c        # PostgreSQL 驱动（VOX_USE_PGSQL）
└── vox_db_mysql.c        # MySQL 驱动（VOX_USE_MYSQL）
```

## 构建选项

在 CMake 中可启用一个或多个驱动（至少启用一个才能使用 DB 功能）：

| 选项 | 默认 | 说明 |
|------|------|------|
| `VOX_USE_SQLITE3` | OFF | SQLite3 嵌入式数据库 |
| `VOX_USE_DUCKDB` | ON | DuckDB 嵌入式分析库 |
| `VOX_USE_PGSQL` | OFF | PostgreSQL（libpq） |
| `VOX_USE_MYSQL` | OFF | MySQL（libmysqlclient） |

示例：

```bash
cmake -DVOX_USE_SQLITE3=ON -DVOX_USE_DUCKDB=ON ..
```

## 连接

### 建立连接

```c
vox_loop_t* loop = vox_loop_create();
vox_db_conn_t* conn = vox_db_connect(loop, driver, conninfo);
if (!conn) { /* 失败，可查 vox_db_last_error(conn) 无意义，用驱动/日志 */ }
```

### 连接串（conninfo）格式

| 驱动 | conninfo 说明 |
|------|----------------|
| **SQLite3** | 文件路径；`":memory:"` 内存库；可选 `file:` 前缀 |
| **DuckDB** | 路径或 `path;key=value;...`（支持 `encryption_key`/`password`、`motherduck_token`） |
| **PostgreSQL** | libpq 格式，例如 `"host=127.0.0.1 port=5432 user=... dbname=... password=..."` |
| **MySQL** | 简化 DSN：`host=... port=... user=... password=... db=... charset=...`（具体以 `vox_db_mysql.c` 为准） |

### DuckDB 密码 / 认证

conninfo 支持可选参数，格式为 **`path;key=value;key2=value2`**（分号分隔）：

- **motherduck_token**：MotherDuck 云认证 token  
  例：`md:your_database;motherduck_token=ey...`
- **encryption_key** 或 **password**：本地加密库（DuckDB 1.4+）密钥  
  例：`/path/to/file.duckdb;encryption_key=your_key` 或 `file.duckdb;password=secret`

仅路径（无分号）时行为不变：`":memory:"` 或 `file.duckdb`。驱动内部用 `duckdb_open_ext` + config 传入上述选项。

### 关闭连接

```c
vox_db_disconnect(conn);
```

## 数据类型

- **vox_db_driver_t**：`VOX_DB_DRIVER_SQLITE3` / `VOX_DB_DRIVER_DUCKDB` / `VOX_DB_DRIVER_PGSQL` / `VOX_DB_DRIVER_MYSQL`
- **vox_db_type_t**：列类型（NULL / I64 / U64 / F64 / BOOL / TEXT / BLOB）
- **vox_db_value_t**：参数或列值（type + union：i64, u64, f64, boolean, text, blob）
- **vox_db_row_t**：一行数据（column_count, column_names, values）；**仅在 row_cb 调用期间有效，如需持久化请自行拷贝**

## 同步 API（阻塞当前线程）

适用于工作线程或脚本/工具场景；**不要在事件循环线程长时间阻塞**。

```c
int64_t affected = 0;
int ret = vox_db_exec(conn, "INSERT INTO t VALUES(?, ?);", params, 2, &affected);

int64_t row_count = 0;
ret = vox_db_query(conn, "SELECT id, name FROM t;", NULL, 0, row_cb, row_user_data, &row_count);
```

- **vox_db_exec**：执行不返回结果集的 SQL，可选输出受影响行数
- **vox_db_query**：执行查询，每行调用一次 `row_cb`，回调在当前线程触发

## 异步 API（不阻塞）

异步操作在线程池中执行，通过回调返回结果。

### 回调线程模式

- **VOX_DB_CALLBACK_WORKER**（默认）：回调在**工作线程**触发，延迟最低，适合纯 DB 逻辑
- **VOX_DB_CALLBACK_LOOP**：回调通过 `vox_loop_queue_work` 切回 **loop 线程**，适合在回调里操作 HTTP/其他 loop 绑定对象

```c
vox_db_set_callback_mode(conn, VOX_DB_CALLBACK_LOOP);  /* 仅影响该连接后续异步操作 */
```

### 异步执行（不返回结果集）

```c
int vox_db_exec_async(conn, sql, params, nparams, exec_cb, user_data);
/* exec_cb(conn, status, affected_rows, user_data) */
```

### 异步查询（逐行回调）

```c
int vox_db_query_async(conn, sql, params, nparams, row_cb, done_cb, user_data);
/* 每行: row_cb(conn, row, user_data)；row 仅在本次调用内有效 */
/* 结束: done_cb(conn, status, row_count, user_data) */
```

**注意**：`row_cb` 中收到的 `vox_db_row_t` 及其内部指针仅在该回调期间有效；若需在回调外使用，必须自行拷贝数据。

## 事务

- 同步：`vox_db_begin_transaction(conn)` / `vox_db_commit(conn)` / `vox_db_rollback(conn)`
- 异步：`vox_db_begin_transaction_async` / `vox_db_commit_async` / `vox_db_rollback_async`，通过 `vox_db_exec_cb` 通知完成

同一连接默认**不支持并发**执行多条语句（避免 native handle 的并发未定义行为）；高并发请用连接池。

## 连接池（vox_db_pool）

### 创建与销毁

```c
vox_db_pool_t* pool = vox_db_pool_create(loop, driver, conninfo,
    initial_size,   /* 常驻连接数 */
    max_size,       /* 最大连接数（含临时连接） */
    connect_cb,     /* 初始连接建完后的回调，可为 NULL */
    user_data);
/* ... 使用 ... */
vox_db_pool_destroy(pool);
```

要求 `initial_size > 0` 且 `initial_size <= max_size`。

### 获取与归还连接

```c
vox_db_pool_acquire_async(pool, acquire_cb, user_data);
/* acquire_cb(pool, conn, status, user_data)：成功时 conn 非 NULL、status==0 */
/* 用完后必须归还 */
vox_db_pool_release(pool, conn);
```

归还时：若该连接是临时连接则关闭并移除；否则标记为空闲供后续 acquire 使用。

### 池内回调模式

```c
vox_db_pool_set_callback_mode(pool, VOX_DB_CALLBACK_LOOP);
vox_db_callback_mode_t mode = vox_db_pool_get_callback_mode(pool);
```

对池内**所有连接**生效（新取出的连接会使用该模式）。

### 便捷接口（内部借还连接）

无需手动 acquire/release，适合单次请求：

- **异步**：`vox_db_pool_exec_async`、`vox_db_pool_query_async`
- **同步**：`vox_db_pool_exec`、`vox_db_pool_query`

### 池状态

- `vox_db_pool_initial_size(pool)` / `vox_db_pool_max_size(pool)`：创建时参数
- `vox_db_pool_current_size(pool)`：当前总连接数（常驻已建立 + 临时）
- `vox_db_pool_available(pool)`：当前空闲连接数（仅常驻中的空闲）

## 协程适配

配合 `coroutine/vox_coroutine_db.h` 可在协程内用 await 风格写 DB 逻辑，避免回调嵌套：

- `vox_coroutine_db_exec_await` / `vox_coroutine_db_query_await`
- `vox_coroutine_db_begin_transaction_await` / `vox_coroutine_db_commit_await` / `vox_coroutine_db_rollback_await`
- 连接池：`vox_coroutine_db_pool_acquire_await`、`vox_coroutine_db_pool_exec_await`、`vox_coroutine_db_pool_query_await`

详见 `coroutine/README.md` 与示例 `examples/db_coroutine_example.c`。

## 示例程序

在项目根目录构建并启用至少一个 DB 驱动后，可运行（具体目标名以 CMake 为准）：

| 示例 | 说明 |
|------|------|
| `db_example` | 单连接 + 异步 exec/query，轮询等待完成 |
| `db_sync_example` | 单连接 + 同步 exec/query（阻塞当前线程） |
| `db_async_loop_example` | 单连接 + VOX_DB_CALLBACK_LOOP，在 loop 线程处理回调 |
| `db_pool_async_example` | 连接池 + 异步，多“请求”并发 |
| `db_coroutine_example` | 协程 + DB/池的 *_await 用法 |
| `http_server_db_async_example` | HTTP 服务内使用异步 DB（含池） |
| `http_server_db_sync_example` | HTTP 服务内使用同步 DB（工作线程） |

## 注意事项

1. **row 数据生命周期**：`row_cb` 中的 `vox_db_row_t` 及列名/值引用的内存仅在该回调内有效；需在回调外使用请自行拷贝。
2. **单连接不并发**：同一 `vox_db_conn_t` 不要并发调用 exec/query；高并发请用连接池。
3. **参数生命周期**：异步调用时，`params` 在提交后可能被工作线程读取，需保证在回调完成前有效（例如用堆/池分配或持久化结构）。
4. **回调线程**：默认在工作线程回调；在回调里访问 loop 绑定对象时，应使用 `VOX_DB_CALLBACK_LOOP` 或自己在回调里 `vox_loop_queue_work` 切回 loop。
5. **SQL 方言**：参数占位符（如 `?`）与 SQL 语法因驱动而异（SQLite/DuckDB 用 `?`；PG/MySQL 可能不同），请按所用驱动文档编写。
6. **至少一个驱动**：CMake 需至少启用一个 `VOX_USE_*` DB 驱动，否则 DB 相关 API 不可用或链接失败。

## ORM（vox_orm）

在 `vox_db` 之上提供按实体描述符的 CRUD，自动生成各驱动兼容的 SQL（占位符 `?` / `$1,$2,...`），便于后期更换数据库。

- **实体描述**：结构体 + `vox_orm_field_t` 数组（列名、类型、offset、主键、auto_gen、buffer_size）
- **写**：`vox_orm_insert` / `vox_orm_update` / `vox_orm_delete`（同步/异步）
- **读**：`vox_orm_select_one` / `vox_orm_select`（单行/多行，同步/异步），多行结果 push 到 `vox_vector_t*`
- **事务**：仍使用 `vox_db_begin_transaction` / `vox_db_commit` / `vox_db_rollback`
- **协程**：`vox_coroutine_db.h` 提供 `vox_coroutine_orm_create_table_await`、`vox_coroutine_orm_insert_await`、`vox_coroutine_orm_select_one_await`、`vox_coroutine_orm_select_await` 等，在协程内以 await 风格调用 ORM。

详见 `db/ORM_DESIGN.md`。

## 依赖

- **核心**：`vox_loop`、`vox_mpool`、`vox_mutex`、`vox_tpool`、`vox_string`（见主库 CMake）
- **各驱动**：对应系统库或 vcpkg（sqlite3、duckdb、libpq、libmysqlclient）
