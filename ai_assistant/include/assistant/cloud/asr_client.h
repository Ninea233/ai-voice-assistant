/*
 * asr_client.h
 * ASR（自动语音识别）客户端接口
 *
 * 支持多引擎模型表决：
 * - 主引擎：讯飞中文识别大模型（WebSocket）
 * - 方言引擎：讯飞方言识别大模型（可启用/禁用）
 * - 表决策略：置信度加权 / 多数投票
 *
 * 调试模式：保存音频文件和 API 原始响应。
 */

#ifndef AI_ASSISTANT_ASR_CLIENT_H
#define AI_ASSISTANT_ASR_CLIENT_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/* ASR 识别结果（含置信度） */
struct ASRResult {
    std::string text;
    float confidence = 0.0f;
    std::string provider;  // "mandarin", "dialect", "voting"
};

class ASRClient {
public:
    virtual ~ASRClient() = default;

    /* 初始化（讯飞 API 凭证 + 调试模式） */
    virtual bool Initialize(const std::string& app_id,
                            const std::string& api_key,
                            const std::string& api_secret,
                            bool debug_mode = false) = 0;

    /* 开始识别（建立 WebSocket 连接） */
    virtual bool StartRecognition() = 0;

    /* 发送音频数据（流式识别时逐帧发送） */
    virtual bool SendAudio(const std::vector<int16_t>& audio_data) = 0;

    /* 结束识别，返回完整文本 */
    virtual std::string StopRecognition() = 0;

    /* 非流式识别（传入完整音频，返回文本 + 置信度） */
    virtual ASRResult Recognize(const std::vector<int16_t>& audio_data) = 0;

    /* 识别结果回调（中间结果 / 最终结果 + 置信度） */
    using ResultCallback = std::function<void(const std::string& text, bool is_final, float confidence)>;
    void OnResult(ResultCallback cb) { result_callback_ = std::move(cb); }

    /* 设置方言（预留多模型表决接口） */
    virtual bool SetDialect(const std::string& dialect) = 0;

    /* 设置中文识别大模型资源 ID（讯飞 Spark iAT 大模型） */
    virtual void SetResId(const std::string& res_id) = 0;

    /* 设置调试模式 */
    virtual void SetDebugMode(bool enable) = 0;

protected:
    ResultCallback result_callback_;
};

#endif /* AI_ASSISTANT_ASR_CLIENT_H */
