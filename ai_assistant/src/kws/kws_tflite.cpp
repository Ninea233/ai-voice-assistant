/*
 * kws_tflite.cpp
 * TFLite KWS 引擎实现（完整 MFCC 特征提取）
 *
 * MFCC 特征提取流程：
 *   预加重 → 分帧 → Hamming 窗 → FFT → 功率谱
 *   → Mel 滤波器组 → log → DCT → MFCC 系数
 *
 * 依赖:
 *   - TensorFlow Lite (交叉编译)
 *   - kissfft (FFT 计算)
 *
 * 模型输入: 400 维 (10帧 x 40维 MFCC)
 * 模型输出: [P(非唤醒词), P(唤醒词)]
 */

#include "assistant/kws/kws_tflite.h"

/* TFLite */
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"
#include "tensorflow/lite/core/interpreter_builder.h"

/* kissfft */
#include "kiss_fft.h"
#include "kiss_fftr.h"  /* 实数 FFT 优化版本 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>

static const char* kTag = "[KWSTFLite]";

/* 不透明指针转换辅助 */
#define TFLITE_MODEL(p)    (static_cast<tflite::FlatBufferModel*>((p)))
#define TFLITE_INTERP(p)   (static_cast<tflite::Interpreter*>((p)))

/* 预加重系数 */
static constexpr float kPreEmphasisAlpha = 0.97f;

/* int16 → float 归一化系数（librosa.load 默认归一化到 [-1, 1]） */
static constexpr float kInt16Norm = 1.0f / 32768.0f;

/* Mel 尺度转换常数 */
static float MelToHz(float mel) {
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}

static float HzToMel(float hz) {
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
}

/* ============================================
 * 构造 / 析构
 * ============================================ */
KWSTFLite::KWSTFLite() = default;

KWSTFLite::~KWSTFLite() {
    /* 释放 TFLite 资源 */
    if (interpreter_) {
        delete TFLITE_INTERP(interpreter_);
        interpreter_ = nullptr;
    }
    if (model_) {
        delete TFLITE_MODEL(model_);
        model_ = nullptr;
    }
    /* 释放缓存的 FFT 资源 */
    if (fft_cfg_) {
        kiss_fft_free(fft_cfg_);
        fft_cfg_ = nullptr;
    }
    if (fft_in_) {
        delete[] static_cast<kiss_fft_cpx*>(fft_in_);
        fft_in_ = nullptr;
    }
    if (fft_out_) {
        delete[] static_cast<kiss_fft_cpx*>(fft_out_);
        fft_out_ = nullptr;
    }
}

/* ============================================
 * 初始化
 * ============================================ */
