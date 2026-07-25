/*
 * audio_playback.h
 * ALSA 播放模块：播放 PCM 音频数据到喇叭
 *
 * 支持同步播放（Play）和异步播放（PlayAsync）
 * 播放完成后通过回调通知调用方
 */

#ifndef AI_ASSISTANT_AUDIO_PLAYBACK_H
#define AI_ASSISTANT_AUDIO_PLAYBACK_H

#include <alsa/asoundlib.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    /* 打开播放设备 */
    bool Open(const std::string& device = "default",
              unsigned int sample_rate = 16000,
              unsigned int channels = 1,
              snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE);

    /* 关闭设备 */
    void Close();

    /* 同步播放 PCM 数据 */
    bool Play(const std::vector<int16_t>& data);

    /* 异步播放（独立线程），立即返回 */
    bool PlayAsync(const std::vector<int16_t>& data);

    /* 停止当前播放 */
    void Stop();

    /* 是否正在播放 */
    bool IsPlaying() const { return playing_; }

    /* 播放完成回调 */
    using PlaybackDoneCallback = std::function<void()>;
    void OnPlaybackDone(PlaybackDoneCallback cb);

private:
    /* 异步播放线程 */
    void PlayThread(const std::vector<int16_t> data);

    snd_pcm_t* handle_ = nullptr;
    unsigned int sample_rate_ = 16000;
    unsigned int channels_ = 1;
    snd_pcm_format_t format_ = SND_PCM_FORMAT_S16_LE;

    bool playing_ = false;
    std::unique_ptr<std::thread> play_thread_;
    PlaybackDoneCallback done_callback_;
};

#endif /* AI_ASSISTANT_AUDIO_PLAYBACK_H */
