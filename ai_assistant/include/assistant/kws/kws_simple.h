/*
 * kws_simple.h
 * 简化 KWS 引擎 — 基于能量检测的唤醒（无 TFLite 依赖）
 *
 * 当 TFLite 模型不可用时作为后备方案。
 * 原理：检测短时 RMS 能量是否超过高阈值，连续触发多次视为唤醒。
 *
 * 注意：这是后备方案，准确率远低于 TFLite 模型唤醒。
 * 后续应在网络可用时交叉编译 TFLite 并替换。
 */

#ifndef AI_ASSISTANT_KWS_SIMPLE_H
#define AI_ASSISTANT_KWS_SIMPLE_H

#include "assistant/kws/kws_engine.h"
#include <cstdint>
#include <vector>

class SimpleKWS : public KWSEngine {
public:
    SimpleKWS();
    ~SimpleKWS() override = default;

    bool Initialize(const std::string& model_path, float threshold) override;
    void ProcessAudio(const std::vector<int16_t>& audio_frame) override;
    bool IsWakeWordDetected() const override;
    void Reset() override;

private:
    float threshold_ = 20000.0f;   /* 能量阈值（RMS，默认较高） */
    bool wakeword_detected_ = false;
    int trigger_counter_ = 0;
    static const int kTriggerCount = 5; /* 连续 5 帧超阈值才触发 */
};

#endif /* AI_ASSISTANT_KWS_SIMPLE_H */