bool KWSTFLite::Initialize(const std::string& model_path, float threshold) {
    /* 安全阈值处理：TFLite 输出 softmax 概率 [0,1]
     * 配置中若残留 SimpleKWS 能量阈值（>1.0）则自动钳位到 0.5 */
    threshold_ = (threshold > 1.0f) ? 0.5f : threshold;

    /* 1. 加载 TFLite 模型 */
    auto flatbuffer_model = tflite::FlatBufferModel::BuildFromFile(model_path.c_str());
    if (!flatbuffer_model) {
        std::cerr << kTag << " 加载模型失败: " << model_path << std::endl;
        return false;
    }
    model_ = flatbuffer_model.release();

    /* 2. 创建解释器（TFLite v2.14 使用 unique_ptr）
     *
     * !!! BuiltinOpResolver 必须使用 static 堆分配 !!!
     *
     * 原因：在此平台上（EGLIBC 2.19 + 静态链接 libstdc++.a），
     * BuiltinOpResolver 析构时触发 "corrupted double-linked list"
     * 堆损坏（TFLite v2.14 ARM 平台 bug），导致程序崩溃。
     *
     * 使用 static + heap 分配确保 resolver 只构造一次，
     * 永远不会被析构——对单进程嵌入式设备完全可接受。
     */
    static auto* s_resolver = new tflite::ops::builtin::BuiltinOpResolver();
    auto model_obj = TFLITE_MODEL(model_);
    tflite::InterpreterBuilder builder(*model_obj, *s_resolver);
    std::unique_ptr<tflite::Interpreter> temp_interpreter;
    builder(&temp_interpreter);
    if (!temp_interpreter) {
        std::cerr << kTag << " 创建解释器失败" << std::endl;
        return false;
    }
    interpreter_ = temp_interpreter.release();

    /* 3. 分配张量 */
    if (TFLITE_INTERP(interpreter_)->AllocateTensors() != kTfLiteOk) {
        std::cerr << kTag << " 分配张量失败" << std::endl;
        return false;
    }

    /* 4. 验证输入输出张量 */
    const auto& input = TFLITE_INTERP(interpreter_)->input_tensor(0);
    const auto& output = TFLITE_INTERP(interpreter_)->output_tensor(0);
    if (input->dims->size != 2 || input->dims->data[1] != kInputDim) {
        std::cerr << kTag << " 输入维度不匹配: 期望 (1," << kInputDim
                  << "), 实际 dims=" << input->dims->size << std::endl;
        return false;
    }
    if (output->dims->size != 2 || output->dims->data[1] != 2) {
        std::cerr << kTag << " 输出维度不匹配: 期望 (1,2)"
                  << std::endl;
        return false;
    }

    /* 5. 预计算 MFCC 参数 */
    if (!PreComputeWindow() || !PreComputeFilterbank() || !PreComputeDCT()) {
        std::cerr << kTag << " 预计算 MFCC 参数失败" << std::endl;
        return false;
    }
    std::cout << kTag << " [DEBUG] MFCC 预计算完成" << std::endl;

    /* 6. 预分配 FFT 缓存（避免每帧栈上分配 8KB） */
    fft_in_ = new kiss_fft_cpx[kFFTSize];
    std::cout << kTag << " [DEBUG] fft_in 分配完成" << std::endl;
    fft_out_ = new kiss_fft_cpx[kFFTSize];
    std::cout << kTag << " [DEBUG] fft_out 分配完成" << std::endl;
    fft_cfg_ = kiss_fft_alloc(kFFTSize, 0, nullptr, nullptr);
    if (!fft_cfg_) {
        std::cerr << kTag << " kiss_fft_alloc 失败" << std::endl;
        return false;
    }
    std::cout << kTag << " [DEBUG] FFT 缓存分配完成" << std::endl;

    std::cout << kTag << " KWS 模型加载成功: " << model_path
              << " 阈值=" << threshold_
              << " 输入维=" << kInputDim
              << std::endl;
    return true;
}

/* ============================================
 * 预计算：Hamming 窗
 * ============================================ */
bool KWSTFLite::PreComputeWindow() {
    hamming_window_.resize(kFrameLen);
    for (int i = 0; i < kFrameLen; i++) {
        hamming_window_[i] = 0.54f - 0.46f * std::cos(
            2.0f * M_PI * i / (kFrameLen - 1));
    }
    return true;
}

/* ============================================
 * 预计算：Mel 滤波器组
 *
 * 在 Mel 尺度上均匀放置 kNumMelBins 个三角形滤波器，
 * 覆盖 [0, sample_rate/2] 频率范围。
 * ============================================ */
bool KWSTFLite::PreComputeFilterbank() {
    const float lowMel = HzToMel(0.0f);
    const float highMel = HzToMel(static_cast<float>(kSampleRate) / 2.0f);

    /* 在 Mel 尺度上均匀取点：kNumMelBins 个滤波器需要 kNumMelBins+2 个点 */
    std::vector<float> melPoints(kNumMelBins + 2);
    for (int i = 0; i < kNumMelBins + 2; i++) {
        melPoints[i] = lowMel + (highMel - lowMel) * i / (kNumMelBins + 1);
    }

    /* 转换为 Hz 和 FFT bin 索引 */
    std::vector<int> binIndices(kNumMelBins + 2);
    for (int i = 0; i < kNumMelBins + 2; i++) {
        float hz = MelToHz(melPoints[i]);
        binIndices[i] = static_cast<int>(std::round(
            hz * kFFTSize / kSampleRate));
        /* 确保在有效范围内 */
        if (binIndices[i] < 0) binIndices[i] = 0;
        if (binIndices[i] >= kNumFFTBins) binIndices[i] = kNumFFTBins - 1;
    }

    /* 构建三角形滤波器 */
    mel_filterbank_.resize(kNumMelBins);
    for (int m = 0; m < kNumMelBins; m++) {
        mel_filterbank_[m].resize(kNumFFTBins, 0.0f);
        int left = binIndices[m];
        int center = binIndices[m + 1];
        int right = binIndices[m + 2];

        /* 上升沿：left → center */
        for (int k = left; k < center; k++) {
            if (center != left) {
                mel_filterbank_[m][k] = static_cast<float>(k - left) /
                                        (center - left);
            }
        }

        /* 下降沿：center → right */
        for (int k = center; k <= right; k++) {
            if (right != center) {
                mel_filterbank_[m][k] = static_cast<float>(right - k) /
                                        (right - center);
            }
        }
    }

    return true;
}

