/*
 * vad.cpp
 * 语音活动检测实现
 */

#include "assistant/audio/vad.h"
#include <cmath>
#include <algorithm>

VAD::VAD() = default;

void VAD::Configure(unsigned int sample_rate,
                    unsigned int silence_timeout_ms,
                    unsigned int min_speech_ms,
                    float threshold,
                    bool adaptive) {
    sample_rate_ = sample_rate;
    silence_timeout_ms_ = silence_timeout_ms;
    min_speech_ms_ = min_speech_ms;
    threshold_ = threshold;
    adaptive_ = adaptive;

    frame_ms_ = 20; /* 320 samples @16kHz */
    silence_timeout_frames_ = silence_timeout_ms_ / frame_ms_;
    min_speech_frames_      = min_speech_ms_ / frame_ms_;
}

float VAD::CalculateEnergy(const std::vector<int16_t>& frame) const {
    if (frame.empty()) return 0.0f;

    double sum_sq = 0.0;
    for (auto sample : frame) {
        sum_sq += static_cast<double>(sample) * sample;
    }
    return std::sqrt(static_cast<float>(sum_sq / frame.size()));
}

bool VAD::Process(const std::vector<int16_t>& frame) {
    float energy = CalculateEnergy(frame);
    bool is_active = (energy > threshold_);

    if (adaptive_) {
        /* 自适应阈值：跟随背景噪声缓慢变化 */
        float alpha = 0.99f;
        threshold_ = alpha * threshold_ + (1.0f - alpha) * energy * 1.5f;
        threshold_ = std::max(threshold_, 50.0f); /* 最小阈值保护 */
    }

    if (is_active) {
        speech_frames_++;
        silence_frames_ = 0;

        if (!in_speech_ && speech_frames_ >= min_speech_frames_) {
            in_speech_ = true;  /* 确认进入语音 */
        }
    } else {
        if (in_speech_) {
            silence_frames_++;
            if (silence_frames_ >= silence_timeout_frames_) {
                speech_ended_ = true;  /* 静音超时，语音结束 */
            }
        } else {
            speech_frames_ = 0;  /* 未开始说话，重置计数 */
        }
    }

    return is_active;
}

bool VAD::IsSpeechEnd() const {
    return speech_ended_;
}

void VAD::Reset() {
    silence_frames_ = 0;
    speech_frames_ = 0;
    in_speech_ = false;
    speech_ended_ = false;
}
