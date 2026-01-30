# VoxLib SSL 模块

SSL 模块提供 **SSL/TLS 与 DTLS 的抽象层**，当前实现基于 **OpenSSL Memory BIO**，供 `vox_tls`、`vox_dtls`、HTTPS、WSS 等使用。

## 特性

- **统一 API**：Context + Session，不直接依赖具体 SSL 库类型
- **Memory BIO**：读写通过 rbio/wbio，便于与异步 I/O（vox_tcp/udp）对接
- **TLS / DTLS**：同一抽象层支持 TLS（TCP）与 DTLS（UDP），通过配置切换
- **服务端/客户端**：`VOX_SSL_MODE_SERVER` / `VOX_SSL_MODE_CLIENT`
- **可配置**：证书/私钥/CA、验证、密码套件、协议版本、DTLS MTU
- **多后端预留**：接口支持 OpenSSL / WolfSSL / mbedTLS，当前仅实现 OpenSSL

## 模块结构

```
ssl/
├── vox_ssl.h           # 公共 API：Context、Session、BIO、错误码
├── vox_ssl.c           # 抽象层实现与后端分发
├── vox_ssl_openssl.h   # OpenSSL 后端声明（内部）
└── vox_ssl_openssl.c   # OpenSSL Memory BIO 实现
```

## 构建选项

CMake 中需启用至少一个 SSL 后端（默认 OpenSSL）：

| 选项 | 默认 | 说明 |
|------|------|------|
| `VOX_USE_OPENSSL` | ON | OpenSSL（当前唯一可用实现） |
| `VOX_USE_WOLFSSL` | OFF | WolfSSL（未实现） |
| `VOX_USE_MBEDTLS` | OFF | mbedTLS（未实现） |

未启用任何后端时，编译会报错。HTTPS/WSS/TLS/DTLS 示例依赖 `VOX_USE_OPENSSL=ON`。

## Context API

- **vox_ssl_context_create(mpool, mode)**：创建 Context；mode 为 `VOX_SSL_MODE_SERVER` 或 `VOX_SSL_MODE_CLIENT`
- **vox_ssl_context_destroy(ctx)**：销毁 Context
- **vox_ssl_context_configure(ctx, config)**：按 `vox_ssl_config_t` 配置证书、CA、验证、密码套件、协议、DTLS MTU

### 配置（vox_ssl_config_t）

| 字段 | 说明 |
|------|------|
| **cert_file** | 证书文件路径（服务端） |
| **key_file** | 私钥文件路径（服务端） |
| **ca_file** | CA 证书文件路径（客户端验证服务端） |
| **ca_path** | CA 证书目录路径 |
| **verify_peer** | 是否验证对端证书（客户端） |
| **verify_hostname** | 是否验证主机名（客户端） |
| **ciphers** | 密码套件列表（OpenSSL 格式） |
| **protocols** | 协议版本字符串；若包含 `"DTLS"` 则切换为 DTLS |
| **dtls_mtu** | DTLS 应用层 MTU（字节）；0 表示默认 1440；建议 IPv4 用 1440，IPv6 用 1420，不超过 1500 |

服务端典型用法：只设 `cert_file`、`key_file`；客户端可选 `ca_file`/`ca_path`、`verify_peer`。DTLS 时在 `protocols` 中带 `"DTLS"` 并设 `dtls_mtu`。

## Session API

Session 从 Context 创建，用于单条连接上的握手与读写：

- **vox_ssl_session_create(ctx, mpool)** / **vox_ssl_session_destroy(session)**
- **vox_ssl_session_handshake(session)**：执行握手；返回 0 表示成功，`VOX_SSL_ERROR_WANT_READ` / `VOX_SSL_ERROR_WANT_WRITE` 表示需先喂入/取出 BIO 数据后重试
- **vox_ssl_session_read(session, buf, len)**：读解密后的应用数据；返回值同 handshake（>0 为字节数，WANT_READ/WANT_WRITE 为需驱动 BIO）
- **vox_ssl_session_write(session, buf, len)**：写待加密数据；返回值同上
- **vox_ssl_session_shutdown(session)**：发送关闭通知
- **vox_ssl_session_get_state(session)**：`VOX_SSL_STATE_INIT` / `HANDSHAKING` / `CONNECTED` / `CLOSED`
- **vox_ssl_session_get_error(session)** / **vox_ssl_session_get_error_string(session, buf, len)**：错误码与字符串

## BIO API（Memory BIO）

Session 内部使用一对 Memory BIO（rbio/wbio）。与 socket 的衔接由上层（如 `vox_tls`、`vox_dtls`）完成：从 socket 读到的数据写入 rbio，从 wbio 读出的数据写入 socket。

- **vox_ssl_session_get_rbio(session)** / **vox_ssl_session_get_wbio(session)**：获取 rbio/wbio 指针（平台特定类型，OpenSSL 为 `BIO*`）
- **vox_ssl_bio_pending(session, bio_type)**：待读取字节数；bio_type 为 `VOX_SSL_BIO_RBIO` 或 `VOX_SSL_BIO_WBIO`
- **vox_ssl_bio_read(session, bio_type, buf, len)** / **vox_ssl_bio_write(session, bio_type, buf, len)**：从 BIO 读/向 BIO 写