/* ============================================
 * 预计算：DCT 矩阵
 *
 * Type-II DCT（正交归一化）：
 *   C(i,j) = sqrt(2/N) * cos(π * i * (j + 0.5) / N)
 *   其中 i = 0...kNumMFCC-1, j = 0...kNumMelBins-1
 *   当 i == 0 时，sqrt(2/N) 替换为 sqrt(1/N)
 * ============================================ */
bool KWSTFLite::PreComputeDCT() {
    dct_matrix_.resize(kNumMFCC);
    for (int i = 0; i < kNumMFCC; i++) {
        dct_matrix_[i].resize(kNumMelBins);
        float scale = (i == 0) ?
            std::sqrt(1.0f / kNumMelBins) :
            std::sqrt(2.0f / kNumMelBins);
        for (int j = 0; j < kNumMelBins; j++) {
            dct_matrix_[i][j] = scale * std::cos(
                M_PI * i * (j + 0.5f) / kNumMelBins);
        }
    }
    return true;
}

/* ============================================
 * 预加重滤波
 *   y[n] = x[n] - α * x[n-1], α = 0.97
 * ============================================ */
void KWSTFLite::PreEmphasis(const std::vector<int16_t>& audio,
                             std::vector<float>& output) {
    output.resize(audio.size());
    if (audio.empty()) return;

    /* 归一化到 [-1, 1]：训练时 librosa.load 自动做了同样处理 */
    output[0] = static_cast<float>(audio[0]) * kInt16Norm;
    for (size_t i = 1; i < audio.size(); i++) {
        output[i] = (static_cast<float>(audio[i]) -
                     kPreEmphasisAlpha * static_cast<float>(audio[i - 1])) *
                    kInt16Norm;
    }
}

/* ============================================
 * 应用 Hamming 窗
 *   windowed[n] = frame[n] * hamming[n]
 * ============================================ */
void KWSTFLite::ApplyHammingWindow(const float* frame, float* windowed) {
    for (int i = 0; i < kFrameLen; i++) {
        windowed[i] = frame[i] * hamming_window_[i];
    }
}

/* ============================================
 * 计算功率谱
 *   使用 kissfft 计算 FFT，取幅度平方作为功率谱
 *   只保留前 kNumFFTBins 个（直流 ~ 奈奎斯特频率）
 * ============================================ */
void KWSTFLite::ComputePowerSpectrum(const float* windowed, float* spectrum) {
    /* 使用预分配的 FFT 缓存（避免栈上分配 8KB） */
    auto* fft_in = static_cast<kiss_fft_cpx*>(fft_in_);
    auto* fft_out = static_cast<kiss_fft_cpx*>(fft_out_);

    /* 前 kFrameLen 个点为窗函数处理后的音频数据 */
    for (int i = 0; i < kFrameLen; i++) {
        fft_in[i].r = windowed[i];
        fft_in[i].i = 0.0f;
    }
    /* 补零 */
    for (int i = kFrameLen; i < kFFTSize; i++) {
        fft_in[i].r = 0.0f;
        fft_in[i].i = 0.0f;
    }

    /* FFT（使用缓存的 cfg，无需每帧 alloc/free） */
    kiss_fft(static_cast<kiss_fft_cfg>(fft_cfg_), fft_in, fft_out);

    /* 功率谱 |X[k]|^2 */
    for (int i = 0; i < kNumFFTBins; i++) {
        spectrum[i] = fft_out[i].r * fft_out[i].r +
                      fft_out[i].i * fft_out[i].i;
    }
}

