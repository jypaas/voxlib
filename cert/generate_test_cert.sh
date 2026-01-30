#!/bin/bash
# generate_test_cert.sh - 生成 TLS 测试证书（Linux/macOS）
# 生成自签名证书用于 TLS echo 测试服务器

echo "========================================"
echo "生成 TLS 测试证书"
echo "========================================"
echo

# 检查 OpenSSL 是否可用
if ! command -v openssl &> /dev/null; then
    echo "错误: 未找到 OpenSSL"
    echo "请安装 OpenSSL:"
    echo "  Ubuntu/Debian: sudo apt-get install openssl"
    echo "  macOS: brew install openssl"
    echo "  CentOS/RHEL: sudo yum install openssl"
    exit 1
fi

echo "[1/3] 生成 2048 位 RSA 私钥..."
openssl genrsa -out server.key 2048
if [ $? -ne 0 ]; then
    echo "错误: 生成私钥失败"
    exit 1
fi
echo "私钥已生成: server.key"
echo

echo "[2/3] 生成自签名证书（有效期 365 天）..."
openssl req -new -x509 -key server.key -out server.crt -days 365 \
    -subj "/C=CN/ST=Beijing/L=Beijing/O=VoxLib Test/OU=Development/CN=localhost"
if [ $? -ne 0 ]; then
    echo "错误: 生成证书失败"
    exit 1
fi
echo "证书已生成: server.crt"
echo

echo "[3/3] 生成 CA 证书（用于客户端验证）..."
# 生成 CA 私钥
openssl genrsa -out ca.key 2048
if [ $? -ne 0 ]; then
    echo "错误: 生成 CA 私钥失败"
    exit 1
fi

# 生成 CA 证书
openssl req -new -x509 -key ca.key -out ca.crt -days 365 \
    -subj "/C=CN/ST=Beijing/L=Beijing/O=VoxLib Test CA/OU=Development/CN=VoxLib Test CA"
if [ $? -ne 0 ]; then
    echo "错误: 生成 CA 证书失败"
    exit 1
fi
echo "CA 证书已生成: ca.crt"
echo

echo "========================================"
echo "证书生成完成！"
echo "========================================"
echo
echo "生成的文件:"
echo "  - server.key  : 服务器私钥"
echo "  - server.crt  : 服务器证书"
echo "  - ca.key      : CA 私钥"
echo "  - ca.crt      : CA 证书（用于客户端验证）"
echo
echo "使用方法:"
echo "  服务器: ./tls_echo_test server 0.0.0.0 8889 server.crt server.key"
echo "  客户端: ./tls_echo_test client 127.0.0.1 8889 \"Hello\" ca.crt true"
echo
