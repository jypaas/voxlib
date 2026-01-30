#!/bin/bash
# generate_test_cert_with_ip.sh - 生成支持 IP 地址的 TLS 测试证书
# 生成包含 IP 地址 SAN 的自签名证书

echo "========================================"
echo "生成支持 IP 地址的 TLS 测试证书"
echo "========================================"
echo

# 检查 OpenSSL 是否可用
if ! command -v openssl &> /dev/null; then
    echo "错误: 未找到 OpenSSL"
    exit 1
fi

# 创建证书配置文件
cat > cert.conf << 'EOF'
[req]
distinguished_name = req_distinguished_name
req_extensions = v3_req
prompt = no

[req_distinguished_name]
C = CN
ST = Beijing
L = Beijing
O = VoxLib Test
OU = Development
CN = localhost

[v3_req]
basicConstraints = CA:FALSE
keyUsage = nonRepudiation, digitalSignature, keyEncipherment
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
DNS.2 = *.localhost
IP.1 = 127.0.0.1
IP.2 = ::1
IP.3 = 0.0.0.0
EOF

echo "[1/3] 生成 2048 位 RSA 私钥..."
openssl genrsa -out server.key 2048
if [ $? -ne 0 ]; then
    echo "错误: 生成私钥失败"
    rm -f cert.conf
    exit 1
fi
echo "私钥已生成: server.key"
echo

echo "[2/3] 生成自签名证书（包含 IP 地址 SAN，有效期 365 天）..."
openssl req -new -x509 -key server.key -out server.crt -days 365 \
    -config cert.conf -extensions v3_req
if [ $? -ne 0 ]; then
    echo "错误: 生成证书失败"
    rm -f cert.conf
    exit 1
fi
echo "证书已生成: server.crt"
echo

echo "[3/3] 生成 CA 证书（用于客户端验证）..."
# 生成 CA 私钥
openssl genrsa -out ca.key 2048
if [ $? -ne 0 ]; then
    echo "错误: 生成 CA 私钥失败"
    rm -f cert.conf
    exit 1
fi

# 生成 CA 证书
openssl req -new -x509 -key ca.key -out ca.crt -days 365 \
    -subj "/C=CN/ST=Beijing/L=Beijing/O=VoxLib Test CA/OU=Development/CN=VoxLib Test CA"
if [ $? -ne 0 ]; then
    echo "错误: 生成 CA 证书失败"
    rm -f cert.conf
    exit 1
fi
echo "CA 证书已生成: ca.crt"
echo

# 清理临时文件
rm -f cert.conf

echo "========================================"
echo "证书生成完成！"
echo "========================================"
echo
echo "生成的文件:"
echo "  - server.key  : 服务器私钥"
echo "  - server.crt  : 服务器证书（支持 localhost 和 127.0.0.1）"
echo "  - ca.key      : CA 私钥"
echo "  - ca.crt      : CA 证书（用于客户端验证）"
echo
echo "证书包含以下 SAN:"
echo "  - DNS: localhost, *.localhost"
echo "  - IP:  127.0.0.1, ::1, 0.0.0.0"
echo
echo "使用方法:"
echo "  服务器: ./tls_echo_test server 0.0.0.0 8889 server.crt server.key"
echo "  客户端: ./tls_echo_test client 127.0.0.1 8889 \"Hello\" ca.crt true"
echo
