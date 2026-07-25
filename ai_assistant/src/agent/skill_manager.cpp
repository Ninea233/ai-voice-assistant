/*
 * skill_manager.cpp
 * Skill 管理器实现 v2.3
 *
 * 加载 skills/ 目录下每个子文件夹中的 SKILL.md 文件。
 * SKILL.md 格式：YAML frontmatter (---...---) + Markdown 正文。
 */

#include "assistant/agent/skill_manager.h"
#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

static const char* kTag = "[SkillManager]";

namespace agent {

/* ── YAML frontmatter 解析 ── */

std::string SkillManager::ExtractYamlValue(const std::string& frontmatter, const std::string& key) {
    std::string search = key + ":";
    size_t pos = 0;
    while (pos < frontmatter.size()) {
        size_t found = frontmatter.find(search, pos);
        if (found == std::string::npos) break;
        /* 确保是行首或前一个字符是换行 */
        if (found > 0 && frontmatter[found - 1] != '\n') {
            pos = found + 1;
            continue;
        }
        size_t val_start = found + search.size();
        /* 跳过空白 */
        while (val_start < frontmatter.size()
               && (frontmatter[val_start] == ' ' || frontmatter[val_start] == '\t'))
            val_start++;
        /* 值到行尾 */
        size_t val_end = val_start;
        while (val_end < frontmatter.size() && frontmatter[val_end] != '\n'
               && frontmatter[val_end] != '\r')
            val_end++;
        std::string value = frontmatter.substr(val_start, val_end - val_start);
        /* 去除首尾空白和引号 */
        while (!value.empty() && (value.front() == '"' || value.front() == ' '))
            value.erase(0, 1);
        while (!value.empty() && (value.back() == '"' || value.back() == ' '))
            value.pop_back();
        return value;
    }
    return "";
}

std::vector<std::string> SkillManager::ExtractYamlArray(const std::string& frontmatter,
                                                         const std::string& key) {
    std::vector<std::string> result;
    std::string search = key + ":";
    size_t pos = 0;
    while (pos < frontmatter.size()) {
        size_t found = frontmatter.find(search, pos);
        if (found == std::string::npos) break;
        /* 确保行首 */
        if (found > 0 && frontmatter[found - 1] != '\n') {
            pos = found + 1;
            continue;
        }
        /* 跳到下一行找 "- xxx" 条目 */
        size_t next_line = frontmatter.find('\n', found + search.size());
        while (next_line != std::string::npos) {
            size_t line_start = next_line + 1;
            if (line_start >= frontmatter.size()) break;
            /* 跳过空白 */
            while (line_start < frontmatter.size()
                   && (frontmatter[line_start] == ' ' || frontmatter[line_start] == '\t'))
                line_start++;
            if (line_start >= frontmatter.size()) break;
            if (frontmatter[line_start] == '-') {
                size_t item_start = line_start + 1;
                while (item_start < frontmatter.size()
                       && (frontmatter[item_start] == ' ' || frontmatter[item_start] == '\t'))
                    item_start++;
                size_t item_end = item_start;
                while (item_end < frontmatter.size()
                       && frontmatter[item_end] != '\n' && frontmatter[item_end] != '\r')
                    item_end++;
                std::string item = frontmatter.substr(item_start, item_end - item_start);
                while (!item.empty() && item.back() == ' ') item.pop_back();
                if (!item.empty()) result.push_back(item);
                next_line = frontmatter.find('\n', item_end);
            } else {
                break; /* 不是 - 开头，说明数组结束 */
            }
        }
        return result;
    }
    return result;
}

int SkillManager::ExtractYamlInt(const std::string& frontmatter, const std::string& key, int def) {
    std::string val = ExtractYamlValue(frontmatter, key);
    if (val.empty()) return def;
    return std::atoi(val.c_str());
}

/* ── 目录加载 ── */

bool SkillManager::LoadFromDirectory(const std::string& dir_path) {
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) {
        std::cerr << kTag << " 无法打开 skills 目录: " << dir_path << std::endl;
        return false;
    }

    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        /* 跳过 . 和 .. */
        if (entry->d_name[0] == '.') continue;

        std::string subdir = dir_path + "/" + entry->d_name;
        struct stat st;
        if (stat(subdir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        /* 查找 SKILL.md */
        std::string skill_file = subdir + "/SKILL.md";
        std::ifstream file(skill_file);
        if (!file.is_open()) {
            std::cout << kTag << " 跳过 " << subdir << "（无 SKILL.md）" << std::endl;
            continue;
        }

        std::stringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();

        /* 解析 YAML frontmatter */
        std::string frontmatter;
        std::string body;
        if (content.size() >= 4 && content[0] == '-' && content[1] == '-' && content[2] == '-') {
            /* 找结束的 --- */
            size_t end_fm = content.find("---", 3);
            if (end_fm != std::string::npos) {
                frontmatter = content.substr(3, end_fm - 3);
                body = content.substr(end_fm + 3);
            } else {
                body = content;
            }
        } else {
            body = content;
        }

        SkillDef skill;
        skill.name = ExtractYamlValue(frontmatter, "name");
        skill.display_name = skill.name;
        skill.description = ExtractYamlValue(frontmatter, "description");
        skill.action = "skill." + skill.name;
        skill.category = ExtractYamlValue(frontmatter, "category");
        skill.mcp_tools = ExtractYamlArray(frontmatter, "tools");
        skill.priority = ExtractYamlInt(frontmatter, "priority", 0);
        skill.body = body;
        skill.folder_path = subdir;

        if (skill.name.empty()) {
            std::cout << kTag << " 跳过 " << skill_file << "（无 name）" << std::endl;
            continue;
        }

        skills_.push_back(skill);
        std::cout << kTag << " 已加载 skill: " << skill.action
                  << " (" << skill.description.substr(0, 40) << "...)"
                  << " [mcp_deps: " << skill.mcp_tools.size() << "]"
                  << std::endl;
        count++;
    }

    closedir(dir);
    std::cout << kTag << " 已加载 " << count << " 个 skill: " << dir_path << std::endl;
    return true;
}

/* ── 处理器 ── */

void SkillManager::RegisterHandler(const std::string& prefix, SkillHandler handler) {
    handlers_[prefix] = std::move(handler);
}

SkillResult SkillManager::Execute(const std::string& action) const {
    SkillResult result;
    result.action = action;

    /* 查找完整内容 */
    const SkillDef* skill = FindSkill(action);
    if (skill) {
        result.full_body = skill->body;
    }

    /* 查找处理器执行 */
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

/* ── 描述生成 ── */

std::string SkillManager::GetSkillsSummary() const {
    std::ostringstream ss;
    for (size_t i = 0; i < skills_.size(); i++) {
        const SkillDef& s = skills_[i];
        ss << "- **" << s.action << "**: " << s.description;
        if (!s.mcp_tools.empty()) {
            ss << " [内部工具: ";
            for (size_t j = 0; j < s.mcp_tools.size(); j++) {
                if (j > 0) ss << ", ";
                ss << s.mcp_tools[j];
            }
            ss << "]";
        }
        ss << "（仅 LLM 可调，调用后返回详细结果）\n";
    }
    return ss.str();
}

std::string SkillManager::GetSkillBody(const std::string& action) const {
    const SkillDef* s = FindSkill(action);
    if (s) return s->body;
    return "";
}

const SkillDef* SkillManager::FindSkill(const std::string& action) const {
    for (size_t i = 0; i < skills_.size(); i++) {
        if (skills_[i].action == action) return &skills_[i];
    }
    return nullptr;
}

}  // namespace agent