/* ============================================
 * Mel 滤波器组
 *   将功率谱映射到 Mel 尺度
 * ============================================ */
void KWSTFLite::ApplyMelFilterbank(const float* spectrum, float* mel_energies) {
    for (int m = 0; m < kNumMelBins; m++) {
        double energy = 0.0;
        for (int k = 0; k < kNumFFTBins; k++) {
            energy += spectrum[k] * mel_filterbank_[m][k];
        }
        mel_energies[m] = static_cast<float>(energy);
    }
}

/* ============================================
 * 对数
 *   对 Mel 能量取自然对数（添加极小值避免 log(0)）
 * ============================================ */
void KWSTFLite::ApplyLog(float* mel_energies) {
    const float kEpsilon = 1e-10f;
    for (int i = 0; i < kNumMelBins; i++) {
        mel_energies[i] = std::log(mel_energies[i] + kEpsilon);
    }
}

/* ============================================
 * DCT（离散余弦变换）
 *   将 log Mel 能量转换为 MFCC 系数
 * ============================================ */
void KWSTFLite::ApplyDCT(const float* log_energies, float* mfcc) {
    for (int i = 0; i < kNumMFCC; i++) {
        double sum = 0.0;
        for (int j = 0; j < kNumMelBins; j++) {
            sum += dct_matrix_[i][j] * log_energies[j];
        }
        mfcc[i] = static_cast<float>(sum);
    }
}

/* ============================================
 * 计算一个 MFCC 帧
 *   从 audio_start 指向的 kFrameLen 个采样点计算 40 维 MFCC
 * ============================================ */
std::vector<float> KWSTFLite::ComputeOneMFCC(const int16_t* audio_start) {
    std::vector<float> mfcc(kNumMFCC, 0.0f);

    /* 1. 预加重 */
    std::vector<int16_t> frame(audio_start, audio_start + kFrameLen);
    std::vector<float> preemphasized;
    PreEmphasis(frame, preemphasized);

    /* 2. Hamming 窗 */
    float windowed[kFrameLen];
    ApplyHammingWindow(preemphasized.data(), windowed);

    /* 3. FFT + 功率谱 */
    float spectrum[kNumFFTBins];
    ComputePowerSpectrum(windowed, spectrum);

    /* 4. Mel 滤波器组 */
    float mel_energies[kNumMelBins];
    ApplyMelFilterbank(spectrum, mel_energies);

    /* 5. 对数 */
    ApplyLog(mel_energies);

    /* 6. DCT → MFCC */
    ApplyDCT(mel_energies, mfcc.data());

    return mfcc;
}

/* ============================================
 * TFLite 推理
 *   用当前 MFCC 缓冲区中最新的 kContextFrames 帧
 *   构建 400 维输入向量，运行 TFLite 推理
 * ============================================ */
