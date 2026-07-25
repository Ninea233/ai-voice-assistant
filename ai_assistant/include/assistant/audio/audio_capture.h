/*
 * audio_capture.h
 * ALSA 录音模块：从麦克风采集 PCM 音频数据
 *
 * 使用阻塞 I/O 模式，在独立线程中循环读取音频帧。
 * 每帧通过回调分发给 KWS（SLEEP 态）和 VAD（LISTENING 态）。
 *
 * 录音参数（可配置）：16kHz, 单声道, S16_LE, 320 帧/period
 */

#ifndef AI_ASSISTANT_AUDIO_CAPTURE_H
#define AI_ASSISTANT_AUDIO_CAPTURE_H

#include <alsa/asoundlib.h>

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

    /* 打开录音设备 */
    bool Open(const std::string& device = "default", unsigned int sample_rate = 16000, unsigned int channels = 1,
              snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE);

    /* 关闭设备 */
    void Close();

    /* 启动录音线程 */
    bool Start();

    /* 停止录音线程（等待线程退出，不可在录音回调中调用） */
    void Stop();

    /* 暂停录音（仅设标志，不 join——安全用于录音回调内部） */
    void Pause();

    /* 读取音频数据到 buffer（非阻塞），返回实际读取的帧数 */
    size_t Read(std::vector<int16_t>& buffer, size_t frames);

    /* 注册音频数据回调，每帧（period_size）触发一次 */
    using DataCallback = std::function<void(const std::vector<int16_t>&)>;
    void OnData(DataCallback cb);

    /* 查询参数 */
    unsigned int SampleRate() const { return sample_rate_; }
    unsigned int Channels() const { return channels_; }
    bool IsRunning() const { return running_; }

private:
    /* 录音线程主循环 */
    void CaptureLoop();

    snd_pcm_t* handle_ = nullptr;
    std::string device_;
    unsigned int sample_rate_ = 16000;
    unsigned int channels_ = 1;
    snd_pcm_format_t format_ = SND_PCM_FORMAT_S16_LE;
    snd_pcm_uframes_t period_size_ = 320; /* 20ms @ 16kHz */

    bool running_ = false;
    std::unique_ptr<std::thread> capture_thread_;
    DataCallback data_callback_;
};

#endif /* AI_ASSISTANT_AUDIO_CAPTURE_H */
