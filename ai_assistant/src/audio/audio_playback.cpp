/*
 * audio_playback.cpp
 * ALSA 播放模块实现 — PCM 直写 + DMA 搬运
 *
 * 播放一句 PCM 的完整过程：
 *   EnsureDevice() 打开播放设备（懒加载，避免与录音长期争用）
 *   → snd_pcm_prepare() 复位播放位置
 *   → snd_pcm_writei() 循环写满内核环形缓冲（DMA 后台搬到 DAC）
 *   → snd_pcm_drain() 等待缓冲播空（保证同步语义）
 *   → CloseDevice() 释放设备
 *
 * 说明：i.MX6ULL + WM8960 驱动在 drain 阶段可能报 -EPIPE（overrun）。
 *       此时数据已全部写入并由 DMA 播完，故将其视为正常结束，仅记日志。
 */

#include "assistant/audio/audio_playback.h"

#include <chrono>
#include <cstring>
#include <iostream>

static const char* kTag = "[AudioPlayback]";

AudioPlayback::AudioPlayback() = default;

AudioPlayback::~AudioPlayback() {
    Stop();
    Close();
}

/* ========== 设备打开 / 关闭 ========== */

bool AudioPlayback::Open(const std::string& device,
                         unsigned int sample_rate,
                         unsigned int channels,
                         snd_pcm_format_t format) {
    /* 仅记录参数：句柄延迟到首次 Play 时打开，避免录音运行期占用声卡 */
    device_      = device;
    sample_rate_ = sample_rate;
    channels_    = channels;
    format_      = format;

    std::cout << kTag << " 播放参数: " << device
              << " " << sample_rate << "Hz "
              << channels << "ch "
              << snd_pcm_format_description(format)
              << " (DMA 直写)" << std::endl;
    return true;
}

