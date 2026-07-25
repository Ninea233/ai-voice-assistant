/*
 * kws_simple.cpp
 * 简化 KWS 引擎实现（能量检测后备方案）
 */

#include "assistant/kws/kws_simple.h"
#include <cmath>
#include <iostream>

static const char* kTag = "[SimpleKWS]";

SimpleKWS::SimpleKWS() = default;

bool SimpleKWS::Initialize(const std::string& model_path, float threshold) {
    (void)model_path;  /* SimpleKWS 不需要模型文件 */
    threshold_ = threshold;
    std::cout << kTag << " 初始化（能量检测模式），阈值=" << threshold_ << std::endl;
    return true;
}

void SimpleKWS::ProcessAudio(const std::vector<int16_t>& audio_frame) {
    if (audio_frame.empty()) return;

    /* 计算 RMS 能量 */
    double sum_sq = 0.0;
    for (auto sample : audio_frame) {
        sum_sq += static_cast<double>(sample) * sample;
    }
    float rms = std::sqrt(static_cast<float>(sum_sq / audio_frame.size()));

    if (rms > threshold_) {
        trigger_counter_++;
        if (trigger_counter_ >= kTriggerCount) {
            wakeword_detected_ = true;
            if (wakeword_callback_) {
                wakeword_callback_();
            }
        }
    } else {
        if (trigger_counter_ > 0) {
            trigger_counter_--;  /* 缓慢衰减，避免误触发 */
        }
    }
}

bool SimpleKWS::IsWakeWordDetected() const {
    return wakeword_detected_;
}

void SimpleKWS::Reset() {
    wakeword_detected_ = false;
    trigger_counter_ = 0;
}
