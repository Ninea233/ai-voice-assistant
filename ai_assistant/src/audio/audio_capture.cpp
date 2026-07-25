/*
 * audio_capture.cpp
 * ALSA 录音模块实现
 */

#include "assistant/audio/audio_capture.h"
#include <cstring>
#include <iostream>

static const char* kTag = "[AudioCapture]";

AudioCapture::AudioCapture() = default;

AudioCapture::~AudioCapture() {
    Stop();
    Close();
}

bool AudioCapture::Open(const std::string& device,
                        unsigned int sample_rate,
                        unsigned int channels,
                        snd_pcm_format_t format) {
    if (handle_) Close();

    device_      = device;
    sample_rate_ = sample_rate;
    channels_    = channels;
    format_      = format;

    int rc = snd_pcm_open(&handle_, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        std::cerr << kTag << " 打开录音设备失败: "
                  << device_ << " - " << snd_strerror(rc) << std::endl;
        return false;
    }

    /* 设置硬件参数 */
    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle_, params);

    snd_pcm_hw_params_set_access(handle_, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle_, params, format_);
    snd_pcm_hw_params_set_channels(handle_, params, channels_);

    unsigned int actual_rate = sample_rate_;
    snd_pcm_hw_params_set_rate_near(handle_, params, &actual_rate, nullptr);

    snd_pcm_uframes_t period = period_size_;
    snd_pcm_hw_params_set_period_size_near(handle_, params, &period, nullptr);
    period_size_ = period;

    rc = snd_pcm_hw_params(handle_, params);
    if (rc < 0) {
        std::cerr << kTag << " 设置硬件参数失败: " << snd_strerror(rc) << std::endl;
        Close();
        return false;
    }

    std::cout << kTag << " 录音设备已打开: " << device_
              << " " << actual_rate << "Hz "
              << channels << "ch "
              << snd_pcm_format_description(format_)
              << " period=" << period_size_ << std::endl;
    return true;
}

void AudioCapture::Close() {
    if (handle_) {
        snd_pcm_close(handle_);
        handle_ = nullptr;
    }
}

bool AudioCapture::Start() {
    if (running_) return false;

    /* 如果 handle 已被关闭（Pause 时），重新打开 */
    if (!handle_) {
        if (!Open(device_, sample_rate_, channels_, format_)) {
            return false;
        }
    }

    int rc = snd_pcm_prepare(handle_);
    if (rc < 0) {
        std::cerr << kTag << " prepare 失败: " << snd_strerror(rc) << std::endl;
        return false;
    }

    running_ = true;
    capture_thread_ = std::make_unique<std::thread>(&AudioCapture::CaptureLoop, this);
    std::cout << kTag << " 录音线程已启动" << std::endl;
    return true;
}

void AudioCapture::Stop() {
    if (running_) {
        running_ = false;
        if (capture_thread_ && capture_thread_->joinable()) {
            capture_thread_->join();
        }
        capture_thread_.reset();
        std::cout << kTag << " 录音线程已停止" << std::endl;
    }
}

void AudioCapture::Pause() {
    running_ = false;
    /* detach 旧线程（不能 join 因为可能在自己线程上），
       让 OS 在线程退出后自动回收资源 */
    if (capture_thread_ && capture_thread_->joinable()) {
        capture_thread_->detach();
    }
    capture_thread_.reset();
    /* 关闭 ALSA 设备，释放硬件资源，避免与 aplay 冲突 */
    if (handle_) {
        snd_pcm_close(handle_);
        handle_ = nullptr;
    }
    std::cout << kTag << " 录音已暂停" << std::endl;
}

size_t AudioCapture::Read(std::vector<int16_t>& buffer, size_t frames) {
    if (!handle_) return 0;

    buffer.resize(frames * channels_);
    snd_pcm_sframes_t rc = snd_pcm_readi(handle_, buffer.data(), frames);
    if (rc < 0) {
        snd_pcm_recover(handle_, rc, 0);
        return 0;
    }
    return static_cast<size_t>(rc);
}

void AudioCapture::OnData(DataCallback cb) {
    data_callback_ = std::move(cb);
}

void AudioCapture::CaptureLoop() {
    std::vector<int16_t> buffer(period_size_ * channels_);

    while (running_) {
        snd_pcm_sframes_t rc = snd_pcm_readi(handle_, buffer.data(), period_size_);
        if (rc < 0) {
            snd_pcm_recover(handle_, rc, 0);
            continue;
        }

        if (data_callback_ && rc > 0) {
            /* 拷贝有效数据给回调，不修改 buffer 的 size（避免下次 read 越界） */
            std::vector<int16_t> frame(buffer.data(),
                                       buffer.data() + rc * channels_);
            data_callback_(frame);
        }
    }
}