bool AudioPlayback::EnsureDevice() {
    std::lock_guard<std::mutex> lock(device_mutex_);
    if (handle_) return true;

    int rc = snd_pcm_open(&handle_, device_.c_str(),
                          SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        std::cerr << kTag << " 打开播放设备失败: "
                  << device_ << " - " << snd_strerror(rc) << std::endl;
        handle_ = nullptr;
        return false;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(handle_, hw);
    snd_pcm_hw_params_set_access(handle_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle_, hw, format_);
    snd_pcm_hw_params_set_channels(handle_, hw, channels_);

    unsigned int rate = sample_rate_;
    snd_pcm_hw_params_set_rate_near(handle_, hw, &rate, nullptr);
    snd_pcm_hw_params_set_period_size_near(handle_, hw, &period_size_, nullptr);
    snd_pcm_hw_params_set_buffer_size_near(handle_, hw, &buffer_size_);

    rc = snd_pcm_hw_params(handle_, hw);
    if (rc < 0) {
        std::cerr << kTag << " 设置播放硬件参数失败: " << snd_strerror(rc) << std::endl;
        snd_pcm_close(handle_);
        handle_ = nullptr;
        return false;
    }

    /* sw_params：有数据即启动 DMA，缩短起播延迟 */
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(handle_, sw);
    snd_pcm_sw_params_set_start_threshold(handle_, sw, 1);
    snd_pcm_sw_params_set_avail_min(handle_, sw, period_size_);
    if (snd_pcm_sw_params(handle_, sw) < 0) {
        std::cerr << kTag << " 设置播放软件参数失败（忽略）" << std::endl;
    }

    if (rate != sample_rate_) {
        std::cerr << kTag << " 警告: 播放采样率被驱动调整为 " << rate
                  << "Hz（请求 " << sample_rate_ << "Hz）" << std::endl;
        sample_rate_ = rate;
    }
    return true;
}

void AudioPlayback::CloseDevice() {
    std::lock_guard<std::mutex> lock(device_mutex_);
    if (handle_) {
        snd_pcm_close(handle_);
        handle_ = nullptr;
    }
}

void AudioPlayback::Close() {
    CloseDevice();
}

/* ========== 播放 ========== */

bool AudioPlayback::Play(const std::vector<int16_t>& data) {
    if (data.empty()) return false;
    if (channels_ == 0) return false;
    if (!EnsureDevice()) return false;

    const size_t total_frames = data.size() / channels_;
    if (total_frames == 0) {
        CloseDevice();
        return false;
    }

    bool ok = true;

    /* 复位播放位置；XRUN 后驱动可能处于错误态，先 recover 再重试 */
    int rc = snd_pcm_prepare(handle_);
    if (rc < 0) {
        rc = snd_pcm_recover(handle_, rc, 1);
        if (rc < 0) {
            std::cerr << kTag << " prepare 失败: " << snd_strerror(rc) << std::endl;
            CloseDevice();
            return false;
        }
    }

    /* 阻塞写：内核环形缓冲有空位即返回；DMA 自动搬运到 DAC */
    const int16_t* cursor = data.data();
    size_t remaining = total_frames;
    while (remaining > 0) {
        snd_pcm_sframes_t written = snd_pcm_writei(handle_, cursor, remaining);
        if (written < 0) {
            /* XRUN/挂起等：恢复后重试剩余数据 */
            written = snd_pcm_recover(handle_, written, 1);
            if (written < 0) {
                std::cerr << kTag << " 写入失败: " << snd_strerror(written) << std::endl;
                ok = false;
                break;
            }
            continue;
        }
        cursor += static_cast<size_t>(written) * channels_;
        remaining -= static_cast<size_t>(written);
    }

    if (ok) {
        /* drain 等到缓冲播空，保证「Play 返回即播完」 */
        rc = snd_pcm_drain(handle_);
        if (rc < 0 && rc != -EPIPE) {
            std::cerr << kTag << " drain 异常(视为已播完): " << snd_strerror(rc) << std::endl;
        }
    }

    CloseDevice();
    return ok;
}

bool AudioPlayback::PlayAsync(const std::vector<int16_t>& data) {
    if (data.empty()) return false;

    if (playing_.load()) {
        std::cerr << kTag << " 正在播放中，忽略新请求" << std::endl;
        return false;
    }

    /* playing_ 为 false 说明上一轮线程已收尾，此处 join 立即返回 */
    if (!JoinPlayThread() && play_thread_) {
        play_thread_->detach();
    }
    play_thread_.reset();

    playing_ = true;
    play_thread_ = std::make_unique<std::thread>(
        &AudioPlayback::PlayThread, this, data);
    return true;
}

bool AudioPlayback::JoinPlayThread() {
    if (!play_thread_ || !play_thread_->joinable()) return true;
    if (play_thread_->get_id() == std::this_thread::get_id()) {
        return false; /* 在播放线程自身的回调链里调用，不能 join 自己 */
    }
    play_thread_->join();
    return true;
}

void AudioPlayback::Stop() {
    playing_ = false;

    /* drop 丢弃未播数据，解除正在 writei/drain 的线程的阻塞 */
    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        if (handle_) {
            snd_pcm_drop(handle_);
        }
    }

    if (!JoinPlayThread() && play_thread_) {
        play_thread_->detach();
    }
    play_thread_.reset();

    /* 句柄统一由 Play() 收尾释放：此处不再 close，避免与仍在 Play 内的
     * 线程并发访问同一句柄（drop 已足以解除其阻塞）。 */
}

void AudioPlayback::OnPlaybackDone(PlaybackDoneCallback cb) {
    done_callback_ = std::move(cb);
}

void AudioPlayback::PlayThread(const std::vector<int16_t> data) {
    bool ok = Play(data);
    if (!ok) {
        std::cerr << kTag << " 播放失败" << std::endl;
    }

    playing_ = false;

    if (done_callback_) {
        done_callback_();
    }
}
