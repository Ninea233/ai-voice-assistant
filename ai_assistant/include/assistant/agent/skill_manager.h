/*
 * skill_manager.h
 * Skill 管理器 v2.3
 *
 * 每个 Skill 是一个文件夹，包含 SKILL.md 文件（YAML frontmatter + Markdown 正文）。
 * 可选子目录：scripts/、references/、assets/。
 *
 * Skill 行为：
 *   - 仅 LLM 可触发（不参与关键词匹配）
 *   - 一定回注 LLM（调用后至少发送内部执行结果）
 *   - Phase 1: 发送 SKILL.md frontmatter description 给 LLM（摘要）
 *   - Phase 2: LLM 调用后加载完整 SKILL.md 正文执行
 */

#ifndef AI_ASSISTANT_SKILL_MANAGER_H
#define AI_ASSISTANT_SKILL_MANAGER_H

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace agent {

/* Skill 定义 */
struct SkillDef {
    std::string name;               /* daily_briefing 等 */
    std::string display_name;       /* 显示名称（如"今日简报"） */
    std::string description;        /* LLM 摘要描述 */
    std::string action;             /* skill.daily_briefing */
    std::string category;
    std::vector<std::string> mcp_tools;  /* 依赖的 MCP 工具列表 */
    int priority = 0;               /* 优先级 */
    std::string body;               /* SKILL.md 正文（Phase 2 发送） */
    std::string folder_path;        /* 技能文件夹路径 */
};

/* Skill 执行结果 */
struct SkillResult {
    bool handled = false;
    std::string response;
    std::string action;
    std::string full_body;          /* 完整 SKILL.md 内容 */
};

class SkillManager {
public:
    using SkillHandler = std::function<std::string(const std::string& action)>;

    SkillManager() = default;
    ~SkillManager() = default;

    /* 从 skills/ 目录加载所有 SKILL.md 文件 */
    bool LoadFromDirectory(const std::string& dir_path);

    /* 注册处理器 */
    void RegisterHandler(const std::string& prefix, SkillHandler handler);

    /* 执行指定 Skill（加载完整 SKILL.md） */
    SkillResult Execute(const std::string& action) const;

    /* 获取所有 Skill 的摘要描述（供 LLM system prompt，仅发送 description）
     * 格式：- **skill.xxx**: description（仅 LLM 可调，调用后加载详细指令） */
    std::string GetSkillsSummary() const;

    /* 获取指定 Skill 的完整内容（Phase 2） */
    std::string GetSkillBody(const std::string& action) const;

    /* 根据 action 查找 */
    const SkillDef* FindSkill(const std::string& action) const;

    /* 数量 */
    size_t Count() const { return skills_.size(); }

    /* 获取所有 Skill 定义（用于原生 function calling API） */
    const std::vector<SkillDef>& GetSkills() const { return skills_; }

private:
    /* 解析 YAML frontmatter */
    static std::string ExtractYamlValue(const std::string& frontmatter, const std::string& key);
    static std::vector<std::string> ExtractYamlArray(const std::string& frontmatter, const std::string& key);
    static int ExtractYamlInt(const std::string& frontmatter, const std::string& key, int def = 0);

    std::vector<SkillDef> skills_;
    std::map<std::string, SkillHandler> handlers_;
};

}  // namespace agent

#endif
