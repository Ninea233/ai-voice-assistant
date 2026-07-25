cmake_minimum_required(VERSION 3.10)
project(ai_assistant CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 交叉编译工具链
# 使用方式: cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake ..
# 或直接: cmake .. -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
#                -DCMAKE_CXX_COMPILER=arm-linux-gnueabihf-g++

# 选项
option(ENABLE_TFLITE "Enable TensorFlow Lite KWS" OFF)
option(ENABLE_TEST  "Build tests" ON)
option(ENABLE_SPARKCHAIN_SDK "Enable SparkChain SDK for TTS (needs ARM .so)" OFF)

# 包含路径
include_directories(${CMAKE_SOURCE_DIR}/include)

# OpenSSL（用于 HTTP/WebSocket TLS 加密）
if(CMAKE_CROSSCOMPILING)
    # 交叉编译：使用预编译的 ARM OpenSSL 静态库
    set(OPENSSL_DIR ${CMAKE_SOURCE_DIR}/third_party/openssl)
    if(EXISTS ${OPENSSL_DIR}/lib/libssl.a)
        include_directories(${OPENSSL_DIR}/include)
        set(OPENSSL_LIBS ${OPENSSL_DIR}/lib/libssl.a ${OPENSSL_DIR}/lib/libcrypto.a)
        message(STATUS "OpenSSL (ARM): ${OPENSSL_DIR}")
    endif()
else()
    # 本地编译（PC 测试）：使用系统 OpenSSL
    find_package(OpenSSL REQUIRED)
    set(OPENSSL_LIBS OpenSSL::SSL OpenSSL::Crypto)
endif()

# 查找 ALSA
find_library(ASOUND_LIB asound REQUIRED)

# kissfft 轻量 FFT 库（MFCC 特征提取必需）
set(KISSFFT_DIR ${CMAKE_SOURCE_DIR}/third_party/kissfft)
if(EXISTS ${KISSFFT_DIR}/kiss_fft.h)
    include_directories(${KISSFFT_DIR})
    message(STATUS "kissfft 已找到: ${KISSFFT_DIR}")
else()
    message(WARNING "kissfft 未找到！请运行: bash scripts/download_kissfft.sh")
endif()

# TFLite
if(ENABLE_TFLITE)
    set(TFLITE_DIR ${CMAKE_SOURCE_DIR}/third_party/tflite)
    if(EXISTS ${TFLITE_DIR}/lib/libtensorflow-lite.a AND EXISTS ${TFLITE_DIR}/include/tensorflow/lite/model.h)
        include_directories(${TFLITE_DIR}/include)
        set(TFLITE_LIB ${TFLITE_DIR}/lib/libtensorflow-lite.a)
        add_definitions(-DUSE_TFLITE)
        message(STATUS "TFLite 已启用: ${TFLITE_DIR}")
    else()
        message(WARNING "TFLite 库未找到！请先编译 TFLite: bash scripts/build_tflite_arm.sh")
        message(WARNING "将使用 SimpleKWS（能量检测）后备方案")
    endif()
endif()

# SparkChain SDK（用于 TTS，编译时启用）
if(ENABLE_SPARKCHAIN_SDK)
    # SDK 路径（相对于项目根目录）
    set(SPARKCHAIN_DIR ${CMAKE_SOURCE_DIR}/../sparkchain_sdk)
    if(EXISTS ${SPARKCHAIN_DIR}/include/sparkchain.h)
        include_directories(${SPARKCHAIN_DIR}/include)
        add_definitions(-DENABLE_SPARKCHAIN_SDK)

        if(CMAKE_CROSSCOMPILING)
            # 交叉编译：ARM 版 .so 需手动放入 third_party/sparkchain/lib/
            set(SPARKCHAIN_LIB_DIR ${CMAKE_SOURCE_DIR}/third_party/sparkchain/lib)
            find_library(SPARKCHAIN_LIB SparkChain PATHS ${SPARKCHAIN_LIB_DIR})
            if(SPARKCHAIN_LIB)
                message(STATUS "SparkChain SDK (ARM): ${SPARKCHAIN_LIB}")
            else()
                message(WARNING "ARM libSparkChain.so 未找到！请放入 third_party/sparkchain/lib/")
            endif()
        else()
            # 本地编译（PC 测试）：使用 SDK 自带的 x86-64 .so
            find_library(SPARKCHAIN_LIB SparkChain PATHS ${SPARKCHAIN_DIR}/libs)
            if(SPARKCHAIN_LIB)
                message(STATUS "SparkChain SDK (x86-64): ${SPARKCHAIN_DIR}")
            endif()
        endif()
    else()
        message(WARNING "SparkChain SDK 未找到: ${SPARKCHAIN_DIR}")
    endif()
endif()

# 源文件列表
set(CORE_SRC
    src/core/assistant.cpp
    src/core/state_machine.cpp
    src/core/config.cpp
)

set(AUDIO_SRC
    src/audio/audio_capture.cpp
    src/audio/audio_playback.cpp
    src/audio/vad.cpp
)

# kissfft 源文件
set(KISSFFT_SRC
    ${KISSFFT_DIR}/kiss_fft.c
    ${KISSFFT_DIR}/kiss_fftr.c
)

# kws_engine.cpp 始终编译（工厂函数自动选择引擎实现）
set(KWS_SRC
    src/kws/kws_engine.cpp
    src/kws/kws_simple.cpp
    ${KISSFFT_SRC}
)

# kws_tflite.cpp + ruy 符号存根，仅在启用 TFLite 时编译
if(ENABLE_TFLITE)
    list(APPEND KWS_SRC src/kws/kws_tflite.cpp)
    list(APPEND KWS_SRC ${TFLITE_DIR}/tflite_stubs.cc)
endif()

set(CLOUD_SRC
    src/cloud/asr_client.cpp
    src/cloud/llm_client.cpp
    src/cloud/tts_client.cpp
    src/cloud/net_socket.cpp
    src/cloud/http_client.cpp
    src/cloud/ws_client.cpp
    src/cloud/xfyun_auth.cpp
)

set(COMMAND_SRC
    src/command/command_manager.cpp
    src/command/ir_controller.cpp
)

set(AGENT_SRC
    src/agent/agent_core.cpp
    src/agent/skill_manager.cpp
    src/agent/mcp_tools.cpp
)

set(SENSITIVE_SRC
    src/sensitive/sensitive_filter.cpp
)

# 主程序
add_executable(ai_assistant
    src/main.cpp
    ${CORE_SRC}
    ${AUDIO_SRC}
    ${KWS_SRC}
    ${CLOUD_SRC}
    ${COMMAND_SRC}
    ${AGENT_SRC}
    ${SENSITIVE_SRC}
)

target_link_libraries(ai_assistant
    ${ASOUND_LIB}
    ${TFLITE_LIB}
    ${OPENSSL_LIBS}
    pthread
    dl
    m
)

# SparkChain SDK (TTS)
if(ENABLE_SPARKCHAIN_SDK AND SPARKCHAIN_LIB)
    target_link_libraries(ai_assistant ${SPARKCHAIN_LIB})
endif()

# 测试程序
if(ENABLE_TEST)
    enable_testing()

    function(add_assistant_test TEST_NAME TEST_SRC)
        add_executable(${TEST_NAME} ${TEST_SRC} ${CORE_SRC} ${AUDIO_SRC}
                       ${KWS_SRC} ${CLOUD_SRC} ${COMMAND_SRC}
                       ${SENSITIVE_SRC})
        target_link_libraries(${TEST_NAME} ${ASOUND_LIB} ${TFLITE_LIB} ${OPENSSL_LIBS} pthread dl m)
    endfunction()
    add_assistant_test(test_state_machine    tests/test_state_machine.cpp)
    add_assistant_test(test_vad              tests/test_vad.cpp)
    add_assistant_test(test_command_manager  tests/test_command_manager.cpp)
    add_assistant_test(test_sensitive_filter tests/test_sensitive_filter.cpp)
endif()
