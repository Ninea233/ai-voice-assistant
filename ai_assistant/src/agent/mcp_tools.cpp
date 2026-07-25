/*
 * mcp_tools.cpp
 * MCP 工具调用框架实现 v2.3
 *
 * MCP JSON-RPC 2.0 协议 + 内置 <tool_call> 标记兼容
 * 工具来自 config/mcp_tools.json（全量发送给 LLM）
 */

#include "assistant/agent/mcp_tools.h"
#include "assistant/cloud/http_client.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

static const char* kTag = "[MCPTools]";

namespace agent {

/* ── JSON 解析辅助 ── */

static std::string TrimStr(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

std::string MCPTools::ExtractJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        /* 正确处理 \\" 转义引号 */
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

bool MCPTools::ExtractJsonBool(const std::string& json, const std::string& key, bool def) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return def;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return (pos + 4 <= json.size() && json.substr(pos, 4) == "true");
}

std::vector<std::string> MCPTools::ExtractJsonArray(const std::string& json, const std::string& key) {
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

std::vector<std::string> MCPTools::ExtractJsonObjects(const std::string& json, const std::string& array_key) {
    std::vector<std::string> result;
    std::string search = "\"" + array_key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos + search.size());
    if (pos == std::string::npos) return result;

    size_t depth = 0;
    size_t start = std::string::npos;
    for (size_t i = pos; i < json.size(); i++) {
        if (json[i] == '{') {
            if (depth == 0) start = i;
            depth++;
        } else if (json[i] == '}') {
            depth--;
            if (depth == 0 && start != std::string::npos) {
                result.push_back(json.substr(start, i - start + 1));
                start = std::string::npos;
            }
        } else if (json[i] == ']' && depth == 0) {
            break;
        }
    }
    return result;
}

/* ── 从 JSON 数组提取对象列表（顶层 "tools" 数组）── */

static std::vector<std::string> ExtractToolObjects(const std::string& content) {
    std::vector<std::string> result;
    std::string trimmed = TrimStr(content);

    size_t array_start = std::string::npos;

    /* 支持两种格式：
     * 1) JSON-RPC 2.0 数组格式： [...]
     * 2) 旧封装格式： {"tools": [...]} */
    if (!trimmed.empty() && trimmed[0] == '[') {
        /* 直接是数组 */
        array_start = content.find('[');
    } else {
        /* 查找 "tools" 键 */
        std::string search = "\"tools\"";
        size_t pos = content.find(search);
        if (pos != std::string::npos) {
            array_start = content.find('[', pos + search.size());
        }
    }

    if (array_start == std::string::npos) return result;

    /* 提取每个 {...} 对象 */
    size_t depth = 0;
    size_t start = std::string::npos;
    for (size_t i = array_start; i < content.size(); i++) {
        if (content[i] == '{') {
            if (depth == 0) start = i;
            depth++;
        } else if (content[i] == '}') {
            depth--;
            if (depth == 0 && start != std::string::npos) {
                result.push_back(content.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return result;
}

/* ── 初始化和注册 ── */

bool MCPTools::LoadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << kTag << " 无法打开: " << path << std::endl;
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    auto objects = ExtractToolObjects(content);
    /* 预分配容量，避免 push_back 时 vector 扩容导致 tool_map_ 指针失效 */
    tools_.reserve(objects.size());
    int count = 0;
    for (size_t i = 0; i < objects.size(); i++) {
        const std::string& obj = objects[i];

        MCPToolDef def;
        def.name = ExtractJsonString(obj, "name");
        if (def.name.empty()) continue;
        def.description = ExtractJsonString(obj, "description");

        /* 提取 inputSchema */
        std::string schema_search = "\"inputSchema\"";
        size_t sp = obj.find(schema_search);
        if (sp != std::string::npos) {
            size_t p_start = obj.find("{", sp);
            size_t p_end = obj.find("}", p_start);
            if (p_start != std::string::npos && p_end != std::string::npos) {
                std::string pobj = obj.substr(p_start, p_end - p_start + 1);
                /* 提取 properties */
                size_t pp = pobj.find("\"properties\"");
                if (pp != std::string::npos) {
                    size_t props_start = pobj.find("{", pp);
                    size_t props_end = pobj.find("}", props_start);
                    if (props_start != std::string::npos && props_end != std::string::npos) {
                        std::string props = pobj.substr(props_start, props_end - props_start + 1);
                        size_t ppos = 0;
                        while (true) {
                            size_t kq1 = props.find("\"", ppos);
                            if (kq1 == std::string::npos) break;
                            size_t kq2 = props.find("\"", kq1 + 1);
                            if (kq2 == std::string::npos) break;
                            std::string pname = props.substr(kq1 + 1, kq2 - kq1 - 1);
                            if (pname == "type" || pname.empty()) { ppos = kq2 + 1; continue; }

                            ParamSchema ps;
                            ps.name = pname;
                            size_t vo = props.find("{", kq2);
                            if (vo == std::string::npos) { ppos = kq2 + 1; continue; }
                            size_t ve = props.find("}", vo);
                            if (ve == std::string::npos) { ppos = kq2 + 1; continue; }
                            std::string prop = props.substr(vo, ve - vo + 1);
                            ps.type = ExtractJsonString(prop, "type");
                            ps.description = ExtractJsonString(prop, "description");
                            ps.default_value = ExtractJsonString(prop, "default");
                            if (ps.type.empty()) ps.type = "string";
                            def.params.push_back(ps);
                            ppos = ve + 1;
                        }
                    }
                }
                /* 提取 required */
                size_t rp = pobj.find("\"required\"");
                if (rp != std::string::npos) {
                    std::vector<std::string> reqs = ExtractJsonArray(pobj, "required");
                    for (size_t ri = 0; ri < def.params.size(); ri++) {
                        for (size_t rj = 0; rj < reqs.size(); rj++) {
                            if (def.params[ri].name == reqs[rj]) {
                                def.params[ri].required = true;
                                break;
                            }
                        }
                    }
                }
            }
        }

        /* 提取 implementation */
        std::string impl_search = "\"_implementation\"";
        size_t ip = obj.find(impl_search);
        if (ip != std::string::npos) {
            size_t is = obj.find("{", ip);
            size_t ie = obj.find("}", is);
            if (is != std::string::npos && ie != std::string::npos) {
                std::string impl = obj.substr(is, ie - is + 1);
                def.impl_type = ExtractJsonString(impl, "type");
                def.impl_url = ExtractJsonString(impl, "url");
                def.impl_method = ExtractJsonString(impl, "method");
                if (def.impl_method.empty()) def.impl_method = "GET";
                def.impl_handler = ExtractJsonString(impl, "handler");
            }
        }

        if (def.impl_type.empty()) def.impl_type = "builtin";

        tools_.push_back(def);
        tool_map_[def.name] = &tools_.back();
        std::cout << kTag << " 工具: " << def.name << " (" << def.impl_type << ")" << std::endl;
        count++;
    }

    std::cout << kTag << " 已加载 " << count << " 个 MCP 工具: " << path << std::endl;
    return true;
}

void MCPTools::RegisterBuiltin(const std::string& handler_name, BuiltinExecutor executor) {
    builtins_[handler_name] = std::move(executor);
    std::cout << kTag << " 注册内置处理器: " << handler_name << std::endl;
}

/* ── LLM Prompt 描述（全量发送）── */

std::string MCPTools::GetToolsDescription() const {
    std::ostringstream ss;
    for (size_t i = 0; i < tools_.size(); i++) {
        const MCPToolDef& tool = tools_[i];
        ss << "- **mcp." << tool.name << "(";
        for (size_t j = 0; j < tool.params.size(); j++) {
            if (j > 0) ss << ", ";
            ss << tool.params[j].name;
            if (tool.params[j].required) ss << "*";
        }
        ss << ")**: " << tool.description << "\n";
        if (!tool.params.empty()) {
            ss << "  参数：";
            for (size_t j = 0; j < tool.params.size(); j++) {
                if (j > 0) ss << "；";
                ss << tool.params[j].name
                   << "(" << tool.params[j].description << ")";
                if (tool.params[j].required) ss << "【必填】";
                else if (!tool.params[j].default_value.empty()) ss << "【默认" << tool.params[j].default_value << "】";
            }
            ss << "\n";
        }
    }
    return ss.str();
}

/* ── 工具调用解析 ── */

std::vector<ToolCall> MCPTools::ParseToolCalls(const std::string& text) const {
    std::vector<ToolCall> calls;
    const std::string kOpenTag = "<tool_call>";
    const std::string kCloseTag = "</tool_call>";

    size_t pos = 0;
    while (true) {
        size_t start = text.find(kOpenTag, pos);
        if (start == std::string::npos) break;
        start += kOpenTag.size();
        size_t end = text.find(kCloseTag, start);
        if (end == std::string::npos) break;

        std::string content = TrimStr(text.substr(start, end - start));
        pos = end + kCloseTag.size();

        size_t pipe = content.find('|');
        if (pipe == std::string::npos) continue;

        ToolCall call;
        call.name = TrimStr(content.substr(0, pipe));
        std::string args_str = TrimStr(content.substr(pipe + 1));

        if (!args_str.empty() && args_str[0] == '{') {
            size_t ap = 1;
            while (ap < args_str.size()) {
                size_t kq1 = args_str.find('"', ap);
                if (kq1 == std::string::npos || kq1 >= args_str.size() - 1) break;
                size_t kq2 = args_str.find('"', kq1 + 1);
                if (kq2 == std::string::npos) break;
                std::string key = args_str.substr(kq1 + 1, kq2 - kq1 - 1);
                size_t colon = args_str.find(':', kq2 + 1);
                if (colon == std::string::npos) break;
                size_t vq1 = args_str.find('"', colon + 1);
                if (vq1 == std::string::npos) break;
                size_t vq2 = args_str.find('"', vq1 + 1);
                if (vq2 == std::string::npos) break;
                call.arguments[key] = args_str.substr(vq1 + 1, vq2 - vq1 - 1);
                ap = vq2 + 1;
                while (ap < args_str.size() && (args_str[ap] == ',' || args_str[ap] == ' ')) ap++;
                if (ap < args_str.size() && args_str[ap] == '}') break;
            }
        }
        if (!call.name.empty()) calls.push_back(call);
    }
    return calls;
}

bool MCPTools::ContainsToolCall(const std::string& text) const {
    return text.find("<tool_call>") != std::string::npos;
}

/* ── 工具执行 ── */

ToolResult MCPTools::ExecuteTool(const ToolCall& call) {
    ToolResult result;
    std::string tool_name = call.name;
    if (tool_name.find("mcp.") == 0) tool_name = tool_name.substr(4);

    auto it = tool_map_.find(tool_name);
    if (it == tool_map_.end()) {
        result.success = false;
        result.error = "未找到 MCP 工具: " + call.name;
        return result;
    }

    const MCPToolDef& tool = *it->second;
    if (tool.impl_type == "http") {
        result = ExecuteHTTPTool(tool, call);
    } else {
        result = ExecuteBuiltinTool(tool, call);
    }

    if (result.success) {
        std::cout << kTag << " 执行成功: " << call.name << std::endl;
    }
    return result;
}

ToolResult MCPTools::ExecuteHTTPTool(const MCPToolDef& tool, const ToolCall& call) {
    ToolResult result;
    if (tool.impl_url.empty()) {
        result.success = false;
        result.error = tool.name + " 未配置 API 地址";
        return result;
    }

    std::string url = tool.impl_url;
    for (size_t i = 0; i < tool.params.size(); i++) {
        std::string placeholder = "{" + tool.params[i].name + "}";
        std::string value;
        auto it = call.arguments.find(tool.params[i].name);
        if (it != call.arguments.end()) value = it->second;
        else value = tool.params[i].default_value;
        for (size_t j = 0; j < value.size(); j++) {
            if (value[j] == ' ') { value.replace(j, 1, "%20"); j += 2; }
        }
        size_t p = url.find(placeholder);
        if (p != std::string::npos) url.replace(p, placeholder.size(), value);
    }

    std::cout << kTag << " [HTTP] " << tool.impl_method << " " << url << std::endl;

    /* 特殊处理 worldtimeapi 响应 */
    HttpClient http;
    std::map<std::string, std::string> headers;
    HttpResponse resp;
    bool ok = (tool.impl_method == "GET") ? http.Get(url, headers, resp)
                                          : http.Post(url, "", headers, resp);

    if (ok && resp.status_code == 200 && !resp.body.empty()) {
        /* 尝试解析 JSON 中的关键字段 */
        std::string datetime = ExtractJsonString(resp.body, "datetime");
        std::string abbreviation = ExtractJsonString(resp.body, "abbreviation");
        if (!datetime.empty()) {
            /* 格式化: "2025-07-25T10:30:00+08:00" → "2025年07月25日 10:30:00" */
            std::string year = datetime.substr(0, 4);
            std::string month = datetime.substr(5, 2);
            std::string day = datetime.substr(8, 2);
            std::string hour = datetime.substr(11, 2);
            std::string min = datetime.substr(14, 2);
            std::string sec = datetime.substr(17, 2);
            std::string tz = abbreviation.empty() ? "UTC" : abbreviation;
            result.content = year + "\xe5\xb9\xb4" + month + "\xe6\x9c\x88"
                           + day + "\xe6\x97\xa5 " + hour + ":" + min + ":" + sec
                           + " (" + tz + ")";
        } else {
            /* 非 worldtimeapi 响应，直接返回（截断过长内容） */
            if (resp.body.size() > 256) {
                result.content = resp.body.substr(0, 253) + "...";
            } else {
                result.content = resp.body;
            }
        }
        result.success = true;
    } else {
        result.success = false;
        result.error = tool.name + " 服务暂不可用";
    }
    return result;
}

ToolResult MCPTools::ExecuteBuiltinTool(const MCPToolDef& tool, const ToolCall& call) {
    auto it = builtins_.find(tool.impl_handler);
    if (it == builtins_.end()) {
        ToolResult r;
        r.success = false;
        r.error = "未找到内置处理器: " + tool.impl_handler;
        return r;
    }
    return it->second(call);
}

/* ── 查询 ── */

const MCPToolDef* MCPTools::FindTool(const std::string& name) const {
    std::string n = name;
    if (n.find("mcp.") == 0) n = n.substr(4);
    auto it = tool_map_.find(n);
    return (it != tool_map_.end()) ? it->second : nullptr;
}

}  // namespace agent
