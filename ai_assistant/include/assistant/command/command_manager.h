/*
 * command_manager.h
 * 键值对指令集管理器
 *
 * 读取 commands.json（key-value 格式）,
 * 将 ASR 文本与命令 Key 做子串匹配，最长匹配优先。
 *
 * 匹配流程:
 *   用户语音 → ASR 文本 → 检查敏感词 → 匹配指令 Key → 执行 Value 动作
 *   → 无匹配 → 走 LLM 通用对话
 */

#ifndef AI_ASSISTANT_COMMAND_MANAGER_H
#define AI_ASSISTANT_COMMAND_MANAGER_H

#include <map>
#include <string>
#include <vector>

/* 命令条目（键值对） */
using CommandMap = std::map<std::string, std::string>;

class CommandManager {
public:
    CommandManager() = default;
    ~CommandManager() = default;

    /* 从 JSON 文件加载指令集 */
    bool LoadFromFile(const std::string& path);

    /* 匹配文本：返回匹配到的 Value（动作名），未匹配返回空字符串 */
    std::string Match(const std::string& text) const;

    /* 获取所有指令 */
    const CommandMap& Commands() const { return commands_; }

private:
    CommandMap commands_;
};

#endif /* AI_ASSISTANT_COMMAND_MANAGER_H */
