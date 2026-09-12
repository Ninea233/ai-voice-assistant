/*
 * audio_capture.cpp
 * ALSA 录音模块实现 — DMA 非阻塞 + poll 采集
 *
 * 与阻塞式 readi 的区别：设备以 SND_PCM_NONBLOCK 打开，线程先 poll() 等待
 * DMA 搬运满一个 period（POLLIN 就绪）再 readi，无数据时不占 CPU。
 * 同时 poll 的 100ms 超时使线程能周期性检查退出标志，Pause() 后能及时收尾。
 */

#include "assistant/audio/audio_capture.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <poll.h>

static const char* kTag = "[AudioCapture]";

/* poll 超时：兼顾「及时响应退出」与「不做无谓唤醒」 */
static const int kPollTimeoutMs = 100;

AudioCapture::AudioCapture() = default;

AudioCapture::~AudioCapture() {
    Stop();
    Close();
}

/* ========== 设备打开 / 关闭 ========== */

bool AudioCapture::Open(const std::string& device,
                        unsigned int sample_rate,
                        unsigned int channels,
                        snd_pcm_format_t format) {
    CloseDevice();

    device_      = device;
    sample_rate_ = sample_rate;
    channels_    = channels;
    format_      = format;

    return OpenDevice();
}

bool AudioCapture::OpenDevice() {
    /* SND_PCM_NONBLOCK：readi 无数据立刻返回 -EAGAIN，由 poll 负责等待 */
    int rc = snd_pcm_open(&handle_, device_.c_str(),
                          SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (rc < 0) {
        std::cerr << kTag << " 打开录音设备失败: "
                  << device_ << " - " << snd_strerror(rc) << std::endl;
        handle_ = nullptr;
        return false;
    }

    /* 下发硬件参数 */
    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle_, params);

    snd_pcm_hw_params_set_access(handle_, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle_, params, format_);
    snd_pcm_hw_params_set_channels(handle_, params, channels_);

    unsigned int actual_rate = sample_rate_;
    snd_pcm_hw_params_set_rate_near(handle_, params, &actual_rate, nullptr);

    /* period = DMA 中断/唤醒粒度，320 帧 = 20ms @16kHz，与 KWS/VAD 节奏一致 */
    snd_pcm_uframes_t period = period_size_;
    snd_pcm_hw_params_set_period_size_near(handle_, params, &period, nullptr);

    rc = snd_pcm_hw_params(handle_, params);
    if (rc < 0) {
        std::cerr << kTag << " 设置硬件参数失败: " << snd_strerror(rc) << std::endl;
        CloseDevice();
        return false;
    }

    /* _near 不保证精确生效，回读驱动实际采纳的值 */
    period_size_ = period;
    snd_pcm_hw_params_get_rate(params, &actual_rate, nullptr);
    if (actual_rate != sample_rate_) {
        std::cerr << kTag << " 警告: 采样率被驱动调整为 " << actual_rate
                  << "Hz（请求 " << sample_rate_ << "Hz）" << std::endl;
        sample_rate_ = actual_rate;
    }

    std::cout << kTag << " 录音设备已打开(DMA): " << device_
              << " " << sample_rate_ << "Hz "
              << channels_ << "ch "
              << snd_pcm_format_description(format_)
              << " period=" << period_size_ << std::endl;
    return true;
}

void AudioCapture::CloseDevice() {
    if (handle_) {
        snd_pcm_close(handle_);
        handle_ = nullptr;
    }
}

void AudioCapture::Close() {
    CloseDevice();
}

/* ========== 线程生命周期 ========== */

bool AudioCapture::JoinCaptureThread() {
    if (!capture_thread_ || !capture_thread_->joinable()) return true;
    if (capture_thread_->get_id() == std::this_thread::get_id()) {
        return false; /* 在录音线程自身的回调链里调用，不能 join 自己 */
    }
    capture_thread_->join();
    return true;
}

bool AudioCapture::Start() {
    if (running_.load()) return false;

    /* 回收上一代线程对象（若为已退出的旧线程，join 会立即返回） */
    if (!JoinCaptureThread() && capture_thread_) {
        capture_thread_->detach();
    }
    capture_thread_.reset();

    if (!handle_ && !OpenDevice()) return false;

    int rc = snd_pcm_prepare(handle_);
    if (rc < 0) {
        std::cerr << kTag << " prepare 失败: " << snd_strerror(rc) << std::endl;
        return false;
    }

    stop_flag_ = std::make_shared<std::atomic<bool>>(false);
    unsigned long generation = ++generation_;
    running_ = true;
    capture_thread_ = std::make_unique<std::thread>(
        &AudioCapture::CaptureLoop, this, stop_flag_, generation);
    std::cout << kTag << " 录音线程已启动" << std::endl;
    return true;
}

