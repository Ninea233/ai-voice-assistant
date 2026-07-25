#!/bin/bash
#===========================================
# cross_compile.sh
# AI 语音助手交叉编译脚本
#
# 使用方式:
#   ./scripts/cross_compile.sh                   # Debug 编译
#   ./scripts/cross_compile.sh release           # Release 编译
#   ./scripts/cross_compile.sh release tflite    # Release + TFLite
#   ./scripts/cross_compile.sh release "" sdk    # Release + SparkChain SDK (TTS)
#
# 依赖:
#   arm-linux-gnueabihf-g++       (目标编译器，默认)
#   Linaro GCC 4.9.4              (tflite 模式，提供 GLIBC 2.19 兼容)
#   libasound2-dev                (ALSA 头文件)
#   kissfft                       (自动下载)
#   TFLite for ARM (可选)         (需先运行 build_tflite_arm.sh)
#===========================================

set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
ROOTFS="${ROOTFS:-}"

# 编译类型
BUILD_TYPE="${1:-debug}"
ENABLE_TFLITE="${2:-}"
ENABLE_SPARKCHAIN="${3:-}"

# Linaro 工具链路径（用于 glibc 2.19 兼容，tflite 模式必须）
LINARO=/opt/arm-linux-gnueabihf
LINARO_CXX="${LINARO}/bin/arm-linux-gnueabihf-g++"
GCC11_LIBDIR=/usr/lib/gcc-cross/arm-linux-gnueabihf/11

# ALSA 头文件（避免使用 -I/usr/include 污染 glibc 版本）
ALSA_ARM_INC="${TMPDIR:-/tmp}/alsa_arm_include"
if [ ! -f "${ALSA_ARM_INC}/alsa/asoundlib.h" ]; then
    mkdir -p "${ALSA_ARM_INC}"
    cp -r /usr/include/alsa "${ALSA_ARM_INC}/"
fi

echo "=========================================="
echo " AI 语音助手 - 交叉编译"
if [ "${ENABLE_TFLITE}" = "tflite" ]; then
    echo " 编译器: Linaro GCC 4.9.4 (GLIBC 2.19 兼容)"
    echo " TFLite: 启用 (C++14 模式，补丁头文件)"
else
    echo " 编译器: arm-linux-gnueabihf-g++ (默认)"
    echo " TFLite: 禁用 (SimpleKWS 能量检测)"
fi
echo " 类型: ${BUILD_TYPE}"
echo " 输出: ${BUILD_DIR}"
echo "=========================================="

mkdir -p "${BUILD_DIR}"

# ============================================
# 编译器和标志
# ============================================
if [ "${ENABLE_TFLITE}" = "tflite" ]; then
    CXX="${LINARO_CXX}"
    CXX_STD="c++14"
    CXXFLAGS=(-std=${CXX_STD} -Os -DNDEBUG -DUSE_TFLITE)

    # TFLite 头文件需要先打 C++17→C++14 补丁
    TFLITE_INC="${PROJECT_DIR}/third_party/tflite/include"
    PATCH_FLAG="${TFLITE_INC}/.patched_for_cxx14"
    if [ ! -f "${PATCH_FLAG}" ]; then
        echo " 补丁 TFLite 头文件 (C++17→C++14)..."
        python3 "${PROJECT_DIR}/scripts/patch_tflite_headers_cxx14.py"
        touch "${PATCH_FLAG}"
    fi

    CXXFLAGS+=(-I"${PROJECT_DIR}/include")
    CXXFLAGS+=(-I"${PROJECT_DIR}/third_party/kissfft")
    CXXFLAGS+=(-I"${TFLITE_INC}")
    CXXFLAGS+=(-I"${ALSA_ARM_INC}")
    CXXFLAGS+=(-I"${PROJECT_DIR}/third_party/openssl/include")
    CXXFLAGS+=(-std=c++14)

    # SparkChain SDK (TTS)
    SPARKCHAIN_DIR="${PROJECT_DIR}/../sparkchain_sdk"
    if [ "${ENABLE_SPARKCHAIN}" = "sdk" ] && [ -f "${SPARKCHAIN_DIR}/include/sparkchain.h" ]; then
        CXXFLAGS+=(-I"${SPARKCHAIN_DIR}/include")
        CXXFLAGS+=(-DENABLE_SPARKCHAIN_SDK)
        echo " SparkChain SDK: 已启用"
    fi
