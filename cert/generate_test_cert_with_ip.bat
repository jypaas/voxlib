@echo off
REM generate_test_cert_with_ip.bat - 生成支持 IP 地址的 TLS 测试证书（Windows）
REM 生成包含 IP 地址 SAN 的自签名证书

echo ========================================
echo 生成支持 IP 地址的 TLS 测试证书
echo ========================================
echo.

REM 检查 OpenSSL 是否可用
where openssl >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo 错误: 未找到 OpenSSL
    echo 请确保 OpenSSL 已安装并在 PATH 中
    pause
    exit /b 1
)

REM 创建证书配置文件
(
echo [req]
echo distinguished_name = req_distinguished_name
echo req_extensions = v3_req
echo prompt = no
echo.
echo [req_distinguished_name]
echo C = CN
echo ST = Beijing
echo L = Beijing
echo O = VoxLib Test
echo OU = Development
echo CN = localhost
echo.
echo [v3_req]
echo basicConstraints = CA:FALSE
echo keyUsage = nonRepudiation, digitalSignature, keyEncipherment
echo subjectAltName = @alt_names
echo.
echo [alt_names]
echo DNS.1 = localhost
echo DNS.2 = *.localhost
echo IP.1 = 127.0.0.1
echo IP.2 = ::1
echo IP.3 = 0.0.0.0
) > cert.conf

echo [1/3] 生成 2048 位 RSA 私钥...
openssl genrsa -out server.key 2048
if %ERRORLEVEL% NEQ 0 (
    echo 错误: 生成私钥失败
    del cert.conf 2>nul
    pause
    exit /b 1
)
echo 私钥已生成: server.key
echo.

echo [2/3] 生成自签名证书（包含 IP 地址 SAN，有效期 365 天）...
openssl req -new -x509 -key server.key -out server.crt -days 365 ^
    -config cert.conf -extensions v3_req
if %ERRORLEVEL% NEQ 0 (
    echo 错误: 生成证书失败
    del cert.conf 2>nul
    pause
    exit /b 1
)
echo 证书已生成: server.crt
echo.

echo [3/3] 生成 CA 证书（用于客户端验证）...
REM 生成 CA 私钥
openssl genrsa -out ca.key 2048
if %ERRORLEVEL% NEQ 0 (
    echo 错误: 生成 CA 私钥失败
    del cert.conf 2>nul
    pause
    exit /b 1
)

REM 生成 CA 证书
openssl req -new -x509 -key ca.key -out ca.crt -days 365 ^
    -subj "/C=CN/ST=Beijing/L=Beijing/O=VoxLib Test CA/OU=Development/CN=VoxLib Test CA"
if %ERRORLEVEL% NEQ 0 (
    echo 错误: 生成 CA 证书失败
    del cert.conf 2>nul
    pause
    exit /b 1
)
echo CA 证书已生成: ca.crt
echo.

REM 清理临时文件
del cert.conf 2>nul

echo ========================================
echo 证书生成完成！
echo ========================================
echo.
echo 生成的文件:
echo   - server.key  : 服务器私钥
echo   - server.crt  : 服务器证书（支持 localhost 和 127.0.0.1）
echo   - ca.key      : CA 私钥
echo   - ca.crt      : CA 证书（用于客户端验证）
echo.
echo 证书包含以下 SAN:
echo   - DNS: localhost, *.localhost
echo   - IP:  127.0.0.1, ::1, 0.0.0.0
echo.
echo 使用方法:
echo   服务器: tls_echo_test server 0.0.0.0 8889 server.crt server.key
echo   客户端: tls_echo_test client 127.0.0.1 8889 "Hello" ca.crt true
echo.
pause
