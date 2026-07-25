/*
 * llm_client.h
 * LLM（大语言模型）客户端接口
 *
 * 面向讯飞星火 Spark-X2 HTTP API，支持流式和非流式对话。
 * 调试模式下保存请求和响应到文件。
 */

#ifndef AI_ASSISTANT_LLM_CLIENT_H
#define AI_ASSISTANT_LLM_CLIENT_H

#include <functional>
#include <string>
#include <vector>

class LLMClient {
public:
    /* 工具定义（OpenAI function calling 格式） */
    struct ToolDef {
        std::string name;               /* mcp.get_time, action.light_on 等 */
        std::string description;
        std::string parameters_json;    /* JSON Schema 字符串 */
    };

    virtual ~LLMClient() = default;

    /* 初始化（讯飞星火 API 凭证 + 调试模式） */
    virtual bool Initialize(const std::string& app_id,
                            const std::string& api_key,
                            const std::string& api_secret,
                            bool debug_mode = false) = 0;

    /* 发送聊天查询（流式回复通过回调返回） */
    virtual bool Chat(const std::string& query) = 0;

    /* 预设完整的 messages JSON（含 system + history + user）。
     * 设置后 Chat() 将使用此 JSON 而非从 system_prompt + query 构建。
     * 调用 Chat("") 即可发送完整上下文。 */
    virtual void SetMessages(const std::string& messages_json) = 0;

    /* 设置系统提示词 */
    virtual void SetSystemPrompt(const std::string& prompt) = 0;

    /* 设置 API 地址（覆盖默认值） */
    virtual void SetApiUrl(const std::string& url) = 0;

    /* 设置工具定义列表（OpenAI function calling 格式）。
     * 设置后 Chat() 会在请求中包含 tools 参数。
     * 当 API 返回 tool_calls 时，自动转换为 <tool_call> 标记格式。 */
    virtual void SetTools(const std::vector<ToolDef>& tools) = 0;

    /* 结果回调 */
    using ResultCallback = std::function<void(const std::string& text, bool is_final)>;
    void OnResult(ResultCallback cb) { result_callback_ = std::move(cb); }

    /* 设置调试模式 */
    virtual void SetDebugMode(bool enable) = 0;

protected:
    ResultCallback result_callback_;
};

#endif /* AI_ASSISTANT_LLM_CLIENT_H */
