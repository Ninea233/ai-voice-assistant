/*
 * audio_playback.h
 * 音频播放模块：PCM 直接写入 ALSA 播放设备，由 DMA 搬运到 DAC
 *
 * 播放模型（DMA）：
 *   snd_pcm_writei 把 PCM 写入内核环形缓冲后立即返回，DAC 侧由 DMA 引擎
 *   自动搬运，CPU 不参与逐采样搬运；snd_pcm_drain 等到缓冲播空，保证
 *   「Play 返回即播完」，供顺序播放使用。
 *
 * 设备生命周期：Open() 只记录参数，真正的 snd_pcm_open 延迟到首次播放时执行，
 * 播放结束即释放句柄——录音与播放共用 WM8960，延迟打开可避免设备占用冲突。
 *
 * 支持同步播放（Play）和异步播放（PlayAsync），播完通过回调通知调用方。
 */

#ifndef AI_ASSISTANT_AUDIO_PLAYBACK_H
#define AI_ASSISTANT_AUDIO_PLAYBACK_H

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    /* 打开播放设备（仅记录参数，句柄在首次播放时创建） */
    bool Open(const std::string& device = "default",
              unsigned int sample_rate = 16000,
              unsigned int channels = 1,
              snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE);

    /* 关闭设备 */
    void Close();

    /* 同步播放 PCM 数据（返回时已播完） */
    bool Play(const std::vector<int16_t>& data);

    /* 异步播放（独立线程），立即返回 */
    bool PlayAsync(const std::vector<int16_t>& data);

    /* 停止当前播放并释放设备 */
    void Stop();

    /* 是否正在播放 */
    bool IsPlaying() const { return playing_.load(); }

    /* 播放完成回调 */
    using PlaybackDoneCallback = std::function<void()>;
    void OnPlaybackDone(PlaybackDoneCallback cb);

    /* 查询参数 */
    unsigned int SampleRate() const { return sample_rate_; }
    unsigned int Channels() const { return channels_; }

private:
    /* 异步播放线程 */
    void PlayThread(const std::vector<int16_t> data);

    /* 打开设备（延迟），成功返回 true */
    bool EnsureDevice();
    void CloseDevice();

    /* 回收播放线程：在自身线程调用时返回 false（由调用方决定 detach） */
    bool JoinPlayThread();

    snd_pcm_t* handle_ = nullptr;
    std::string device_;
    unsigned int sample_rate_ = 16000;
    unsigned int channels_ = 1;
    snd_pcm_format_t format_ = SND_PCM_FORMAT_S16_LE;
    snd_pcm_uframes_t period_size_ = 320;
    snd_pcm_uframes_t buffer_size_ = 16000;

    std::atomic<bool> playing_{false};
    std::unique_ptr<std::thread> play_thread_;
    PlaybackDoneCallback done_callback_;
    std::mutex device_mutex_; /* 保护句柄的创建/释放 */
};

#endif /* AI_ASSISTANT_AUDIO_PLAYBACK_H */
