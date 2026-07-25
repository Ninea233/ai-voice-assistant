/*
 * agent_core.h
 * Agent 核心：全局 Prompt 管理 + 可改动区语音修改 + 对话上下文
 *
 * 借鉴 Agent 设计思想，通过全局 Prompt 统一控制助手行为。
 * Prompt 文件（agent_prompt.md）分为：
 *   [不可改动区] — 核心约束（字数限制、称呼、名字等）
 *   [可改动区] — 用户偏好（可通过语音交互增删）
 *
 * 对话上下文在一次唤醒周期内保留，休眠时清除。
 */

#ifndef AI_ASSISTANT_AGENT_CORE_H
#define AI_ASSISTANT_AGENT_CORE_H

#include <functional>
#include <string>
#include <vector>

namespace agent {

/* 单轮对话记录 */
struct ConversationTurn {
    std::string role;     /* "user" / "assistant" / "system" */
    std::string content;
};

/* 偏好修改结果 */
struct PreferenceResult {
    bool modified = false;
    std::string message;  /* 给用户的反馈文本 */
};

class AgentCore {
public:
    AgentCore() = default;
    ~AgentCore() = default;

    /* 加载 agent_prompt.md，解析不可改动区和可改动区 */
    bool LoadPrompt(const std::string& prompt_path);

    /* 获取完整的系统提示词（不可改动区 + 可改动区） */
    std::string GetSystemPrompt() const;

    /* 获取不可改动区内容 */
    std::string GetImmutableSection() const;

    /* 获取可改动区内容 */
    std::string GetMutableSection() const;

    /* ========== 对话上下文管理 ========== */

    /* 添加一轮对话 */
    void AddTurn(const std::string& role, const std::string& content);

    /* 获取对话历史（不含系统提示词） */
    const std::vector<ConversationTurn>& GetHistory() const;

    /*
     * 构建完整的 messages 数组 JSON 字符串。
     * system_prompt: 为空时使用内部 GetSystemPrompt()
     */
    std::string BuildMessagesJson(const std::string& current_query,
                                  const std::string& system_prompt = "") const;

    /* 清除对话历史（休眠时调用） */
    void ClearHistory();

    /* 获取对话轮数 */
    size_t HistorySize() const;

    /* ========== 可改动区语音修改 ========== */

    /*
     * 检测用户语音中是否包含偏好修改意图。
     * 支持的模式：
     *   "添加偏好：xxx" / "加上xxx" / "以后xxx"
     *   "删除偏好：xxx" / "不要xxx了" / "去掉xxx"
     *   "修改偏好：xxx改成yyy"
     *
     * 返回 PreferenceResult，modified=true 表示已处理。
     */
    PreferenceResult DetectAndApplyPreference(const std::string& text);

    /* 直接添加一条偏好到可改动区 */
    bool AddMutableItem(const std::string& item);

    /* 删除一条偏好 */
    bool RemoveMutableItem(const std::string& keyword);

    /* 保存 prompt 文件 */
    bool SavePrompt();

private:
    /* JSON 转义辅助 */
    static std::string EscapeJson(const std::string& s);

    /* 从文本中提取列表项（以 "- " 开头的行） */
    static std::vector<std::string> ExtractListItems(const std::string& section);

    /* 将列表项拼接为区段文本 */
    static std::string JoinListItems(const std::vector<std::string>& items);

    std::string prompt_path_;
    std::string immutable_section_;  /* 不可改动区原始文本 */
    std::string mutable_section_;    /* 可改动区原始文本 */
    std::vector<std::string> mutable_items_;  /* 可改动区解析后的列表项 */

    std::vector<ConversationTurn> history_;
};

}  // namespace agent

#endif /* AI_ASSISTANT_AGENT_CORE_H */
