/*
 * agent_core.cpp
 * Agent 核心实现：Prompt 管理 + 对话上下文 + 语音偏好修改
 */

#include "assistant/agent/agent_core.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

static const char* kTag = "[AgentCore]";

namespace agent {

/* ========== 辅助函数 ========== */

static std::string TrimStr(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

static bool StrContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/* ========== Prompt 加载 ========== */

bool AgentCore::LoadPrompt(const std::string& prompt_path) {
    prompt_path_ = prompt_path;

    std::ifstream file(prompt_path);
    if (!file.is_open()) {
        std::cerr << kTag << " 无法打开 Prompt 文件: " << prompt_path << std::endl;
        return false;
    }

    std::string line;
    std::string current_section;
    std::ostringstream immutable_ss;
    std::ostringstream mutable_ss;
    bool in_immutable = false;
    bool in_mutable = false;

    while (std::getline(file, line)) {
        std::string trimmed = TrimStr(line);

        /* 检测区段标记 */
        if (trimmed.find("## [不可改动区]") == 0) {
            in_immutable = true;
            in_mutable = false;
            continue;
        }
        if (trimmed.find("## [可改动区]") == 0) {
            in_immutable = false;
            in_mutable = true;
            continue;
        }

        /* 跳过 frontmatter 和其他标题 */
        if (trimmed.empty() || trimmed[0] == '#' || trimmed == "---") {
            /* 保留空行在内容中 */
            if (in_immutable && !trimmed.empty() && trimmed[0] != '#' && trimmed != "---") {
                /* 这是内容行 */
            } else if (in_mutable && !trimmed.empty() && trimmed[0] != '#' && trimmed != "---") {
                /* 这是内容行 */
            } else {
                continue;
            }
        }

        if (in_immutable) {
            immutable_ss << line << "\n";
        } else if (in_mutable) {
            mutable_ss << line << "\n";
        }
    }

    immutable_section_ = immutable_ss.str();
    mutable_section_ = mutable_ss.str();
    mutable_items_ = ExtractListItems(mutable_section_);

    std::cout << kTag << " Prompt 已加载: " << prompt_path
              << " (不可改动区 " << immutable_section_.size() << " 字节"
              << ", 可改动区 " << mutable_items_.size() << " 条)" << std::endl;
    return true;
}

std::string AgentCore::GetSystemPrompt() const {
    std::ostringstream ss;
    ss << immutable_section_;
    if (!mutable_items_.empty()) {
        ss << "\n## 用户偏好\n";
        for (size_t i = 0; i < mutable_items_.size(); i++) {
            ss << "- " << mutable_items_[i] << "\n";
        }
    }
    return ss.str();
}

std::string AgentCore::GetImmutableSection() const {
    return immutable_section_;
}

std::string AgentCore::GetMutableSection() const {
    return mutable_section_;
}

/* ========== 对话上下文 ========== */

void AgentCore::AddTurn(const std::string& role, const std::string& content) {
    history_.push_back({role, content});
}

const std::vector<ConversationTurn>& AgentCore::GetHistory() const {
    return history_;
}

std::string AgentCore::BuildMessagesJson(const std::string& current_query,
                                          const std::string& system_prompt) const {
    std::ostringstream ss;
    ss << "[";

    /* System prompt：优先用外部传入的完整 Prompt */
    std::string sys_prompt = system_prompt.empty() ? GetSystemPrompt() : system_prompt;
    if (!sys_prompt.empty()) {
        ss << "{\"role\":\"system\",\"content\":\""
           << EscapeJson(sys_prompt) << "\"}";
    }

    /* 历史对话 */
    for (size_t i = 0; i < history_.size(); i++) {
        const ConversationTurn& turn = history_[i];
        if (ss.str().size() > 2) ss << ",";
        ss << "{\"role\":\"" << turn.role << "\",\"content\":\""
           << EscapeJson(turn.content) << "\"}";
    }

    /* 当前查询（仅当非空且未在历史中时添加） */
    if (!current_query.empty()) {
        if (ss.str().size() > 2) ss << ",";
        ss << "{\"role\":\"user\",\"content\":\""
           << EscapeJson(current_query) << "\"}";
    }

    ss << "]";
    return ss.str();
}

void AgentCore::ClearHistory() {
    history_.clear();
    std::cout << kTag << " 对话历史已清除" << std::endl;
}

size_t AgentCore::HistorySize() const {
    return history_.size();
}

/* ========== 可改动区语音修改 ========== */

PreferenceResult AgentCore::DetectAndApplyPreference(const std::string& text) {
    PreferenceResult result;

    /* 模式1: "添加偏好：xxx" */
    std::string add_prefixes[] = {"添加偏好：", "添加偏好:", "加上偏好：", "加上偏好:",
                                   "增加偏好：", "增加偏好:"};
    for (size_t i = 0; i < sizeof(add_prefixes)/sizeof(add_prefixes[0]); i++) {
        const std::string& prefix = add_prefixes[i];
        size_t pos = text.find(prefix);
        if (pos != std::string::npos) {
            std::string item = TrimStr(text.substr(pos + prefix.length()));
            if (!item.empty()) {
                AddMutableItem(item);
                result.modified = true;
                result.message = "好的，已添加偏好：" + item;
                return result;
            }
        }
    }

    /* 模式2: "删除偏好：xxx" */
    std::string del_prefixes[] = {"删除偏好：", "删除偏好:", "去掉偏好：", "去掉偏好:",
                                   "移除偏好：", "移除偏好:"};
    for (size_t i = 0; i < sizeof(del_prefixes)/sizeof(del_prefixes[0]); i++) {
        const std::string& prefix = del_prefixes[i];
        size_t pos = text.find(prefix);
        if (pos != std::string::npos) {
            std::string keyword = TrimStr(text.substr(pos + prefix.length()));
            if (!keyword.empty()) {
                RemoveMutableItem(keyword);
                result.modified = true;
                result.message = "好的，已删除包含「" + keyword + "」的偏好";
                return result;
            }
        }
    }

    /* 模式3: "以后xxx" 风格的隐式添加 */
    if (text.find("以后") != std::string::npos) {
        /* 提取"以后"后面的内容作为偏好 */
        size_t pos = text.find("以后");
        std::string item = TrimStr(text.substr(pos + strlen("以后")));
        /* 去掉句末的"吧"、"啊"等语气词 */
        if (!item.empty()) {
            /* 去掉常见的后缀语气词 */
            std::string suffixes[] = {"吧", "啊", "哦", "哟", "呢", "嘛", "哈"};
            for (size_t i = 0; i < sizeof(suffixes)/sizeof(suffixes[0]); i++) {
                if (item.size() >= suffixes[i].size() &&
                    item.substr(item.size() - suffixes[i].size()) == suffixes[i]) {
                    item = TrimStr(item.substr(0, item.size() - suffixes[i].size()));
                    break;
                }
            }
        }
        if (!item.empty() && item.size() < 100) {  /* 不要太长的偏好 */
            AddMutableItem(item);
            result.modified = true;
            result.message = "好的，已记住：以后" + item;
            return result;
        }
    }

    /* 模式4: "不要xxx了" */
    if (text.find("不要") != std::string::npos && text.find("了") != std::string::npos) {
        size_t start = text.find("不要") + strlen("不要");
        size_t end = text.find("了", start);
        if (end != std::string::npos && end > start) {
            std::string keyword = TrimStr(text.substr(start, end - start));
            if (!keyword.empty()) {
                RemoveMutableItem(keyword);
                result.modified = true;
                result.message = "好的，已去掉「" + keyword + "」相关偏好";
                return result;
            }
        }
    }

    return result;
}

bool AgentCore::AddMutableItem(const std::string& item) {
    /* 去重 */
    for (size_t i = 0; i < mutable_items_.size(); i++) {
        if (mutable_items_[i] == item) {
            std::cout << kTag << " 偏好已存在: " << item << std::endl;
            return true;
        }
    }
    mutable_items_.push_back(item);
    std::cout << kTag << " 已添加偏好: " << item << std::endl;
    return SavePrompt();
}

bool AgentCore::RemoveMutableItem(const std::string& keyword) {
    bool found = false;
    auto it = mutable_items_.begin();
    while (it != mutable_items_.end()) {
        if (StrContains(*it, keyword)) {
            std::cout << kTag << " 已删除偏好: " << *it << std::endl;
            it = mutable_items_.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    if (found) {
        return SavePrompt();
    }
    std::cout << kTag << " 未找到包含「" << keyword << "」的偏好" << std::endl;
    return false;
}

bool AgentCore::SavePrompt() {
    if (prompt_path_.empty()) return false;

    std::ifstream original(prompt_path_);
    if (!original.is_open()) return false;

    /* 读取完整的原始文件 */
    std::ostringstream file_content;
    std::string line;
    bool in_mutable = false;
    bool mutable_written = false;

    while (std::getline(original, line)) {
        std::string trimmed = TrimStr(line);

        if (trimmed.find("## [可改动区]") != std::string::npos) {
            /* 写入可改动区标记 */
            file_content << line << "\n";
            in_mutable = true;
            continue;
        }

        if (in_mutable && trimmed.find("## [") == 0) {
            /* 下一个区段开始，先写入可改动区内容 */
            in_mutable = false;
            if (!mutable_written) {
                for (size_t i = 0; i < mutable_items_.size(); i++) {
                    file_content << "- " << mutable_items_[i] << "\n";
                }
                file_content << "\n";
                mutable_written = true;
            }
            file_content << line << "\n";
            continue;
        }

        if (in_mutable && !mutable_written) {
            /* 跳过旧的可改动区内容，写入新的 */
            if (trimmed.empty() || trimmed[0] == '-') {
                continue;  /* 跳过旧条目 */
            }
        }

        file_content << line << "\n";
    }

    /* 如果可改动区在文件末尾，追加新内容 */
    if (in_mutable && !mutable_written) {
        for (size_t i = 0; i < mutable_items_.size(); i++) {
            file_content << "- " << mutable_items_[i] << "\n";
        }
    }

    /* 写回文件 */
    std::ofstream out(prompt_path_);
    if (!out.is_open()) {
        std::cerr << kTag << " 保存 Prompt 文件失败: " << prompt_path_ << std::endl;
        return false;
    }
    out << file_content.str();
    std::cout << kTag << " Prompt 已保存: " << prompt_path_ << std::endl;
    return true;
}

/* ========== 辅助 ========== */

std::vector<std::string> AgentCore::ExtractListItems(const std::string& section) {
    std::vector<std::string> items;
    std::istringstream ss(section);
    std::string line;
    while (std::getline(ss, line)) {
        std::string trimmed = TrimStr(line);
        if (trimmed.size() > 2 && trimmed[0] == '-' && trimmed[1] == ' ') {
            items.push_back(TrimStr(trimmed.substr(2)));
        }
    }
    return items;
}

std::string AgentCore::JoinListItems(const std::vector<std::string>& items) {
    std::ostringstream ss;
    for (size_t i = 0; i < items.size(); i++) {
        ss << "- " << items[i] << "\n";
    }
    return ss.str();
}

std::string AgentCore::EscapeJson(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += c; break;
        }
    }
    return result;
}

}  // namespace agent
