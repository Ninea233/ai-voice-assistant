/*
 * assistant.h
 * AI 语音助手主控制器 v2.3.1
 *
 * 优先级：action > skill > MCP
 *   action: keyword + LLM 触发，直执行，不回注
 *   skill:  仅 LLM 触发，一定回注（SKILL.md 描述→调用→加载完整内容）
 *   MCP:    仅 LLM 触发，一定回注（JSON-RPC 2.0 标准协议）
 *
 * 数据流:
 *   [KWS唤醒 → 播放wakeup.wav] → [连续对话循环]
 *   ASR → 偏好检测 → action 匹配 → LLM Agent(action/skill/MCP)
 *     └─ 优先级过滤: action > skill > MCP
 *     └─ action 不回注，skill 必回注，MCP 必回注
 *     └─ 最终回复 → TTS 流式管线（生产者-消费者）
 */

#ifndef AI_ASSISTANT_ASSISTANT_H
#define AI_ASSISTANT_ASSISTANT_H

#include "assistant/core/state_machine.h"
#include "assistant/core/config.h"
#include "assistant/audio/audio_capture.h"
#include "assistant/audio/audio_playback.h"
#include "assistant/audio/parallel_tts.h"
#include "assistant/audio/vad.h"
#include "assistant/kws/kws_engine.h"
#include "assistant/cloud/asr_client.h"
#include "assistant/cloud/llm_client.h"
#include "assistant/cloud/tts_client.h"
#include "assistant/command/ir_controller.h"
#include "assistant/sensitive/sensitive_filter.h"
#include "assistant/agent/agent_core.h"
#include "assistant/agent/action_manager.h"
#include "assistant/agent/skill_manager.h"
#include "assistant/agent/mcp_tools.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Assistant {
public:
    Assistant();
    ~Assistant();

    bool Initialize(const std::string& config_path);
    void Start();
    void Stop();
    void TriggerWakeup();
    void ForceSleep();

private:
    /* 状态机回调 */
    void OnStateChanged(AssistantState old_state, AssistantState new_state);
    void OnWakeWordDetected();
    void OnAudioData(const std::vector<int16_t>& data);
    void OnSpeechEnd();

    /* 处理 ASR 结果（核心路由：action > skill > MCP） */
    void ProcessResult(const std::string& asr_text);

    /* 执行单个工具/Skill/Action 调用 */
    struct ToolExecResult {
        bool success = false;
        std::string content;
        bool reenter_llm = true;
    };
    ToolExecResult ExecuteSingleTool(const agent::ToolCall& call);

    /* 批量执行 */
    std::string ExecuteToolCalls(const std::vector<agent::ToolCall>& calls);

    /* 移除 <tool_call> 标记 */
    static std::string StripToolCalls(const std::string& text);

    /* TTS */
    void OnTTSAudio(const std::vector<int16_t>& pcm);
    void OnPlaybackDone();
    void PlayWakeupSound();
    void SafeTTS(const std::string& text);

    /* 并行 TTS 播报一段完整文本（非流式来源：action 结果、错误提示等） */
    void StartParallelTTS(const std::string& text);

    /* 播放态进入/退出：暂停录音 + 状态机切换 */
    void EnterSpeakingState();
    void LeaveSpeakingState();

    /* 构建 LLM 系统提示词 */
    std::string BuildSystemPrompt() const;

    /* 构建工具定义列表（用于原生 function calling API） */
    std::vector<LLMClient::ToolDef> BuildToolDefs() const;

    /* 注册 */
    void RegisterActions();
    void RegisterMCPBuiltins();
    void RegisterSkillHandlers();

    /* ── 模块指针 ── */
    std::unique_ptr<StateMachine>    state_machine_;
    std::unique_ptr<Config>          config_;
    std::unique_ptr<AudioCapture>    audio_capture_;
    std::unique_ptr<AudioPlayback>   audio_playback_;
    std::unique_ptr<VAD>             vad_;
    std::unique_ptr<KWSEngine>       kws_;
    std::unique_ptr<ASRClient>       asr_;
    std::unique_ptr<LLMClient>       llm_;
    std::unique_ptr<TTSClient>       tts_;
    std::unique_ptr<IRController>    ir_;
    std::unique_ptr<SensitiveFilter> sensitive_;

    /* ── Agent 模块 ── */
    std::unique_ptr<agent::AgentCore>       agent_core_;
    std::unique_ptr<agent::ActionManager>   action_mgr_;
    std::unique_ptr<agent::SkillManager>    skill_mgr_;
    std::unique_ptr<agent::MCPTools>        mcp_tools_;

    /* ── 对话状态 ── */
    std::vector<int16_t> audio_buffer_;
    std::string wakeup_sound_path_;
    size_t listening_silence_samples_ = 0;
    unsigned int conversation_timeout_ms_ = 10000;

    /* ── 并行 TTS 管线（合成线程池 + Reorder Buffer + 顺序播放）── */
    std::unique_ptr<ParallelTTS> parallel_tts_;

    /* ── 运行标志 ── */
    bool running_ = false;
    bool sleep_requested_ = false;
};

#endif
