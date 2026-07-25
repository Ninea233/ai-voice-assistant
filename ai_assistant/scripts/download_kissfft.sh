#!/bin/bash
#===========================================
# download_kissfft.sh
# 下载 kissfft 轻量 FFT 库到 third_party/
#===========================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
KISSFFT_DIR="${PROJECT_DIR}/third_party/kissfft"

# 使用 kissfft v131 稳定版
KISSFFT_VERSION="v131"
KISSFFT_URL="https://github.com/mborgerding/kissfft/archive/refs/tags/${KISSFFT_VERSION}.tar.gz"

echo "=========================================="
echo " 下载 kissfft ${KISSFFT_VERSION}"
echo " 目标: ${KISSFFT_DIR}"
echo "=========================================="

mkdir -p "${KISSFFT_DIR}"

# 下载并解压
TMP_DIR=$(mktemp -d)
cd "${TMP_DIR}"
wget -q "${KISSFFT_URL}" -O kissfft.tar.gz
tar xzf kissfft.tar.gz

# 复制需要的文件（只取核心文件）
cd kissfft-*
cp kiss_fft.h _kiss_fft_guts.h kiss_fft.c "${KISSFFT_DIR}/"
cp tools/kiss_fftr.h tools/kiss_fftr.c "${KISSFFT_DIR}/" 2>/dev/null || true
cp CHANGELOG "${KISSFFT_DIR}/" 2>/dev/null || true

# 清理
rm -rf "${TMP_DIR}"

echo "✅ kissfft 下载完成"
ls -lh "${KISSFFT_DIR}/"
echo "=========================================="
