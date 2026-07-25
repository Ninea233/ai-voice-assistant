#!/bin/bash
#===========================================
# build_tflite_arm_offline.sh
# 离线编译 TFLite for ARM（跳过网络下载）
#
# 原理：修改 TFLite 的 cmake 模块文件，
# 将 FetchContent 下载替换为本地路径引用。
#===========================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TFLITE_DIR="${PROJECT_DIR}/third_party/tflite"
TFLITE_SRC="${PROJECT_DIR}/build/tflite_build/tensorflow_src"
TFLITE_BUILD="${PROJECT_DIR}/build/tflite_build/tflite_arm_build"
DEPS_DIR="${PROJECT_DIR}/third_party/tflite_deps"

CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf}"
JOBS=${JOBS:-$(nproc)}

echo "=========================================="
echo " TFLite ARM 离线编译"
echo " 编译器: ${CROSS_COMPILE}"
echo " 依赖: ${DEPS_DIR}"
echo "=========================================="

# ====== 1. 检查依赖 ======
echo ""
echo "[1/5] 检查依赖..."
for dep in abseil-cpp flatbuffers eigen farmhash ruy; do
    if [ -d "${DEPS_DIR}/${dep}" ]; then
        echo "  ✅ ${dep}"
    else
        echo "  ❌ ${dep} (缺失)"
        exit 1
    fi
done

# ====== 2. 备份并修改 cmake 模块 ======
echo ""
echo "[2/5] 配置 cmake 模块使用本地源码..."

# 需要修改的模块列表（核心依赖）
MODULES="abseil-cpp flatbuffers eigen farmhash ruy"

for module in abseil-cpp flatbuffers eigen farmhash ruy; do
    cmake_file="${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/${module}.cmake"

    # 备份
    if [ ! -f "${cmake_file}.bak" ]; then
        cp "${cmake_file}" "${cmake_file}.bak"
    fi

    echo "  修改: ${module}.cmake"
done

# --- abseil-cpp.cmake ---
cat > "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/abseil-cpp.cmake" << 'ABSEIL'
if(TARGET absl_base)
  return()
endif()
add_subdirectory(
  "${DEPS_DIR}/abseil-cpp"
  "${CMAKE_BINARY_DIR}/abseil-cpp"
  EXCLUDE_FROM_ALL
)
ABSEIL
sed -i "s|\${DEPS_DIR}|${DEPS_DIR}|g" "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/abseil-cpp.cmake"
# 禁用 abseil 测试
echo 'set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)' >> "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/abseil-cpp.cmake"
echo 'set(ABSL_USE_EXTERNAL_GOOGLETEST OFF CACHE BOOL "" FORCE)' >> "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/abseil-cpp.cmake"
echo 'set(ABSL_FIND_GOOGLETEST OFF CACHE BOOL "" FORCE)' >> "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/abseil-cpp.cmake"
echo "  ✅ abseil-cpp.cmake"

# --- flatbuffers.cmake ---
cat > "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/flatbuffers.cmake" << 'FLAT'
if(TARGET flatbuffers)
  return()
endif()
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(
  "${DEPS_DIR}/flatbuffers"
  "${CMAKE_BINARY_DIR}/flatbuffers"
  EXCLUDE_FROM_ALL
)
FLAT
sed -i "s|\${DEPS_DIR}|${DEPS_DIR}|g" "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/flatbuffers.cmake"
echo "  ✅ flatbuffers.cmake"

# --- eigen.cmake ---
cat > "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/eigen.cmake" << 'EIGEN'
if(TARGET eigen)
  return()
endif()
# Eigen 是 header-only 库，只需设置包含路径
set(EIGEN3_INCLUDE_DIR "${DEPS_DIR}/eigen" CACHE PATH "Eigen include")
add_library(eigen INTERFACE)
target_include_directories(eigen INTERFACE "${DEPS_DIR}/eigen")
EIGEN
sed -i "s|\${DEPS_DIR}|${DEPS_DIR}|g" "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/eigen.cmake"
echo "  ✅ eigen.cmake"

# --- farmhash.cmake ---
cat > "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/farmhash.cmake" << 'FARM'
if(TARGET farmhash)
  return()
endif()
add_subdirectory(
  "${DEPS_DIR}/farmhash/src"
  "${CMAKE_BINARY_DIR}/farmhash"
  EXCLUDE_FROM_ALL
)
FARM
sed -i "s|\${DEPS_DIR}|${DEPS_DIR}|g" "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/farmhash.cmake"
echo "  ✅ farmhash.cmake"

# --- ruy.cmake ---
cat > "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/ruy.cmake" << 'RUY'
if(TARGET ruy)
  return()