else
    CXX="${CROSS_COMPILE:-arm-linux-gnueabihf}-g++"
    CXX_STD="c++14"
    CXXFLAGS=(-std=${CXX_STD} -Wall -Wextra ${CXXFLAGS_EXTRA:-})

    if [ "${BUILD_TYPE}" = "release" ]; then
        CXXFLAGS+=(-Os -DNDEBUG)
    else
        CXXFLAGS+=(-g -O0)
    fi

    CXXFLAGS+=(-I"${PROJECT_DIR}/include")
    CXXFLAGS+=(-I"${PROJECT_DIR}/third_party/openssl/include")

    # SparkChain SDK (TTS)
    SPARKCHAIN_DIR="${PROJECT_DIR}/../sparkchain_sdk"
    if [ "${ENABLE_SPARKCHAIN}" = "sdk" ] && [ -f "${SPARKCHAIN_DIR}/include/sparkchain.h" ]; then
        CXXFLAGS+=(-I"${SPARKCHAIN_DIR}/include")
        CXXFLAGS+=(-DENABLE_SPARKCHAIN_SDK)
        echo " SparkChain SDK: 已启用"
    fi

    # sysroot 和 ALSA 头文件
    if [ -d "${ROOTFS}" ]; then
        CXXFLAGS+=(-I"${ROOTFS}/usr/include")
    fi
    if [ -f /usr/include/asoundlib.h ] && [ "${ROOTFS}" = "" ]; then
        CXXFLAGS+=(-I/usr/include)
    fi
fi

# 链接标志
LDFLAGS=(-lpthread -ldl -lm)

# OpenSSL ARM 静态库（网络加密必需）
OPENSSL_LIB="${PROJECT_DIR}/third_party/openssl/lib/libssl.a"
OPENSSL_CRYPTO="${PROJECT_DIR}/third_party/openssl/lib/libcrypto.a"
if [ -f "${OPENSSL_LIB}" ]; then
    echo " OpenSSL: ${OPENSSL_LIB}"
    LDFLAGS+=("${OPENSSL_LIB}" "${OPENSSL_CRYPTO}")
else
    echo " 警告: OpenSSL ARM 库未找到 (third_party/openssl/lib/libssl.a)"
    echo " 请运行: bash scripts/build_openssl_arm.sh"
fi

# SparkChain SDK (TTS)
if [ "${ENABLE_SPARKCHAIN}" = "sdk" ]; then
    # ARM 版 libSparkChain.so 需手动放入 third_party/sparkchain/lib/
    SPARKCHAIN_LIB="${PROJECT_DIR}/third_party/sparkchain/lib/libSparkChain.so"
    if [ -f "${SPARKCHAIN_LIB}" ]; then
        LDFLAGS+=("${SPARKCHAIN_LIB}" -ldl)
        echo " SparkChain SDK: ${SPARKCHAIN_LIB}"
    else
        echo " 警告: ARM libSparkChain.so 未找到!"
        echo " 请将 ARM 版 libSparkChain.so 放入 third_party/sparkchain/lib/"
    fi
fi

if [ "${ENABLE_TFLITE}" = "tflite" ]; then
    echo " TFLite: 已启用（含所有依赖库）"

    # ALSA 库
    ASOUND_LIB=$(find "${ROOTFS}/lib" -name "libasound.so*" -type f 2>/dev/null | head -1)
    if [ -n "${ASOUND_LIB}" ]; then
        LDFLAGS+=("${ASOUND_LIB}")
        echo " ALSA: ${ASOUND_LIB}"
    fi

    # TFLite 静态库
    TFLITE_LIB="${PROJECT_DIR}/third_party/tflite/lib/libtensorflow-lite.a"
    if [ ! -f "${TFLITE_LIB}" ]; then
        echo " 错误: TFLite 库未找到，请先运行: bash scripts/build_tflite_arm.sh"
        exit 1
    fi
    LDFLAGS+=("${TFLITE_LIB}")

    # TFLite 依赖库
    TFLITE_DEPS="${PROJECT_DIR}/build/tflite_build/tflite_arm_build/_deps"
    LDFLAGS+=(-Wl,--whole-archive -Wl,--allow-multiple-definition)

    for a in "${TFLITE_DEPS}"/ruy-build/ruy/libruy_*.a; do
        [ -f "$a" ] && LDFLAGS+=("$a")
    done
    for a in "${TFLITE_DEPS}"/cpuinfo-build/libcpuinfo*.a; do
        [ -f "$a" ] && LDFLAGS+=("$a")
    done
    for a in "${TFLITE_DEPS}"/fft2d-build/libfft2d_*.a; do
        [ -f "$a" ] && LDFLAGS+=("$a")
    done
    for a in \
        "${TFLITE_DEPS}"/farmhash-build/libfarmhash.a \
        "${TFLITE_DEPS}"/gemmlowp-build/lib*.a \
        "${TFLITE_DEPS}"/flatbuffers-build/libflatbuffers.a; do
        for f in $a; do [ -f "$f" ] && LDFLAGS+=("$f"); done
    done

    while IFS= read -r -d '' a; do
        LDFLAGS+=("$a")
    done < <(find "${TFLITE_DEPS}/abseil-cpp-build" -name "*.a" -print0 2>/dev/null)

    PTHREADPOOL_LIB="${TFLITE_DEPS}/../pthreadpool/libpthreadpool.a"
    [ -f "$PTHREADPOOL_LIB" ] && LDFLAGS+=("$PTHREADPOOL_LIB")

    LDFLAGS+=(-Wl,--no-whole-archive)

    # 静态链接 C++ 标准库（gcc 11.4 版本，提供 CXX11 ABI）
    LDFLAGS+=("${GCC11_LIBDIR}/libstdc++.a")
    LDFLAGS+=("${GCC11_LIBDIR}/libgcc.a")
    # 末尾补 -lpthread，确保 OpenSSL 等静态库的 pthread 符号能解析
    LDFLAGS+=(-lpthread)

    echo " 静态 C++ 库: gcc 11.4"

