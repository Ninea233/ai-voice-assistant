/*
 * vad.h
 * 语音活动检测（Voice Activity Detection）
 *
 * 基于能量阈值的 VAD 实现。
 * 每帧 20ms（320 samples @16kHz），计算 RMS 能量。
 * 连续静音帧达到 silence_timeout_ms 判定语音结束。
 */

#ifndef AI_ASSISTANT_VAD_H
#define AI_ASSISTANT_VAD_H

#include <cstdint>
#include <vector>

class VAD {
public:
    VAD();
    ~VAD() = default;

    /* 配置 VAD 参数 */
    void Configure(unsigned int sample_rate,
                   unsigned int silence_timeout_ms = 1500,
                   unsigned int min_speech_ms = 200,
                   float threshold = 500.0f,
                   bool adaptive = true);

    /* 处理一帧音频，返回该帧是否活动（有语音） */
    bool Process(const std::vector<int16_t>& frame);

    /* 是否检测到语音结束（静音超时） */
    bool IsSpeechEnd() const;

    /* 当前是否正在说话 */
    bool IsSpeaking() const { return in_speech_; }

    /* 重置 VAD 状态 */
    void Reset();

private:
    /* 计算 RMS 能量 */
    float CalculateEnergy(const std::vector<int16_t>& frame) const;

    unsigned int sample_rate_ = 16000;
    unsigned int silence_timeout_ms_ = 1500;
    unsigned int min_speech_ms_ = 200;
    float threshold_ = 500.0f;
    bool adaptive_ = true;

    /* 帧计数器（20ms/帧） */
    unsigned int frame_ms_ = 20;
    unsigned int silence_frames_ = 0;
    unsigned int speech_frames_ = 0;
    unsigned int silence_timeout_frames_ = 0;
    unsigned int min_speech_frames_ = 0;

    bool in_speech_ = false;
    bool speech_ended_ = false;
};

#endif /* AI_ASSISTANT_VAD_H */
