/*
 * audio_capture.h
 * ALSA 录音模块：以 DMA 方式从麦克风采集 PCM 音频
 *
 * 采集模型（DMA）：
 *   声卡 DMA 控制器把 ADC 采样自动搬进内核环形缓冲，每填满一个 period
 *   （320 帧 = 20ms @16kHz）触发中断。本模块用 SND_PCM_NONBLOCK 打开设备，
 *   在独立线程中 poll() 等待 period 就绪后再 readi，CPU 只在数据到达时被唤醒。
 *
 * 每帧（period）通过回调分发给 KWS（SLEEP 态）和 VAD（LISTENING 态）。
 *
 * 录音参数（可配置）：16kHz, 单声道, S16_LE, 320 帧/period
 */

#ifndef AI_ASSISTANT_AUDIO_CAPTURE_H
#define AI_ASSISTANT_AUDIO_CAPTURE_H

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    /* 打开并配置录音设备（DMA 非阻塞模式） */
    bool Open(const std::string& device = "default", unsigned int sample_rate = 16000, unsigned int channels = 1,
              snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE);

    /* 关闭设备 */
    void Close();

    /* 启动录音线程 */
    bool Start();

    /* 停止录音线程（等待线程退出，不可在录音回调中调用） */
    void Stop();

    /* 暂停录音：设置退出标志并释放设备。
     * 可在录音回调内部调用（此时线程不能 join 自己，改为 detach）。 */
    void Pause();

    /* 读取音频数据到 buffer（非阻塞，读取已就绪的帧），返回实际读取的帧数 */
    size_t Read(std::vector<int16_t>& buffer, size_t frames);

    /* 注册音频数据回调，每帧（period_size）触发一次 */
    using DataCallback = std::function<void(const std::vector<int16_t>&)>;
    void OnData(DataCallback cb);

    /* 查询参数 */
    unsigned int SampleRate() const { return sample_rate_; }
    unsigned int Channels() const { return channels_; }
    bool IsRunning() const { return running_.load(); }

private:
    /* 录音线程主循环：poll 等待 DMA period → readi → 回调。
     * generation 为本线程所属运行代次，仅当代次仍是最新时才回写 running_。 */
    void CaptureLoop(std::shared_ptr<std::atomic<bool>> stop_flag, unsigned long generation);

    /* 真正打开/关闭 ALSA 句柄 */
    bool OpenDevice();
    void CloseDevice();

    /* 等待录音线程结束；在自身线程调用时返回 false（由调用方决定 detach） */
    bool JoinCaptureThread();

    snd_pcm_t* handle_ = nullptr;
    std::string device_;
    unsigned int sample_rate_ = 16000;
    unsigned int channels_ = 1;
    snd_pcm_format_t format_ = SND_PCM_FORMAT_S16_LE;
    snd_pcm_uframes_t period_size_ = 320; /* 20ms @ 16kHz */

    /* running_：设备/线程是否处于活动状态；stop_flag_：当前运行代的退出标志。
     * 分代设计避免「Pause 后旧线程尚未退出、Start 复位 running_ 导致旧线程复活」
     * 这类两个线程同时操作同一句柄的竞态。 */
    std::atomic<bool> running_{false};
    std::shared_ptr<std::atomic<bool>> stop_flag_;
    /* 运行代次：每次 Start() 自增；线程据此判断自己是否仍是当前录音线程，
     * 避免旧线程退出时把新线程的状态误置为「已停止」。 */
    std::atomic<unsigned long> generation_{0};
    std::unique_ptr<std::thread> capture_thread_;
    DataCallback data_callback_;
};

#endif /* AI_ASSISTANT_AUDIO_CAPTURE_H */
