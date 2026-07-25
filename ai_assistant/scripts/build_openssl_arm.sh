#!/bin/bash
#===========================================
# build_openssl_arm.sh
# 交叉编译 OpenSSL 1.1.1d ARM 静态库
#
# 编译产物输出到: third_party/openssl/
#===========================================
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OPENSSL_DIR="${PROJECT_DIR}/third_party/openssl"

# 工具链
LINARO=/opt/arm-linux-gnueabihf
CROSS_PREFIX="${LINARO}/bin/arm-linux-gnueabihf-"

OPENSSL_VERSION="1.1.1d"
OPENSSL_TAR="openssl-${OPENSSL_VERSION}.tar.gz"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/OpenSSL_${OPENSSL_VERSION//./_}/${OPENSSL_TAR}"

echo "=========================================="
echo " OpenSSL ${OPENSSL_VERSION} ARM 交叉编译"
echo "=========================================="

# 下载
if [ ! -f "/tmp/${OPENSSL_TAR}" ]; then
    echo "下载 OpenSSL ${OPENSSL_VERSION}..."
    curl -sL "${OPENSSL_URL}" -o "/tmp/${OPENSSL_TAR}"
fi

# 解压
cd /tmp
rm -rf "openssl-${OPENSSL_VERSION}"
tar xzf "${OPENSSL_TAR}"
cd "openssl-${OPENSSL_VERSION}"

# 配置 ARM 交叉编译
echo "配置..."
./Configure linux-armv4 \
    --cross-compile-prefix="${CROSS_PREFIX}" \
    --prefix=/tmp/openssl_arm_build \
    no-asm no-shared \
    --openssldir=/tmp/openssl_arm_build

# 编译
echo "编译 (ARM)..."
make -j$(nproc)

# 复制产物到项目目录
echo "安装..."
mkdir -p "${OPENSSL_DIR}/lib" "${OPENSSL_DIR}/include"
cp libcrypto.a libssl.a "${OPENSSL_DIR}/lib/"
cp -r include/openssl "${OPENSSL_DIR}/include/"

# 清理
make clean 2>/dev/null || true

echo ""
echo "完成！"
echo "  库: ${OPENSSL_DIR}/lib/"
ls -lh "${OPENSSL_DIR}/lib/"
echo "  头文件: ${OPENSSL_DIR}/include/openssl/"
echo "=========================================="