else
    # 非 TFLite 模式
    if [ -d "${ROOTFS}" ]; then
        ASOUND_LIB="${ROOTFS}/lib/libasound.so.2.0.0"
        if [ -f "${ASOUND_LIB}" ]; then
            LDFLAGS+=("${ASOUND_LIB}")
        else
            ASOUND_SO=$(find "${ROOTFS}" -name "libasound.so*" 2>/dev/null | head -1)
            [ -n "${ASOUND_SO}" ] && LDFLAGS+=("${ASOUND_SO}") || LDFLAGS+=(-lasound)
        fi
    fi
fi

# ============================================
# kissfft
# ============================================
KISSFFT_DIR="${PROJECT_DIR}/third_party/kissfft"
if [ ! -d "${KISSFFT_DIR}" ]; then
    echo "下载 kissfft..."
    bash "${PROJECT_DIR}/scripts/download_kissfft.sh"
fi
echo " kissfft: ${KISSFFT_DIR}"

# ============================================
# 源文件列表
# ============================================
SRC_FILES=()
while IFS= read -r -d '' f; do
    # 非 TFLite 模式排除 kws_tflite.cpp
    if [ "${ENABLE_TFLITE}" != "tflite" ] && echo "$f" | grep -q "kws_tflite.cpp"; then
        continue
    fi
    SRC_FILES+=("$f")
done < <(find "${PROJECT_DIR}/src" -name "*.cpp" -print0 | sort -z)

# kissfft
SRC_FILES+=("${KISSFFT_DIR}/kiss_fft.c")
SRC_FILES+=("${KISSFFT_DIR}/kiss_fftr.c")

# TFLite 符号存根 + glibc 兼容
if [ "${ENABLE_TFLITE}" = "tflite" ]; then
    SRC_FILES+=("${PROJECT_DIR}/third_party/tflite/tflite_stubs.cc")
    SRC_FILES+=("${PROJECT_DIR}/third_party/tflite/glibc_compat.c")

    # __libc_single_threaded 存根（EGLIBC 2.19 不包含此符号）
    SRC_FILES+=("${PROJECT_DIR}/third_party/tflite/glibc_stubs.c")

    echo " tflite_stubs: 已添加"
    echo " glibc_compat: 已添加"
fi

# ============================================
# 编译
# ============================================
echo ""
echo "编译中..."
echo "  CXX: ${CXX}"
echo "  源文件: ${#SRC_FILES[@]}"
echo ""

"${CXX}" "${CXXFLAGS[@]}" "${SRC_FILES[@]}" -o "${BUILD_DIR}/ai_assistant" "${LDFLAGS[@]}"

echo ""
echo "编译完成!"
"${LINARO}/bin/arm-linux-gnueabihf-strip" "${BUILD_DIR}/ai_assistant" 2>/dev/null || true
echo ""
ls -lh "${BUILD_DIR}/ai_assistant"
file "${BUILD_DIR}/ai_assistant"

# GLIBC 版本检查
echo ""
echo "GLIBC 需求:"
arm-linux-gnueabihf-readelf -a "${BUILD_DIR}/ai_assistant" 2>/dev/null | grep "GLIBC_" | grep -v "^$" | sed 's/.*GLIBC_/GLIBC_/g' | sort -Vu | grep -v "2.4$"

echo ""
echo "=========================================="
echo " 部署到开发板:"
echo "   cp ${BUILD_DIR}/ai_assistant 到 board:/ai_assistant/"
echo "   注意: libstdc++.so.6 和 libgcc_s.so.1 也需要部署"
echo "=========================================="
