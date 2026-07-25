/*
 * kws_engine.h
 * KWS（Keyword Spotting）引擎抽象接口
 *
 * 定义统一的唤醒词检测接口，支持不同的后端实现：
 * - KWSTFLite: TFLite 模型推理（主方案）
 * - SimpleKWS: 简化版（备用）
 */

#ifndef AI_ASSISTANT_KWS_ENGINE_H
#define AI_ASSISTANT_KWS_ENGINE_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/* KWS 引擎抽象基类 */
class KWSEngine {
public:
    virtual ~KWSEngine() = default;

    /* 初始化引擎，加载模型 */
    virtual bool Initialize(const std::string& model_path, float threshold) = 0;

    /* 处理音频帧（通常 20ms），检测是否包含唤醒词 */
    virtual void ProcessAudio(const std::vector<int16_t>& audio_frame) = 0;

    /* 当前帧是否检测到唤醒词 */
    virtual bool IsWakeWordDetected() const = 0;

    /* 重置内部状态 */
    virtual void Reset() = 0;

    /* 设置调试模式 */
    virtual void SetDebugMode(bool enable) { (void)enable; }

    /* 唤醒词检测到时的回调 */
    using WakeWordCallback = std::function<void()>;
    void OnWakeWordDetected(WakeWordCallback cb) {
        wakeword_callback_ = std::move(cb);
    }

protected:
    WakeWordCallback wakeword_callback_;
};

#endif /* AI_ASSISTANT_KWS_ENGINE_H */
