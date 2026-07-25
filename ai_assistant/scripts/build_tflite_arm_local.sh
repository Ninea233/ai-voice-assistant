#!/bin/bash
#===========================================
# build_tflite_arm_local.sh
# 交叉编译 TFLite for ARM — 使用本地依赖源码
#
# 前提:
#   1. TFLite v2.14.0 源码已克隆到 build/tflite_build/tensorflow_src
#   2. 依赖库已下载到 Of_TFlite/ 目录
#
# 使用方式:
#   ./scripts/build_tflite_arm_local.sh
#===========================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TFLITE_DIR="${PROJECT_DIR}/third_party/tflite"
TFLITE_SRC="${PROJECT_DIR}/build/tflite_build/tensorflow_src"
TFLITE_BUILD="${PROJECT_DIR}/build/tflite_build/tflite_arm_build"
DEPS_DIR="${PROJECT_DIR}/Of_TFlite"

# 交叉编译器
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf}"
JOBS=${JOBS:-$(nproc)}

echo "=========================================="
echo " TFLite ARM 交叉编译 (本地依赖)"
echo " 编译器: ${CROSS_COMPILE}"
echo " 依赖目录: ${DEPS_DIR}"
echo "=========================================="

# 验证依赖是否存在
echo ""
echo "[检查] 验证本地依赖..."
DEPS_OK=true
for dep in abseil-cpp flatbuffers eigen farmhash ruy; do
    if [ -d "${DEPS_DIR}/${dep}" ]; then
        echo "  ✅ ${dep}"
    else
        echo "  ❌ ${dep} 未找到!"
        DEPS_OK=false
    fi
done

if [ "$DEPS_OK" = false ]; then
    echo "请先下载所有依赖到 ${DEPS_DIR}"
    exit 1
fi

# ====== 修改 TFLite 的 cmake 模块，指向本地源码 ======
echo ""
echo "[1/4] 配置 cmake 模块使用本地依赖..."

# 备份原始文件（只备份一次）
for module in abseil-cpp flatbuffers eigen farmhash cpuinfo ml_dtypes ruy fp16_headers gemmlowp; do
    cmake_file="${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/${module}.cmake"
    if [ -f "${cmake_file}" ] && [ ! -f "${cmake_file}.bak" ]; then
        cp "${cmake_file}" "${cmake_file}.bak"
        echo "  备份: ${module}.cmake → ${module}.cmake.bak"
    fi
done

# 创建统一的模块覆盖脚本
# 这些 .cmake 文件会让 TFLite 使用本地源码而非 GitHub
for dep in abseil-cpp flatbuffers ruy; do
    cmake_file="${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/${dep}.cmake"
    dep_path="${DEPS_DIR}/${dep}"
    cat > "${cmake_file}" << EOFCMAK
# 本地依赖覆盖: ${dep} → ${dep_path}
# 原始文件备份在 ${dep}.cmake.bak

if(TARGET $(echo "$dep" | sed 's/-/_/g')_local)
  return()
endif()

# 直接使用本地源码
if(CMAKE_CROSSCOMPILING)
  set(_absl_local_src_dir "${dep_path}")
endif()

add_subdirectory(
  "${dep_path}"
  "\${CMAKE_BINARY_DIR}/$(echo "$dep" | tr '-' '_')"
  EXCLUDE_FROM_ALL
)
EOFCMAK
    echo "  ✅ ${dep}.cmake → 本地路径: ${dep_path}"
done

# 对于 eigen（GitLab），它可能使用不同的 cmake 结构
cat > "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/eigen.cmake" << 'EOFCMAK'
# 本地依赖覆盖: eigen → 使用本地源码
if(TARGET eigen)
  return()
endif()
set(EIGEN3_INCLUDE_DIR "${DEPS_DIR}/eigen" CACHE PATH "Eigen include directory")
EOFCMAK
# 需要替换路径变量
sed -i "s|\${DEPS_DIR}|${DEPS_DIR}|g" "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/eigen.cmake"
echo "  ✅ eigen.cmake → 本地路径: ${DEPS_DIR}/eigen"

# farmhash 类似
cat > "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/farmhash.cmake" << 'EOFCMAK'
# 本地依赖覆盖: farmhash
if(TARGET farmhash)
  return()
endif()
add_subdirectory(
  "${DEPS_DIR}/farmhash/src"
  "${CMAKE_BINARY_DIR}/farmhash"
  EXCLUDE_FROM_ALL
)
EOFCMAK
sed -i "s|\${DEPS_DIR}|${DEPS_DIR}|g" "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/farmhash.cmake"
echo "  ✅ farmhash.cmake → 本地路径: ${DEPS_DIR}/farmhash"

# ====== CMake 配置 ======
echo ""
echo "[2/4] CMake 配置..."
rm -rf "${TFLITE_BUILD}"
mkdir -p "${TFLITE_BUILD}"
cd "${TFLITE_BUILD}"

