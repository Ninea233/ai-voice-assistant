/*
 * kws_engine.cpp
 * KWS 引擎工厂函数
 *
 * 如果编译时启用了 TFLite 且模型文件存在，使用 KWSTFLite（高精度）；
 * 否则使用 SimpleKWS（能量检测后备方案）。
 */

#include "assistant/kws/kws_engine.h"
#include "assistant/kws/kws_simple.h"

#ifdef USE_TFLITE
#include "assistant/kws/kws_tflite.h"
#endif

#include <fstream>
#include <iostream>
#include <memory>

static const char* kTag = "[KWSEngine]";

/* 创建 KWS 引擎实例，根据模型文件是否存在自动选择 */
std::unique_ptr<KWSEngine> CreateKWSEngine(const std::string& model_path) {
#ifdef USE_TFLITE
    std::ifstream model_file(model_path);
    if (model_file.good()) {
        model_file.close();
        std::cout << kTag << " 检测到模型文件，使用 TFLite KWS 引擎: "
                  << model_path << std::endl;
        return std::make_unique<KWSTFLite>();
    }

    std::cout << kTag << " 未找到模型文件 (" << model_path
              << ")" << std::endl;
#else
    (void)model_path;
    std::cout << kTag << " 未启用 TFLite 编译" << std::endl;
#endif

    std::cout << kTag << " 使用能量检测后备方案（SimpleKWS）" << std::endl;
    return std::make_unique<SimpleKWS>();
}

/* 兼容旧版无参数调用 */
std::unique_ptr<KWSEngine> CreateKWSEngine() {
    return CreateKWSEngine("models/kws_model.tflite");
}
