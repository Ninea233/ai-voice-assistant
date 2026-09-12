/*
 * mcp_tools.h
 * MCP（Model Context Protocol）工具调用框架 v2.3.1
 *
 * 工具调用框架：工具定义的字段格式参考 MCP（Model Context Protocol）
 *   name / description / inputSchema(JSON Schema) / implementation
 *
 * ⚠️ 注意：**未实现 JSON-RPC 2.0 协议**。没有 initialize/tools/list/tools/call
 *    握手；ExecuteTool() 返回的是纯文本 ToolResult，不组装 JSON-RPC 响应。
 *    下面保留的 JsonRpcRequest/JsonRpcResponse 结构体目前无任何使用（预留）。
 *
 * 实际交互方式：工具定义转成 OpenAI tools 数组发给 LLM，
 *    调用结果转成内部 <tool_call> 标记解析，执行后以纯文本回注 LLM。
 *
 * 工具定义来自 config/mcp_tools.json，HTTP 类型无需改 C++。
 * MCP 工具仅 LLM 可调用，结果一定回注 LLM。
 */

#ifndef AI_ASSISTANT_MCP_TOOLS_H
#define AI_ASSISTANT_MCP_TOOLS_H

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace agent {

/* ── 工具定义 / 调用结构体 ── */

/* 输入参数 Schema（JSON Schema 子集） */
struct ParamSchema {
    std::string name;
    std::string type;
    std::string description;
    std::string default_value;
    bool required = false;
};

/* MCP 工具定义 */
struct MCPToolDef {
    std::string name;            /* get_weather, get_time 等 */
    std::string description;
    std::vector<ParamSchema> params;

    /* 实现方式 */
    std::string impl_type;       /* "builtin" | "http" */
    std::string impl_url;
    std::string impl_method;     /* "GET" | "POST" */
    std::string impl_handler;    /* builtin: 处理器名称 */
};

/* 工具调用请求 */
struct ToolCall {
    std::string name;            /* mcp.get_weather, skill.daily_briefing, action.light_on */
    std::map<std::string, std::string> arguments;
};

/* 工具调用结果 */
struct ToolResult {
    bool success = false;
    std::string content;
    std::string error;
};

/* JSON-RPC 2.0 请求（预留结构体，当前无使用者） */
struct JsonRpcRequest {
    std::string jsonrpc = "2.0";
    std::string method;          /* "tools/list", "tools/call", "initialize" */
    std::string params;          /* 原始 JSON */
    int id = 1;
};

/* JSON-RPC 2.0 响应（预留结构体，当前无使用者） */
struct JsonRpcResponse {
    std::string jsonrpc = "2.0";
    std::string result;          /* JSON 字符串 */
    std::string error;           /* 错误 JSON */
    int id = 1;
};

class MCPTools {
public:
    using BuiltinExecutor = std::function<ToolResult(const ToolCall& call)>;

    MCPTools() = default;
    ~MCPTools() = default;

    /* ── 初始化 ── */
    bool LoadFromFile(const std::string& path);
    void RegisterBuiltin(const std::string& handler_name, BuiltinExecutor executor);

    /* ── LLM 交互 API ── */

    /* 获取所有工具描述（供 LLM system prompt，全量发送）
     * 格式：- **mcp.tool_name(param1, param2)**: description (inputSchema) */
    std::string GetToolsDescription() const;

    /* 从 LLM 文本中解析工具调用
     * 格式: <tool_call>mcp.tool_name|{"key":"value"}</tool_call> */
    std::vector<ToolCall> ParseToolCalls(const std::string& text) const;
    bool ContainsToolCall(const std::string& text) const;

    /* 执行工具调用 */
    ToolResult ExecuteTool(const ToolCall& call);

    /* ── 查询 ── */
    const MCPToolDef* FindTool(const std::string& name) const;
    size_t ToolCount() const { return tools_.size(); }
    int MaxRounds() const { return max_rounds_; }
    void SetMaxRounds(int rounds) { max_rounds_ = rounds; }

    /* 获取所有工具定义（用于原生 function calling API） */
    const std::vector<MCPToolDef>& GetTools() const { return tools_; }

    /* JSON 解析工具（公有，供内置处理器使用） */
    static std::string ExtractJsonString(const std::string& json, const std::string& key);
    static std::vector<std::string> ExtractJsonArray(const std::string& json, const std::string& key);
    static bool ExtractJsonBool(const std::string& json, const std::string& key, bool def = false);
    /* 从 JSON 对象数组中提取每个对象（支持嵌套） */
    static std::vector<std::string> ExtractJsonObjects(const std::string& json, const std::string& array_key);

private:
    /* 内部执行 */
    ToolResult ExecuteHTTPTool(const MCPToolDef& tool, const ToolCall& call);
    ToolResult ExecuteBuiltinTool(const MCPToolDef& tool, const ToolCall& call);

    std::vector<MCPToolDef> tools_;
    std::map<std::string, MCPToolDef*> tool_map_;
    std::map<std::string, BuiltinExecutor> builtins_;
    int max_rounds_ = 3;
};

}  // namespace agent

#endif
