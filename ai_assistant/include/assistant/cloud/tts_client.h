/*
 * tts_client.h
 * TTS（文本转语音）客户端接口
 *
 * 面向讯飞超拟人合成 WebSocket API。
 * 调试模式下保存合成文本和 PCM 音频到文件。
 */

#ifndef AI_ASSISTANT_TTS_CLIENT_H
#define AI_ASSISTANT_TTS_CLIENT_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class TTSClient {
public:
    virtual ~TTSClient() = default;

    /* 初始化（讯飞 API 凭证 + 调试模式）
     * 对于使用 x-api-key 认证的接口（如超拟人合成），
     * 通过 auth_token 参数传入 APIPassword（ak-... 格式）。
     */
    virtual bool Initialize(const std::string& app_id,
                            const std::string& api_key,
                            const std::string& api_secret,
                            bool debug_mode = false,
                            const std::string& auth_token = "") = 0;

    /* 合成文本为语音（结果通过回调返回） */
    virtual bool Synthesize(const std::string& text) = 0;

    /* 设置音色 */
    virtual void SetVoice(const std::string& voice_name) = 0;

    /* 音频数据回调（PCM 帧） */
    using AudioCallback = std::function<void(const std::vector<int16_t>&)>;
    void OnAudio(AudioCallback cb) { audio_callback_ = std::move(cb); }

    /* 设置调试模式 */
    virtual void SetDebugMode(bool enable) = 0;

protected:
    AudioCallback audio_callback_;
};

#endif /* AI_ASSISTANT_TTS_CLIENT_H */