void AudioCapture::Stop() {
    running_ = false;
    if (stop_flag_) stop_flag_->store(true);

    if (!JoinCaptureThread() && capture_thread_) {
        /* Stop 被录音线程自身调用：只能 detach，交由 OS 回收 */
        capture_thread_->detach();
    }
    capture_thread_.reset();
    stop_flag_.reset();
    CloseDevice();
    std::cout << kTag << " 录音已停止" << std::endl;
}

void AudioCapture::Pause() {
    running_ = false;
    if (stop_flag_) stop_flag_->store(true);

    if (!JoinCaptureThread() && capture_thread_) {
        /* 典型路径：唤醒/语音结束的回调链在录音线程内调用 Pause()，
         * 此时不能 join 自己，detach 后由本线程自行走到循环出口退出。 */
        capture_thread_->detach();
    }
    capture_thread_.reset();
    stop_flag_.reset();

    /* 立即释放声卡：播放（DMA/aplay）需要用同一设备 */
    CloseDevice();
    std::cout << kTag << " 录音已暂停" << std::endl;
}

/* ========== 采集 ========== */

void AudioCapture::OnData(DataCallback cb) {
    data_callback_ = std::move(cb);
}

size_t AudioCapture::Read(std::vector<int16_t>& buffer, size_t frames) {
    if (!handle_) return 0;

    buffer.resize(frames * channels_);
    snd_pcm_sframes_t rc = snd_pcm_readi(handle_, buffer.data(), frames);
    if (rc < 0) {
        if (rc == -EAGAIN) return 0; /* 非阻塞模式下暂无数据 */
        snd_pcm_recover(handle_, rc, 1);
        return 0;
    }
    return static_cast<size_t>(rc);
}

void AudioCapture::CaptureLoop(std::shared_ptr<std::atomic<bool>> stop_flag,
                               unsigned long generation) {
    std::vector<int16_t> buffer(period_size_ * channels_);

    /* 仅当代次仍是最新时才回写 running_：旧线程不得覆盖新线程的状态 */
    auto mark_stopped = [this, generation]() {
        if (generation_.load() == generation) {
            running_ = false;
        }
    };

    /* 获取 ALSA 的 poll 描述符（接口允许返回多个，通常为 1 个） */
    int nfds = snd_pcm_poll_descriptors_count(handle_);
    if (nfds <= 0) {
        std::cerr << kTag << " 获取 poll 描述符数量失败" << std::endl;
        mark_stopped();
        return;
    }
    std::vector<struct pollfd> pfds(static_cast<size_t>(nfds));
    if (snd_pcm_poll_descriptors(handle_, pfds.data(), nfds) < 0) {
        std::cerr << kTag << " 获取 poll 描述符失败" << std::endl;
        mark_stopped();
        return;
    }

    /* 非阻塞模式下采集流需显式启动，否则 poll 永远不会就绪 */
    int sret = snd_pcm_start(handle_);
    if (sret < 0 && sret != -EPIPE) {
        std::cerr << kTag << " 启动采集流失败: " << snd_strerror(sret) << std::endl;
        mark_stopped();
        return;
    }

    while (!stop_flag->load()) {
        int ret = poll(pfds.data(), static_cast<nfds_t>(nfds), kPollTimeoutMs);

        /* Pause/Stop 可能已在回调内关闭句柄，此后不得再触碰 handle_ */
        if (stop_flag->load()) break;

        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << kTag << " poll 失败: " << std::strerror(errno) << std::endl;
            break;
        }
        if (ret == 0) continue; /* 超时：回到循环检查退出标志 */

        unsigned short revents = 0;
        if (snd_pcm_poll_descriptors_revents(handle_, pfds.data(), nfds, &revents) < 0) {
            continue;
        }
        if (!(revents & POLLIN)) continue;

        /* DMA 已搬满一个 period，readi 只做一次 memcpy，不阻塞 */
        snd_pcm_sframes_t rc = snd_pcm_readi(handle_, buffer.data(), period_size_);
        if (rc < 0) {
            if (rc == -EAGAIN) continue;
            /* -EPIPE=overrun：恢复后需重新启动采集流 */
            if (snd_pcm_recover(handle_, rc, 1) < 0) {
                std::cerr << kTag << " 不可恢复的采集错误: " << snd_strerror(rc) << std::endl;
                break;
            }
            snd_pcm_start(handle_);
            continue;
        }
        if (rc == 0) continue;

        if (data_callback_) {
            /* 拷贝有效数据给回调；回调内部可能调用 Pause() 关闭句柄，
             * 因此回调返回后立即由 while 条件检查退出标志，不再使用 handle_。 */
            std::vector<int16_t> frame(buffer.begin(),
                                       buffer.begin() + rc * channels_);
            data_callback_(frame);
        }
    }

    /* 因不可恢复错误退出循环时标记停止，让上层可重新 Start；
     * 若已被 Pause/Start 换代，则不动新线程的状态 */
    mark_stopped();
    std::cout << kTag << " 录音线程退出" << std::endl;
}
