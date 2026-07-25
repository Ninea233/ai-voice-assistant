/*
 * action_manager.h
 * Action 管理器 v2.3
 *
 * Action 是最高优先级的系统指令（keyword + LLM 均可触发，一定不回注）。
 * 用于直接的硬件/系统操作：开关灯、空调控制、音量调节、休眠等。
 *
 * 配置来自 config/actions.json。
 */

#ifndef AI_ASSISTANT_ACTION_MANAGER_H
#define AI_ASSISTANT_ACTION_MANAGER_H

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace agent {

/* Action 定义 */
struct ActionDef {
    std::string name;
    std::string description;
    std::vector<std::string> triggers;
    std::string action;          /* action.light_on 等 */
    std::string category;
};

/* Action 执行结果 */
struct ActionResult {
    bool handled = false;
    std::string response;
    std::string action;
};

class ActionManager {
public:
    using ActionHandler = std::function<std::string(const std::string& action)>;

    ActionManager() = default;
    ~ActionManager() = default;

    /* 从 JSON 文件加载 */
    bool LoadFromFile(const std::string& path);

    /* 注册处理器 */
    void RegisterHandler(const std::string& prefix, ActionHandler handler);

    /* 关键词匹配（所有 action 都参与） */
    ActionResult Match(const std::string& text) const;

    /* 执行指定 action */
    ActionResult Execute(const std::string& action) const;

    /* 获取所有 action 描述（供 LLM system prompt，全量发送） */
    std::string GetActionsDescription() const;

    /* 根据 action 查找定义 */
    const ActionDef* FindAction(const std::string& action) const;

    /* Action 数量 */
    size_t Count() const { return actions_.size(); }

    /* 获取所有 Action 定义（用于原生 function calling API） */
    const std::vector<ActionDef>& GetActions() const { return actions_; }

private:
    std::vector<ActionDef> actions_;
    std::map<std::string, ActionHandler> handlers_;
};

}  // namespace agent

#endif /* AI_ASSISTANT_ACTION_MANAGER_H */