一般应用不直接调用 BIO API，而是使用 `vox_tls` / `vox_dtls` 或 HTTP/WebSocket 的 listen_tls / WSS 接口。

## 错误码（vox_ssl_error_t）

- **VOX_SSL_ERROR_NONE**：无错误
- **VOX_SSL_ERROR_WANT_READ**：需要更多输入（先向 rbio 写入数据）
- **VOX_SSL_ERROR_WANT_WRITE**：需要输出（先从 wbio 读出数据发到 socket）
- **VOX_SSL_ERROR_SYSCALL** / **VOX_SSL_ERROR_SSL** / **VOX_SSL_ERROR_ZERO_RETURN** / **VOX_SSL_ERROR_INVALID_STATE**：其它错误或连接关闭

## 快速开始：服务端 TLS（HTTPS 场景）

```c
vox_mpool_t* mpool = vox_loop_get_mpool(loop);
vox_ssl_context_t* ssl_ctx = vox_ssl_context_create(mpool, VOX_SSL_MODE_SERVER);
if (!ssl_ctx) { /* 失败 */ }

vox_ssl_config_t cfg = {0};
cfg.cert_file = "cert/server.crt";
cfg.key_file = "cert/server.key";
if (vox_ssl_context_configure(ssl_ctx, &cfg) != 0) { /* 失败 */ }

/* 将 ssl_ctx 传给 vox_http_server_listen_tls(server, ssl_ctx, &addr, backlog) 等 */
```

## 快速开始：客户端 TLS

```c
vox_ssl_context_t* ssl_ctx = vox_ssl_context_create(mpool, VOX_SSL_MODE_CLIENT);
vox_ssl_config_t cfg = {0};
cfg.verify_peer = true;   /* 可选 */
cfg.ca_file = "cert/ca.crt";  /* 可选 */
if (vox_ssl_context_configure(ssl_ctx, &cfg) != 0) { /* 失败 */ }

/* 将 ssl_ctx 传给 vox_tls 等客户端接口 */
```

## DTLS

- 创建 Context 时仍用 `vox_ssl_context_create(mpool, mode)`。
- 在 **vox_ssl_context_configure** 的 `config->protocols` 中传入包含 `"DTLS"` 的字符串（例如 `"DTLSv1.2"`），实现会将该 Context 切换为 DTLS。
- 可选设置 `config->dtls_mtu`（建议 1440/1420，最大 1500）。
- 之后由 `vox_dtls` 使用该 Context 在 UDP 上建立 DTLS 连接。

## 证书生成

服务端自签名证书示例（仅开发/测试）：

```bash
openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt \
  -days 365 -nodes -subj "/CN=localhost"
```

项目根目录 `cert/` 下可有现成脚本或说明，生产环境请使用正式 CA 签发的证书。

## 使用本模块的上层组件

- **vox_tls**：TLS 封装（TCP），用于 HTTPS 客户端/服务端
- **vox_dtls**：DTLS 封装（UDP）
- **vox_http_server**：`vox_http_server_listen_tls(server, ssl_ctx, addr, backlog)` 提供 HTTPS
- **websocket**：WSS 服务端/客户端通过传入 `vox_ssl_context_t*` 使用 TLS

应用层通常只创建并配置 Context，再交给上述组件；Session 的创建、握手、读写由组件内部完成。

## 示例程序

在启用 `VOX_USE_OPENSSL` 后，可运行（具体目标名以 CMake 为准）：

| 示例 | 说明 |
|------|------|
| `https_server_example` | HTTPS 服务端（证书/私钥路径可传参，默认 cert/server.crt、cert/server.key） |
| `tls_echo_test` | TLS Echo 服务端/客户端 |
| `dtls_echo_test` | DTLS Echo 服务端/客户端 |
| `websocket_echo_server` | WSS 服务端（可选 `--ssl`） |
| `websocket_echo_client` | WSS 客户端（wss:// URL） |

## 注意事项

1. **Context 生命周期**：Context 须在使用它的所有 Session 和上层连接关闭后再 destroy；与 vox_tls/vox_dtls/HTTP/WS 配合时，一般在程序退出或不再监听后再销毁。
2. **Memory BIO**：握手与读写可能返回 WANT_READ/WANT_WRITE，上层必须先把 rbio/wbio 与 socket 数据交换完毕再重试。
3. **证书与私钥**：服务端必须配置 cert_file + key_file，且私钥与证书匹配；客户端验证服务端时配置 ca_file/ca_path 与 verify_peer。
4. **线程**：当前实现未强调线程安全；Context 建议单线程使用或由调用方加锁。
5. **WolfSSL/mbedTLS**：接口已预留，实现尚未完成，需使用 OpenSSL。

## 依赖

- **核心**：`vox_mpool`、`vox_os`（见主库 CMake）
- **OpenSSL 后端**：OpenSSL（vcpkg 或系统包），需 `VOX_USE_OPENSSL` 且链接 OpenSSL
