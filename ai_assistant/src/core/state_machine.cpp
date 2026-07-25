/*
 * state_machine.cpp
 * AI 语音助手状态机实现
 */

#include "assistant/core/state_machine.h"
#include <algorithm>

const char* StateName(AssistantState state) {
    switch (state) {
        case AssistantState::SLEEP:      return "SLEEP";
        case AssistantState::WAKEUP:     return "WAKEUP";
        case AssistantState::LISTENING:  return "LISTENING";
        case AssistantState::PROCESSING: return "PROCESSING";
        case AssistantState::SPEAKING:   return "SPEAKING";
        default:                         return "UNKNOWN";
    }
}

/* 合法的状态转换表
 *
 * 唤醒后可连续多轮对话，播报完成后回到 LISTENING 继续聆听，
 * 而非直接 SLEEP。只有用户说"休眠"或静默超时才会 SLEEP。
 *
 * 转换图:
 *   SLEEP → WAKEUP → LISTENING ⇄ PROCESSING ⇄ SPEAKING
 *            ↑          ↓              ↓           ↓
 *            └──────────┴──────────────┴───────────┘
 *                      任意状态可强制回 SLEEP
 */
static const bool kValidTransitions[5][5] = {
    /* 当前 ↓ / 目标 →  SLEEP  WAKEUP  LISTENING  PROCESSING  SPEAKING */
    /* SLEEP    */ { false, true,   false,    false,      false },
    /* WAKEUP   */ { false, false,  true,     false,      false },
    /* LISTENING*/ { true,  false,  false,    true,       false },
    /* PROCESSING*/{ true,  false,  true,     false,      true  },
    /* SPEAKING */ { true,  false,  true,     false,      false },
};

StateMachine::StateMachine(AssistantState initial)
    : current_state_(initial) {}

AssistantState StateMachine::CurrentState() const {
    return current_state_.load();
}

bool StateMachine::TransitionTo(AssistantState new_state) {
    AssistantState old = current_state_.load();
    if (!IsValidTransition(old, new_state)) {
        return false;
    }
    current_state_.store(new_state);

    /* 复制回调列表后释放锁，避免回调重入导致死锁 */
    std::vector<Callback> cbs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cbs = callbacks_;
    }
    for (auto& cb : cbs) {
        cb(old, new_state);
    }
    return true;
}

void StateMachine::OnStateChanged(Callback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(std::move(cb));
}

bool StateMachine::IsValidTransition(AssistantState from, AssistantState to) {
    int fi = static_cast<int>(from);
    int ti = static_cast<int>(to);
    if (fi < 0 || fi >= 5 || ti < 0 || ti >= 5) return false;
    return kValidTransitions[fi][ti];
}
