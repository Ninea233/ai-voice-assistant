#!/bin/bash
#===========================================
# build_tflite_arm.sh
# 交叉编译 TensorFlow Lite for ARM (i.MX6ULL)
#
# 使用 CMake 构建（TF v2.14.0+ 推荐方式）
# 前提：已安装 arm-linux-gnueabihf-g++ 交叉编译器
#
# 使用方式:
#   ./scripts/build_tflite_arm.sh
#
# 产物:
#   third_party/tflite/lib/libtensorflow-lite.a
#   third_party/tflite/include/tensorflow/lite/...
#===========================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TFLITE_DIR="${PROJECT_DIR}/third_party/tflite"
BUILD_DIR="${PROJECT_DIR}/build/tflite_build"
TFLITE_SRC="${BUILD_DIR}/tensorflow_src"
TFLITE_BUILD="${BUILD_DIR}/tflite_arm_build"
DEPS_CACHE="${BUILD_DIR}/deps_cache"

# TFLite 版本
TFLITE_VERSION="v2.14.0"

# 交叉编译器（使用系统已安装的）
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf}"

# 并行编译线程数
JOBS=${JOBS:-$(nproc)}

echo "=========================================="
echo " TFLite ARM 交叉编译 (CMake)"
echo " 版本: ${TFLITE_VERSION}"
echo " 编译器: ${CROSS_COMPILE}"
echo " 线程: ${JOBS}"
echo " 输出: ${TFLITE_DIR}"
echo "=========================================="

# ---------- 0. 预下载慢的依赖（避免网络问题导致构建卡住） ----------
echo ""
echo "[0/5] 检查并预下载慢速依赖（ml_dtypes, eigen, pthreadpool, ruy）..."

mkdir -p "${DEPS_CACHE}"

# ml_dtypes（header-only，但有 eigen 子模块导致 gitlab 慢）
ML_DTYPES_DIR="${DEPS_CACHE}/ml_dtypes"
if [ ! -d "${ML_DTYPES_DIR}/ml_dtypes" ]; then
    echo "  下载 ml_dtypes..."
    rm -rf "${ML_DTYPES_DIR}"
    git clone --depth 1 https://github.com/jax-ml/ml_dtypes.git "${ML_DTYPES_DIR}" 2>&1
    echo "  ✅ ml_dtypes: ${ML_DTYPES_DIR}"
else
    echo "  ✅ ml_dtypes 已存在"
fi

# eigen（gitlab 有时较慢，使用 depth=1 加速）
EIGEN_DIR="${DEPS_CACHE}/eigen"
if [ ! -d "${EIGEN_DIR}/Eigen" ]; then
    echo "  下载 eigen (depth=1)..."
    rm -rf "${EIGEN_DIR}"
    git clone --depth 1 https://gitlab.com/libeigen/eigen.git "${EIGEN_DIR}" 2>&1
    echo "  ✅ eigen: ${EIGEN_DIR}"
else
    echo "  ✅ eigen 已存在"
fi

# pthreadpool（GitHub ZIP 下载偶尔连接重置）
PTHREADPOOL_DIR="${DEPS_CACHE}/pthreadpool"
if [ ! -d "${PTHREADPOOL_DIR}/CMakeLists.txt" ]; then
    echo "  下载 pthreadpool..."
    rm -rf "${PTHREADPOOL_DIR}"
    git clone --depth 1 https://github.com/Maratyszcza/pthreadpool.git "${PTHREADPOOL_DIR}" 2>&1
    echo "  ✅ pthreadpool: ${PTHREADPOOL_DIR}"
else
    echo "  ✅ pthreadpool 已存在"
fi

# ruy（矩阵运算库，需要预下载）
RUY_DIR="${DEPS_CACHE}/ruy"
if [ ! -d "${RUY_DIR}/ruy" ]; then
    echo "  下载 ruy..."
    rm -rf "${RUY_DIR}"
    git clone --depth 1 https://github.com/google/ruy.git "${RUY_DIR}" 2>&1
    echo "  ✅ ruy: ${RUY_DIR}"
