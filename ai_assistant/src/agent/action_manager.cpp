/*
 * action_manager.cpp
 * Action 管理器实现
 *
 * v2.3: Action 是最简单的系统操作，keyword + LLM 触发，不回注
 */

#include "assistant/agent/action_manager.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

static const char* kTag = "[ActionManager]";

namespace agent {

/* 简易 JSON 解析 */
static std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        /* 逐字符扫描，正确处理 \" 转义引号 */
        size_t end = pos + 1;
        while (end < json.size()) {
            if (json[end] == '\\') { end += 2; }
            else if (json[end] == '"') { break; }
            else { end++; }
        }
        if (end >= json.size()) return "";
        return json.substr(pos + 1, end - pos - 1);
    }
    return "";
}

static std::vector<std::string> ExtractJsonArray(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos + search.size());
    if (pos == std::string::npos) return result;
    size_t end = json.find(']', pos);
    if (end == std::string::npos) return result;
    std::string arr = json.substr(pos + 1, end - pos - 1);
    size_t i = 0;
    while (i < arr.size()) {
        size_t q1 = arr.find('"', i);
        if (q1 == std::string::npos) break;
        size_t q2 = arr.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return result;
}

bool ActionManager::LoadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << kTag << " 无法打开: " << path << std::endl;
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    int count = 0;
    size_t pos = 0;
    while (true) {
        pos = content.find('{', pos);
        if (pos == std::string::npos) break;
        size_t end = content.find('}', pos);
        if (end == std::string::npos) break;

        std::string obj = content.substr(pos, end - pos + 1);
        pos = end + 1;

        ActionDef a;
        a.name = ExtractJsonString(obj, "name");
        a.description = ExtractJsonString(obj, "description");
        a.action = ExtractJsonString(obj, "action");
        a.category = ExtractJsonString(obj, "category");
        a.triggers = ExtractJsonArray(obj, "triggers");

        if (!a.name.empty() && !a.action.empty()) {
            actions_.push_back(a);
            count++;
        }
    }

    std::cout << kTag << " 已加载 " << count << " 个 action: " << path << std::endl;
    return true;
}

void ActionManager::RegisterHandler(const std::string& prefix, ActionHandler handler) {
    handlers_[prefix] = std::move(handler);
}

ActionResult ActionManager::Match(const std::string& text) const {
    ActionResult result;

    const ActionDef* best = nullptr;
    size_t best_len = 0;

    for (size_t i = 0; i < actions_.size(); i++) {
        for (size_t j = 0; j < actions_[i].triggers.size(); j++) {
            const std::string& trigger = actions_[i].triggers[j];
            if (text.find(trigger) != std::string::npos) {
                if (trigger.size() > best_len) {
                    best = &actions_[i];
                    best_len = trigger.size();
                }
            }
        }
    }

    if (best != nullptr) {
        std::cout << kTag << " 关键词匹配: \"" << text << "\" → "
                  << best->action << std::endl;
        return Execute(best->action);
    }

    return result;
}

ActionResult ActionManager::Execute(const std::string& action) const {
    ActionResult result;
    result.action = action;

    for (auto it = handlers_.begin(); it != handlers_.end(); ++it) {
        const std::string& prefix = it->first;
        if (action.find(prefix) == 0) {
            result.response = it->second(action);
            result.handled = true;
            return result;
        }
    }

    std::cout << kTag << " 未找到处理器: " << action << std::endl;
    return result;
}

std::string ActionManager::GetActionsDescription() const {
    std::ostringstream ss;
    for (size_t i = 0; i < actions_.size(); i++) {
        const ActionDef& a = actions_[i];
        ss << "- **" << a.action << "**";
        if (!a.triggers.empty()) {
            ss << " [触发词: ";
            for (size_t j = 0; j < a.triggers.size(); j++) {
                if (j > 0) ss << "、";
                ss << a.triggers[j];
            }
            ss << "]";
        }
        ss << ": " << a.description << "（调用后直接执行，不回传）\n";
    }
    return ss.str();
}

const ActionDef* ActionManager::FindAction(const std::string& action) const {
    for (size_t i = 0; i < actions_.size(); i++) {
        if (actions_[i].action == action) return &actions_[i];
    }
    return nullptr;
}

}  // namespace agent
