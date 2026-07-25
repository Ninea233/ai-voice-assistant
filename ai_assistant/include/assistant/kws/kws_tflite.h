/*
 * kws_tflite.h
 * TFLite 实现的 KWS 引擎（完整 MFCC 特征提取）
 *
 * 使用 TensorFlow Lite for ARM Linux 运行唤醒词检测模型。
 * MFCC 参数与 Python 训练脚本 scripts/train_kws.py 保持一致。
 *
 * 注意：TFLite 类型通过不透明指针使用，避免头文件暴露实现细节。
 * 真实 TFLite 头文件仅在 .cpp 中包含。
 */

#ifndef AI_ASSISTANT_KWS_TFLITE_H
#define AI_ASSISTANT_KWS_TFLITE_H

#include "assistant/kws/kws_engine.h"
#include <deque>
#include <memory>
#include <vector>

class KWSTFLite : public KWSEngine {
public:
    KWSTFLite();
    ~KWSTFLite() override;

    bool Initialize(const std::string& model_path, float threshold) override;
    void ProcessAudio(const std::vector<int16_t>& audio_frame) override;
    bool IsWakeWordDetected() const override;
    void Reset() override;
    void SetDebugMode(bool enable) override { debug_mode_ = enable; }

private:
    /* === MFCC 参数 === */
    static constexpr int kSampleRate = 16000;
    static constexpr int kFrameLen = 480;        // 30ms @ 16kHz
    static constexpr int kFrameShift = 160;      // 10ms @ 16kHz
    static constexpr int kFFTSize = 512;         // 2^N >= kFrameLen
    static constexpr int kNumMFCC = 40;          // MFCC 维度
    static constexpr int kNumMelBins = 40;       // Mel 滤波器数量
    static constexpr int kNumFFTBins = kFFTSize / 2 + 1;

    /* === 推理参数 === */
    static constexpr int kContextFrames = 10;
    static constexpr int kInputDim = kContextFrames * kNumMFCC;
    static constexpr int kTriggerFrameCount = 4;

    /* === MFCC 特征提取 === */
    void PreEmphasis(const std::vector<int16_t>& audio, std::vector<float>& output);
    void ApplyHammingWindow(const float* frame, float* windowed);
    void ComputePowerSpectrum(const float* windowed, float* spectrum);
    void ApplyMelFilterbank(const float* spectrum, float* mel_energies);
    void ApplyLog(float* mel_energies);
    void ApplyDCT(const float* log_energies, float* mfcc);
    std::vector<float> ComputeOneMFCC(const int16_t* audio_start);
    void RunInference();

    /* === 预计算数据 === */
    bool PreComputeFilterbank();
    bool PreComputeDCT();
    bool PreComputeWindow();

    std::vector<std::vector<float>> mel_filterbank_;
    std::vector<std::vector<float>> dct_matrix_;
    std::vector<float> hamming_window_;

    /* === 运行时状态 === */
    std::vector<int16_t> sample_buffer_;
    std::deque<std::vector<float>> mfcc_buffer_;

    /* === TFLite 模型及解释器（不透明指针） === */
    void* model_ = nullptr;         // tflite::FlatBufferModel*
    void* interpreter_ = nullptr;   // tflite::Interpreter*

    /* === 缓存 FFT 配置，避免每帧栈上分配 8KB + alloc/free === */
    void* fft_cfg_ = nullptr;       // kiss_fft_cfg
    void* fft_in_ = nullptr;        // kiss_fft_cpx[kFFTSize]
    void* fft_out_ = nullptr;       // kiss_fft_cpx[kFFTSize]

    float threshold_ = 0.5f;
    bool wakeword_detected_ = false;
    bool debug_mode_ = false;
    int trigger_counter_ = 0;
};

#endif /* AI_ASSISTANT_KWS_TFLITE_H */