void KWSTFLite::RunInference() {
    if (!interpreter_) return;
    if (static_cast<int>(mfcc_buffer_.size()) < kContextFrames) return;

    /* 构建输入向量：从环形缓冲区末尾取 kContextFrames 帧 */
    float* input = TFLITE_INTERP(interpreter_)->typed_input_tensor<float>(0);
    if (!input) return;

    int idx = 0;
    auto it = mfcc_buffer_.end() - kContextFrames;
    for (int f = 0; f < kContextFrames; f++) {
        const auto& frame = *(it + f);
        for (int c = 0; c < kNumMFCC; c++) {
            input[idx++] = frame[c];
        }
    }

    /* 推理 */
    if (TFLITE_INTERP(interpreter_)->Invoke() != kTfLiteOk) return;

    /* 读取输出 [P(非唤醒词), P(唤醒词)] */
    float* output = TFLITE_INTERP(interpreter_)->typed_output_tensor<float>(0);
    if (!output) return;

    float wakeword_score = output[1];

    /* 打印推理分数（调试用，方便排查阈值问题） */
    if (debug_mode_) {
        static int infer_cnt = 0;
        if (++infer_cnt % 2 == 0 || wakeword_score > 0.1f) {
            std::cout << kTag << " [DEBUG] score: neg=" << output[0]
                      << " wake=" << wakeword_score
                      << " (thresh=" << threshold_
                      << ", counter=" << trigger_counter_ << "/" << kTriggerFrameCount << ")"
                      << std::endl;
            /* 每 10 次打印前 5 个 MFCC 输入值 */
            if (infer_cnt % 10 == 0) {
                std::cout << kTag << " [DEBUG] input[0..4]="
                          << input[0] << "," << input[1] << "," << input[2] << ","
                          << input[3] << "," << input[4]
                          << std::endl;
            }
        }
    }

    /* 连续判断：连续 kTriggerFrameCount 次超过阈值才触发 */
    if (wakeword_score > threshold_) {
        trigger_counter_++;
        if (trigger_counter_ >= kTriggerFrameCount) {
            if (!wakeword_detected_) {
                wakeword_detected_ = true;
                if (debug_mode_) {
                    std::cout << kTag << " 唤醒词检测到! score="
                              << wakeword_score << std::endl;
                }
                if (wakeword_callback_) {
                    wakeword_callback_();
                }
            }
        }
    } else {
        /* 分数低于阈值时，缓慢衰减计数值（防误触发抖动） */
        if (trigger_counter_ > 0) {
            trigger_counter_--;
        }
    }
}

/* ============================================
 * ProcessAudio
 *   处理 20ms (320 采样点) 的音频帧。
 *
 *   维护一个采样点缓冲区，每次调用尽可能多地
 *   滑动计算 MFCC 帧（帧移 10ms = 160 采样点）。
 *
 *   数据流：
 *     采样点累积 → 帧提取 → MFCC 计算
 *     → MFCC 缓冲区 → 上下文窗口 → TFLite 推理 → 决策
 * ============================================ */
void KWSTFLite::ProcessAudio(const std::vector<int16_t>& audio_frame) {
    if (audio_frame.empty()) return;

    /* 追加到采样点缓冲区 */
    sample_buffer_.insert(sample_buffer_.end(),
                          audio_frame.begin(), audio_frame.end());

    /* 每 50 次调用打印一次音频幅度（调试用） */
    if (debug_mode_) {
        static int audio_debug_cnt = 0;
        if (++audio_debug_cnt % 50 == 0) {
            int16_t max_val = INT16_MIN, min_val = INT16_MAX;
            for (auto s : audio_frame) {
                if (s > max_val) max_val = s;
                if (s < min_val) min_val = s;
            }
            std::cout << kTag << " [DEBUG] audio: frame_size=" << audio_frame.size()
                      << " buf_size=" << sample_buffer_.size()
                      << " min=" << min_val << " max=" << max_val
                      << std::endl;
        }
    }

    /* 滑动窗口处理：每次滑动 kFrameShift 个采样点 */
    size_t pos = 0;
    while (pos + kFrameLen <= sample_buffer_.size()) {
        /* 在当前偏移位置计算一个 MFCC 帧 */
        std::vector<float> mfcc = ComputeOneMFCC(
            &sample_buffer_[pos]);
        mfcc_buffer_.push_back(std::move(mfcc));

        /* 限制 MFCC 缓冲区大小（保留足够上下文即可） */
        while (mfcc_buffer_.size() > kContextFrames + 3) {
            mfcc_buffer_.pop_front();
        }

        pos += kFrameShift;
    }

    /* 移除已处理的采样点 */
    if (pos > 0) {
        sample_buffer_.erase(
            sample_buffer_.begin(),
            sample_buffer_.begin() + pos);
    }

    /* 如果 MFCC 缓冲区足够，运行推理 */
    if (mfcc_buffer_.size() >= static_cast<size_t>(kContextFrames)) {
        RunInference();
    }
}

/* ============================================
 * 查询 / 重置
 * ============================================ */
bool KWSTFLite::IsWakeWordDetected() const {
    return wakeword_detected_;
}

void KWSTFLite::Reset() {
    wakeword_detected_ = false;
    trigger_counter_ = 0;
    sample_buffer_.clear();
    mfcc_buffer_.clear();
}
