#!/bin/bash
#===========================================
# build_tflite_arm_direct.sh
# 直接编译 TFLite for ARM（跳过 CMake FetchContent）
#
# 手工编译 TFLite 核心源文件为静态库，
# 使用本地下载的依赖头文件。
#===========================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TFLITE_DIR="${PROJECT_DIR}/third_party/tflite"
TFLITE_SRC="${PROJECT_DIR}/build/tflite_build/tensorflow_src"
DEPS_DIR="${PROJECT_DIR}/Of_TFlite"
BUILD_DIR="${PROJECT_DIR}/build/tflite_direct_build"

CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf}"
JOBS=${JOBS:-$(nproc)}

echo "=========================================="
echo " TFLite for ARM - 直接编译"
echo " 编译器: ${CROSS_COMPILE}"
echo " 线程: ${JOBS}"
echo "=========================================="

# ==================== 1. 依赖检查 ====================
echo ""
echo "[1/5] 检查依赖..."
for dep in abseil-cpp flatbuffers eigen farmhash ruy; do
    [ -d "${DEPS_DIR}/${dep}" ] && echo "  ✅ ${dep}" || { echo "  ❌ ${dep}"; exit 1; }
done

# ==================== 2. 创建构建目录 ====================
echo ""
echo "[2/5] 创建构建目录..."
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}/obj"
mkdir -p "${TFLITE_DIR}/lib"
mkdir -p "${TFLITE_DIR}/include"

# ==================== 3. 设置编译参数 ====================
echo ""
echo "[3/5] 设置编译参数..."

CXX="${CROSS_COMPILE}-g++"
AR="${CROSS_COMPILE}-ar"

CXXFLAGS="-std=c++14 -fPIC -O3 -DNDEBUG"
CXXFLAGS+=" -march=armv7-a -mfpu=neon -mfloat-abi=hard"
CXXFLAGS+=" -DTFLITE_WITH_RUY"  # 使用 ruy 优化矩阵运算
CXXFLAGS+=" -w"  # 减少警告输出

# 包含路径
INCLUDES=""
INCLUDES+=" -I${TFLITE_SRC}"
INCLUDES+=" -I${DEPS_DIR}/abseil-cpp"
INCLUDES+=" -I${DEPS_DIR}/flatbuffers/include"
INCLUDES+=" -I${DEPS_DIR}/eigen"
INCLUDES+=" -I${DEPS_DIR}/farmhash/src"
INCLUDES+=" -I${DEPS_DIR}/ruy"

CXXFLAGS+="${INCLUDES}"

# ==================== 4. 编译源文件 ====================
echo ""
echo "[4/5] 编译 TFLite 源文件..."

cd "${TFLITE_SRC}"

# 定义需要编译的源文件目录
SOURCE_DIRS=(
    "tensorflow/lite"
    "tensorflow/lite/core"
    "tensorflow/lite/core/acceleration"
    "tensorflow/lite/core/api"
    "tensorflow/lite/core/async"
    "tensorflow/lite/core/c"
    "tensorflow/lite/core/subgraph"
    "tensorflow/lite/kernels"
    "tensorflow/lite/kernels/internal"
    "tensorflow/lite/kernels/internal/optimized"
    "tensorflow/lite/kernels/internal/reference"
    "tensorflow/lite/profiling"
    "tensorflow/lite/profiling/root_error_basic"
    "tensorflow/lite/schema"
)

# 收集所有需要编译的 .cc 文件（排除测试文件 *_test.cc *_test_main.cc）
CC_FILES=""
for dir in "${SOURCE_DIRS[@]}"; do
    if [ -d "${dir}" ]; then
        files=$(find "${dir}" -maxdepth 1 -name "*.cc" ! -name "*_test*" ! -name "*_benchmark*" 2>/dev/null | sort)
        CC_FILES="${CC_FILES} ${files}"
    fi
done

