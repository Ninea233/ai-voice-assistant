/*
 * audio_playback.cpp
 * ALSA 播放模块实现
 *
 * 由于 i.MX6ULL + WM8960 的 ALSA 驱动在程序内直接调用
 * snd_pcm_writei/drain 时存在 DMA 状态兼容问题（drain 时报 overrun），
 * 改用 aplay 命令行工具播放 WAV 文件。aplay 能正确处理该硬件平台的音频输出。
 */

#include "assistant/audio/audio_playback.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unistd.h>

static const char* kTag = "[AudioPlayback]";
static const char* kWavPath = "playback.wav";

AudioPlayback::AudioPlayback() = default;

AudioPlayback::~AudioPlayback() {
    Stop();
    Close();
}

bool AudioPlayback::Open(const std::string& device,
                         unsigned int sample_rate,
                         unsigned int channels,
                         snd_pcm_format_t format) {
    /* aplay 方案不需要 ALSA handle，仅记录参数用于 WAV 头 */
    sample_rate_ = sample_rate;
    channels_    = channels;
    format_      = format;

    std::cout << kTag << " 播放模式: aplay (" << device
              << " " << sample_rate << "Hz "
              << channels << "ch)" << std::endl;
    return true;
}

void AudioPlayback::Close() {
    /* aplay 方案无 ALSA handle 需要关闭 */
    handle_ = nullptr;
}

/* 将 PCM 数据保存为 WAV 文件 */
static bool SaveWav(const std::string& path, const int16_t* data, size_t samples,
                    unsigned int rate, unsigned int channels) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;

    uint32_t data_bytes = static_cast<uint32_t>(samples * sizeof(int16_t));
    uint16_t bits = 16;

    struct {
        char     riff[4];
        uint32_t file_size;
        char     wave[4];
        char     fmt[4];
        uint32_t fmt_size;
        uint16_t audio_fmt;
        uint16_t channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
        char     data[4];
        uint32_t data_size;
    } wav = {
        {'R','I','F','F'},
        data_bytes + 36,
        {'W','A','V','E'},
        {'f','m','t',' '},
        16,
        1,
        static_cast<uint16_t>(channels),
        rate,
        rate * channels * bits / 8,
        static_cast<uint16_t>(channels * bits / 8),
        bits,
        {'d','a','t','a'},
        data_bytes
    };

    ofs.write(reinterpret_cast<const char*>(&wav), sizeof(wav));
    ofs.write(reinterpret_cast<const char*>(data), data_bytes);
    ofs.close();
    return true;
}

/* 用 aplay 播放 WAV 文件 */
static bool PlayWav(const std::string& wav_path) {
    std::string cmd = std::string("aplay -q ") + wav_path;
    std::cout << kTag << " 执行: " << cmd << std::endl;
    int rc = system(cmd.c_str());
    if (rc != 0) {
        std::cerr << kTag << " aplay 播放失败 (rc=" << rc
                  << ", WIFEXITED=" << WIFEXITED(rc)
                  << ", exit=" << WEXITSTATUS(rc)
                  << ", WIFSIGNALED=" << WIFSIGNALED(rc)
                  << ", term=" << WTERMSIG(rc) << ")"
                  << std::endl;
        return false;
    }
    return true;
}

bool AudioPlayback::Play(const std::vector<int16_t>& data) {
    if (data.empty()) return false;

    if (!SaveWav(kWavPath, data.data(), data.size(), sample_rate_, channels_)) {
        std::cerr << kTag << " 保存 WAV 失败" << std::endl;
        return false;
    }

    return PlayWav(kWavPath);
}

bool AudioPlayback::PlayAsync(const std::vector<int16_t>& data) {
    if (data.empty()) return false;

    /* 清理上一个播放线程对象（线程已结束但未 join，直接覆盖销毁会 terminate） */
    if (play_thread_ && play_thread_->joinable()) {
        play_thread_->detach();
    }
    play_thread_.reset();

    if (playing_) {
        std::cerr << kTag << " 正在播放中，忽略新请求" << std::endl;
        return false;
    }

    playing_ = true;
    play_thread_ = std::make_unique<std::thread>(&AudioPlayback::PlayThread, this, data);
    return true;
}

void AudioPlayback::Stop() {
    if (playing_) {
        playing_ = false;
        if (play_thread_ && play_thread_->joinable()) {
            play_thread_->join();
        }
        play_thread_.reset();
        /* 停止正在播放的 aplay 进程 */
        system("killall -q aplay 2>/dev/null");
    }
}

void AudioPlayback::OnPlaybackDone(PlaybackDoneCallback cb) {
    done_callback_ = std::move(cb);
}

void AudioPlayback::PlayThread(const std::vector<int16_t> data) {
    bool ok = false;

    if (SaveWav(kWavPath, data.data(), data.size(), sample_rate_, channels_)) {
        ok = PlayWav(kWavPath);
    } else {
        std::cerr << kTag << " 保存 WAV 失败" << std::endl;
    }

    if (!ok) {
        std::cerr << kTag << " 播放失败" << std::endl;
    }

    playing_ = false;

    if (done_callback_) {
        done_callback_();
    }
}