# 创建工具链文件
TOOLCHAIN_FILE="${PROJECT_DIR}/build/tflite_build/arm_toolchain.cmake"
cat > "${TOOLCHAIN_FILE}" << 'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_AR arm-linux-gnueabihf-ar)
set(CMAKE_RANLIB arm-linux-gnueabihf-ranlib)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv7-a -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv7-a -mfpu=neon -mfloat-abi=hard")
EOF

cmake "${TFLITE_SRC}/tensorflow/lite" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTFLITE_ENABLE_XNNPACK=OFF \
    -DTFLITE_ENABLE_GPU=OFF \
    -DTFLITE_ENABLE_RPC=OFF \
    -DTFLITE_ENABLE_NNAPI=OFF \
    -DTFLITE_ENABLE_MMAP=OFF \
    -DTFLITE_ENABLE_METAL=OFF \
    -DTFLITE_ENABLE_RUY=OFF \
    2>&1 | tail -30

# 如果配置失败，尝试更简化的配置
if [ $? -ne 0 ]; then
    echo ""
    echo "CMake 配置失败，尝试使用默认设置..."
    echo "检查 CMakeError.log..."
    cat "${TFLITE_BUILD}/CMakeFiles/CMakeError.log" 2>/dev/null | tail -30 || true
    exit 1
fi

# ====== 编译 ======
echo ""
echo "[3/4] 编译 TFLite ARM 静态库..."
cmake --build . -j"${JOBS}" 2>&1 | tail -30

if [ $? -ne 0 ]; then
    echo "编译失败！"
    exit 1
fi

# ====== 复制产物 ======
echo ""
echo "[4/4] 复制编译产物..."
mkdir -p "${TFLITE_DIR}/lib" "${TFLITE_DIR}/include"

# 静态库
if [ -f "${TFLITE_BUILD}/libtensorflow-lite.a" ]; then
    cp "${TFLITE_BUILD}/libtensorflow-lite.a" "${TFLITE_DIR}/lib/"
    echo "  ✅ 静态库: $(ls -lh ${TFLITE_DIR}/lib/libtensorflow-lite.a)"
else
    echo "  ❌ libtensorflow-lite.a 未生成"
    find "${TFLITE_BUILD}" -name "*.a" 2>/dev/null
    exit 1
fi

# 头文件（从源码目录复制）
cd "${TFLITE_SRC}"
find "tensorflow/lite" -name "*.h" | while IFS= read -r header; do
    mkdir -p "${TFLITE_DIR}/include/$(dirname ${header})"
    cp "${header}" "${TFLITE_DIR}/include/${header}"
done
echo "  ✅ TFLite 头文件"

# flatbuffers 头文件
if [ -d "${DEPS_DIR}/flatbuffers/include" ]; then
    cp -r "${DEPS_DIR}/flatbuffers/include/flatbuffers" "${TFLITE_DIR}/include/"
    echo "  ✅ flatbuffers 头文件"
fi

# abseil 头文件
if [ -d "${DEPS_DIR}/abseil-cpp/absl" ]; then
    mkdir -p "${TFLITE_DIR}/include/absl"
    cp -r "${DEPS_DIR}/abseil-cpp/absl/." "${TFLITE_DIR}/include/absl/"
    echo "  ✅ abseil 头文件"
fi

# farmhash 头文件
if [ -f "${DEPS_DIR}/farmhash/src/farmhash.h" ]; then
    cp "${DEPS_DIR}/farmhash/src/farmhash.h" "${TFLITE_DIR}/include/"
    echo "  ✅ farmhash 头文件"
fi

# eigen 头文件 - 只需 Eigen 目录
if [ -d "${DEPS_DIR}/eigen/Eigen" ]; then
    cp -r "${DEPS_DIR}/eigen/Eigen" "${TFLITE_DIR}/include/"
    echo "  ✅ Eigen 头文件"
fi

# ruy 头文件
if [ -d "${DEPS_DIR}/ruy" ]; then
    find "${DEPS_DIR}/ruy" -name "*.h" -path "*/ruy/*" 2>/dev/null | while IFS= read -r h; do
        rel_path="${h#${DEPS_DIR}/ruy/}"
        mkdir -p "${TFLITE_DIR}/include/$(dirname ${rel_path})"
        cp "${h}" "${TFLITE_DIR}/include/${rel_path}"
    done 2>/dev/null || true
    echo "  ✅ ruy 头文件"
fi

echo ""
echo "=========================================="
echo " TFLite ARM 交叉编译完成!"
echo " 头文件: ${TFLITE_DIR}/include/tensorflow/lite/"
echo " 静态库: ${TFLITE_DIR}/lib/libtensorflow-lite.a"
echo "=========================================="
