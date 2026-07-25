/*
 * sensitive_filter.h
 * 敏感词过滤器
 *
 * 使用 Aho-Corasick 自动机实现多模式字符串匹配。
 * ASR 文本转写后，先经过敏感词过滤，再进入指令匹配或 LLM。
 *
 * 支持三种动作:
 *   SILENCE - 静默忽略（假装没听到）
 *   REPLACE - 替换为 ***
 *   ALERT   - 报警记录（写入日志和记忆）
 */

#ifndef AI_ASSISTANT_SENSITIVE_FILTER_H
#define AI_ASSISTANT_SENSITIVE_FILTER_H

#include <memory>
#include <string>
#include <vector>

enum class SensitiveAction {
    SILENCE,  /* 静默忽略 */
    REPLACE,  /* 替换为 *** */
    ALERT,    /* 报警记录 */
};

/* 匹配结果 */
struct SensitiveMatch {
    std::string keyword;
    SensitiveAction action;
    size_t position; /* 在文本中的起始位置 */
};

/* Aho-Corasick 自动机节点 */
struct ACTrieNode;

class SensitiveFilter {
public:
    SensitiveFilter();
    ~SensitiveFilter();

    /* 从文件加载敏感词列表 */
    bool LoadFromFile(const std::string& path);

    /* 添加单个敏感词 */
    void AddWord(const std::string& word, SensitiveAction action = SensitiveAction::REPLACE);

    /* 检测文本，返回所有匹配 */
    std::vector<SensitiveMatch> Detect(const std::string& text) const;

    /* 替换文本中的敏感词为 replacement */
    std::string Filter(const std::string& text,
                       const std::string& replacement = "***") const;

    /* 是否包含敏感词 */
    bool ContainsSensitive(const std::string& text) const;

    /* 获取默认动作 */
    SensitiveAction DefaultAction() const { return default_action_; }
    void SetDefaultAction(SensitiveAction action) { default_action_ = action; }

private:
    /* 构建自动机（失败指针） */
    void Build();

    std::unique_ptr<ACTrieNode> root_;
    bool built_ = false;
    SensitiveAction default_action_ = SensitiveAction::REPLACE;
};

#endif /* AI_ASSISTANT_SENSITIVE_FILTER_H */