# 排除特定文件（有编译问题的）
EXCLUDE_FILES=(
    "tensorflow/lite/toco_logging.cc"
    "tensorflow/lite/python"
)
for exclude in "${EXCLUDE_FILES[@]}"; do
    CC_FILES=$(echo "${CC_FILES}" | tr ' ' '\n' | grep -v "${exclude}" | tr '\n' ' ')
done

echo "  源文件总数: $(echo ${CC_FILES} | wc -w) 个"

# 编译每个 .cc 文件为 .o
OBJ_FILES=""
count=0
total=$(echo ${CC_FILES} | wc -w)

for cc_file in ${CC_FILES}; do
    count=$((count + 1))
    # 生成 .o 文件路径
    obj_name=$(echo "${cc_file}" | tr '/' '_' | sed 's/\.cc$/.o/')
    obj_path="${BUILD_DIR}/obj/${obj_name}"
    OBJ_FILES="${OBJ_FILES} ${obj_path}"

    # 显示进度（每 20 个文件显示一次）
    if [ $((count % 20)) -eq 0 ] || [ "${count}" -eq 1 ] || [ "${count}" -eq "${total}" ]; then
        echo "  编译进度: ${count}/${total}"
    fi

    # 编译
    ${CXX} ${CXXFLAGS} -c "${cc_file}" -o "${obj_path}" 2>/dev/null
done

echo "  全部 ${count} 个文件编译完成"

# ==================== 5. 创建静态库 ====================
echo ""
echo "[5/5] 创建静态库..."

cd "${BUILD_DIR}"
${AR} rcs "${BUILD_DIR}/libtensorflow-lite.a" ${OBJ_FILES} 2>/dev/null

# 复制到目标目录
cp "${BUILD_DIR}/libtensorflow-lite.a" "${TFLITE_DIR}/lib/"
echo "  ✅ 静态库: $(ls -lh ${TFLITE_DIR}/lib/libtensorflow-lite.a)"

# 复制 TFLite 头文件
cd "${TFLITE_SRC}"
find "tensorflow/lite" -name "*.h" ! -path "*test*" ! -path "*examples*" | while IFS= read -r header; do
    mkdir -p "${TFLITE_DIR}/include/$(dirname ${header})"
    cp "${header}" "${TFLITE_DIR}/include/${header}"
done 2>/dev/null
echo "  ✅ TFLite 头文件"

# 复制依赖头文件
cp -r "${DEPS_DIR}/flatbuffers/include/flatbuffers" "${TFLITE_DIR}/include/" 2>/dev/null && echo "  ✅ flatbuffers 头文件"
mkdir -p "${TFLITE_DIR}/include/absl" && cp -r "${DEPS_DIR}/abseil-cpp/absl/." "${TFLITE_DIR}/include/absl/" 2>/dev/null && echo "  ✅ abseil 头文件"
cp -r "${DEPS_DIR}/eigen/Eigen" "${TFLITE_DIR}/include/" 2>/dev/null && echo "  ✅ Eigen 头文件"
[ -f "${DEPS_DIR}/farmhash/src/farmhash.h" ] && cp "${DEPS_DIR}/farmhash/src/farmhash.h" "${TFLITE_DIR}/include/" 2>/dev/null && echo "  ✅ farmhash 头文件"

# ruy 头文件
if [ -d "${DEPS_DIR}/ruy" ]; then
    find "${DEPS_DIR}/ruy" -name "*.h" 2>/dev/null | while IFS= read -r h; do
        rel_path="${h#${DEPS_DIR}/ruy/}"
        mkdir -p "${TFLITE_DIR}/include/$(dirname ${rel_path})"
        cp "${h}" "${TFLITE_DIR}/include/${rel_path}"
    done 2>/dev/null || true
    echo "  ✅ ruy 头文件"
fi

echo ""
echo "=========================================="
echo " TFLite ARM 直接编译完成!"
echo " 头文件: ${TFLITE_DIR}/include/tensorflow/lite/"
echo " 静态库: ${TFLITE_DIR}/lib/libtensorflow-lite.a"
echo " 大小:"
ls -lh "${TFLITE_DIR}/lib/libtensorflow-lite.a"
echo "=========================================="