else
    echo "  ✅ ruy 已存在"
fi

# ---------- 1. 克隆源码 ----------
if [ ! -d "${TFLITE_SRC}" ]; then
    echo ""
    echo "[1/5] 克隆 TensorFlow Lite ${TFLITE_VERSION}..."
    mkdir -p "${BUILD_DIR}"
    git clone --depth 1 --branch "${TFLITE_VERSION}" \
        https://github.com/tensorflow/tensorflow.git "${TFLITE_SRC}"
else
    echo ""
    echo "[1/5] TFLite 源码已存在，跳过克隆"
fi

# ---------- 2. 创建 CMake 工具链文件 ----------
echo ""
echo "[2/5] 创建 ARM 交叉编译工具链文件..."

TOOLCHAIN_FILE="${BUILD_DIR}/arm_toolchain.cmake"
cat > "${TOOLCHAIN_FILE}" << EOF
# ARM 交叉编译工具链（使用系统已安装的 arm-linux-gnueabihf-g++）
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER ${CROSS_COMPILE}-gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}-g++)
set(CMAKE_AR ${CROSS_COMPILE}-ar)
set(CMAKE_RANLIB ${CROSS_COMPILE}-ranlib)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ARM v7a 优化
set(CMAKE_C_FLAGS "\${CMAKE_C_FLAGS} -march=armv7-a -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS "\${CMAKE_CXX_FLAGS} -march=armv7-a -mfpu=neon -mfloat-abi=hard")
EOF

echo " 工具链文件: ${TOOLCHAIN_FILE}"

# ---------- 3. CMake 配置 ----------
echo ""
echo "[3/5] CMake 配置..."

# 清理旧的构建目录，确保全新配置
rm -rf "${TFLITE_BUILD}"
mkdir -p "${TFLITE_BUILD}"
cd "${TFLITE_BUILD}"

cmake "${TFLITE_SRC}/tensorflow/lite" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTFLITE_ENABLE_XNNPACK=OFF \
    -DTFLITE_ENABLE_GPU=OFF \
    -DTFLITE_ENABLE_RPC=OFF \
    -DTFLITE_ENABLE_NNAPI=OFF \
    -DTFLITE_ENABLE_MMAP=OFF \
    -DTFLITE_CUSTOM_OP_REGISTRATION=ON \
    -DTFLITE_ENABLE_METAL=OFF \
    -DTFLITE_ENABLE_RUY=OFF \
    -DPTHREADPOOL_SOURCE_DIR="${PTHREADPOOL_DIR}" \
    -DFETCHCONTENT_SOURCE_DIR_ML_DTYPES="${ML_DTYPES_DIR}" \
    -DFETCHCONTENT_SOURCE_DIR_EIGEN="${EIGEN_DIR}" \
    -DFETCHCONTENT_SOURCE_DIR_RUY="${RUY_DIR}" \
    2>&1 | tail -40

# ---------- 4. 编译 ----------
echo ""
echo "[4/5] 编译 TFLite ARM 静态库..."
cmake --build . -j"${JOBS}" 2>&1 | tail -40

echo ""
echo "[5/5] 编译完成，复制产物..."

# 创建目标目录
mkdir -p "${TFLITE_DIR}/lib"
mkdir -p "${TFLITE_DIR}/include"

# 复制静态库
LIB_PATH="${TFLITE_BUILD}/libtensorflow-lite.a"
if [ -f "${LIB_PATH}" ]; then
    cp "${LIB_PATH}" "${TFLITE_DIR}/lib/"
    echo " ✅ 静态库: $(ls -lh ${TFLITE_DIR}/lib/libtensorflow-lite.a)"
else
    echo " ❌ 错误: 未找到 ${LIB_PATH}"
    find "${TFLITE_BUILD}" -name "*.a" 2>/dev/null
    exit 1
fi

# 复制头文件
cd "${TFLITE_SRC}"
find "tensorflow/lite" -name "*.h" | while IFS= read -r header; do
    mkdir -p "${TFLITE_DIR}/include/$(dirname ${header})"
    cp "${header}" "${TFLITE_DIR}/include/${header}"
done

# 复制 flatbuffers 头文件（TFLite 模型格式必需）
for fb_dir in "${TFLITE_BUILD}/flatbuffers/include" "${TFLITE_BUILD}/_deps/flatbuffers-src/include"; do
    if [ -d "${fb_dir}/flatbuffers" ]; then
        cp -r "${fb_dir}/flatbuffers" "${TFLITE_DIR}/include/"
        echo " ✅ flatbuffers 头文件"
        break
    fi
done

# 复制 abseil 头文件
for ab_dir in "${TFLITE_BUILD}/abseil-cpp" "${TFLITE_BUILD}/_deps/abseil-cpp-src"; do
    if [ -d "${ab_dir}/absl" ]; then
        mkdir -p "${TFLITE_DIR}/include/absl"
        cp -r "${ab_dir}/absl"/* "${TFLITE_DIR}/include/absl/"
        echo " ✅ absl 头文件"
        break
    fi
done

# 复制 farmhash 头文件
for fh_dir in "${TFLITE_BUILD}/farmhash" "${TFLITE_BUILD}/_deps/farmhash-src/src"; do
    if [ -f "${fh_dir}/farmhash.h" ]; then
        cp "${fh_dir}/farmhash.h" "${TFLITE_DIR}/include/"
        echo " ✅ farmhash 头文件"
        break
    fi
done

# 复制 cpuinfo 头文件
for ci_dir in "${TFLITE_BUILD}/cpuinfo" "${TFLITE_BUILD}/_deps/cpuinfo-src/include"; do
    if [ -d "${ci_dir}/cpuinfo" ]; then
        mkdir -p "${TFLITE_DIR}/include/cpuinfo"
        cp -r "${ci_dir}/cpuinfo"/* "${TFLITE_DIR}/include/cpuinfo/" 2>/dev/null || true
        echo " ✅ cpuinfo 头文件"
        break
    fi
    if [ -f "${ci_dir}/cpuinfo.h" ]; then
        cp "${ci_dir}/cpuinfo.h" "${TFLITE_DIR}/include/" 2>/dev/null || true
        echo " ✅ cpuinfo 头文件"
        break
    fi
done

# 复制 eigen 头文件
if [ -d "${EIGEN_DIR}/Eigen" ]; then
    cp -r "${EIGEN_DIR}/Eigen" "${TFLITE_DIR}/include/"
    if [ -d "${EIGEN_DIR}/unsupported" ]; then
        cp -r "${EIGEN_DIR}/unsupported" "${TFLITE_DIR}/include/"
    fi
    echo " ✅ eigen 头文件"
fi

echo ""
echo "=========================================="
echo " TFLite ARM 交叉编译完成!"
echo " 头文件: ${TFLITE_DIR}/include/tensorflow/lite/"
echo " 静态库: ${TFLITE_DIR}/lib/libtensorflow-lite.a"
echo "=========================================="

# 验证
if [ -f "${TFLITE_DIR}/lib/libtensorflow-lite.a" ] && [ -f "${TFLITE_DIR}/include/tensorflow/lite/model.h" ]; then
    echo " ✅ 验证通过: 库和头文件均已就绪"
else
    echo " ⚠️ 验证失败: 部分文件缺失"
    echo "    库: $([ -f "${TFLITE_DIR}/lib/libtensorflow-lite.a" ] && echo '✅' || echo '❌')"
    echo "    头文件: $([ -f "${TFLITE_DIR}/include/tensorflow/lite/model.h" ] && echo '✅' || echo '❌')"
fi
