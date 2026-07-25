/*
 * state_machine.h
 * AI 语音助手状态机
 *
 * 状态流转:
 *   SLEEP → WAKEUP → LISTENING → PROCESSING → SPEAKING → SLEEP
 *   SLEEP ← (任何状态) ← 空闲超时/强制休眠
 */

#ifndef AI_ASSISTANT_STATE_MACHINE_H
#define AI_ASSISTANT_STATE_MACHINE_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

/* 助手运行状态 */
enum class AssistantState {
    SLEEP,      /* 休眠：仅 KWS 监听 */
    WAKEUP,     /* 唤醒过渡：播放提示音 */
    LISTENING,  /* 聆听：录音 + VAD */
    PROCESSING, /* 处理：ASR → 命令/LLM → TTS */
    SPEAKING,   /* 播报：TTS 播放 */
};

/* 状态名称转字符串 */
const char* StateName(AssistantState state);

/* 线程安全的状态机 */
class StateMachine {
public:
    using Callback = std::function<void(AssistantState old_state, AssistantState new_state)>;

    StateMachine(AssistantState initial = AssistantState::SLEEP);
    ~StateMachine() = default;

    /* 获取当前状态 */
    AssistantState CurrentState() const;

    /* 尝试转换到新状态，返回是否允许转换 */
    bool TransitionTo(AssistantState new_state);

    /* 注册状态变更回调 */
    void OnStateChanged(Callback cb);

    /* 检查给定转换是否合法 */
    static bool IsValidTransition(AssistantState from, AssistantState to);

private:
    std::atomic<AssistantState> current_state_;
    mutable std::mutex mutex_;
    std::vector<Callback> callbacks_;
};

#endif /* AI_ASSISTANT_STATE_MACHINE_H */