endif()
set(RUY_MINIMUM_SIZE 64 CACHE STRING "" FORCE)
add_subdirectory(
  "${DEPS_DIR}/ruy"
  "${CMAKE_BINARY_DIR}/ruy"
  EXCLUDE_FROM_ALL
)
RUY
sed -i "s|\${DEPS_DIR}|${DEPS_DIR}|g" "${TFLITE_SRC}/tensorflow/lite/tools/cmake/modules/ruy.cmake"
echo "  ✅ ruy.cmake"

# ====== 3. 创建工具链文件 ======
echo ""
echo "[3/5] 创建 ARM 工具链文件..."
TOOLCHAIN_FILE="${PROJECT_DIR}/build/tflite_build/arm_toolchain.cmake"
mkdir -p "$(dirname "${TOOLCHAIN_FILE}")"

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

# ====== 4. CMake 配置 ======
echo ""
echo "[4/5] CMake 配置..."
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
    -DTFLITE_ENABLE_METAL=OFF \
    -DTFLITE_ENABLE_RUY=ON \
    -DFETCHCONTENT_FULLY_DISCONNECTED=OFF \
    2>&1 | tail -40

if [ $? -ne 0 ]; then
    echo ""
    echo "CMake 配置失败。检查日志..."
    echo "=== CMakeError.log ==="
    cat "${TFLITE_BUILD}/CMakeFiles/CMakeError.log" 2>/dev/null | tail -20 || true
    exit 1
fi

# ====== 5. 编译 ======
echo ""
echo "[5/5] 编译 TFLite ARM 静态库..."
cmake --build . -j"${JOBS}" 2>&1 | tail -30

if [ $? -ne 0 ]; then
    echo "编译失败！查看详细输出..."
    exit 1
fi

# ====== 复制产物 ======
echo ""
echo "复制产物..."
mkdir -p "${TFLITE_DIR}/lib" "${TFLITE_DIR}/include"

# 静态库
if [ -f "${TFLITE_BUILD}/libtensorflow-lite.a" ]; then
    cp "${TFLITE_BUILD}/libtensorflow-lite.a" "${TFLITE_DIR}/lib/"
    echo "  ✅ 静态库: $(ls -lh ${TFLITE_DIR}/lib/libtensorflow-lite.a)"
else
    echo "  ❌ 未找到 libtensorflow-lite.a"
    find "${TFLITE_BUILD}" -name "*.a" 2>/dev/null
    exit 1
fi

# 头文件
echo "复制头文件..."
cd "${TFLITE_SRC}"
find "tensorflow/lite" -name "*.h" | while IFS= read -r h; do
    mkdir -p "${TFLITE_DIR}/include/$(dirname ${h})"
    cp "${h}" "${TFLITE_DIR}/include/${h}"
done 2>/dev/null

# flatbuffers
if [ -d "${DEPS_DIR}/flatbuffers/include/flatbuffers" ]; then
    cp -r "${DEPS_DIR}/flatbuffers/include/flatbuffers" "${TFLITE_DIR}/include/"
fi

# abseil
if [ -d "${DEPS_DIR}/abseil-cpp/absl" ]; then
    mkdir -p "${TFLITE_DIR}/include/absl"
    cp -r "${DEPS_DIR}/abseil-cpp/absl/." "${TFLITE_DIR}/include/absl/"
fi

# eigen
if [ -d "${DEPS_DIR}/eigen/Eigen" ]; then
    cp -r "${DEPS_DIR}/eigen/Eigen" "${TFLITE_DIR}/include/"
fi

# farmhash
if [ -f "${DEPS_DIR}/farmhash/src/farmhash.h" ]; then
    cp "${DEPS_DIR}/farmhash/src/farmhash.h" "${TFLITE_DIR}/include/"
fi

# ruy
if [ -d "${DEPS_DIR}/ruy" ]; then
    find "${DEPS_DIR}/ruy" -name "*.h" 2>/dev/null | while IFS= read -r h; do
        rel="${h#${DEPS_DIR}/ruy/}"
        mkdir -p "${TFLITE_DIR}/include/$(dirname ${rel})"
        cp "${h}" "${TFLITE_DIR}/include/${rel}"
    done 2>/dev/null || true
fi

echo ""
echo "=========================================="
echo " TFLite ARM 离线编译完成！"
echo " 库: ${TFLITE_DIR}/lib/libtensorflow-lite.a"
echo " 头文件: ${TFLITE_DIR}/include/"
ls -lh "${TFLITE_DIR}/lib/libtensorflow-lite.a"
echo "=========================================="
