/*
 * llm_client.cpp
 * LLM 客户端 — 讯飞星火 Spark-X2，纯同步 HTTP POST
 */

#include "assistant/cloud/llm_client.h"
#include "assistant/cloud/http_client.h"

#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>

static const char* kTag = "[LLMClient]";

/* JSON 值提取 */
static std::string ExtractContent(const std::string& json) {
    size_t cp = json.find("\"choices\"");
    if (cp == std::string::npos) cp = 0;

    size_t mp = json.find("\"message\"", cp);
    if (mp != std::string::npos) {
        size_t sp = mp + 9;
        size_t c1 = json.find("\"content\":\"", sp);
        size_t c2 = json.find("\"content\": \"", sp);
        size_t pp = (c1 != std::string::npos) ? c1 : c2;
        if (pp != std::string::npos) {
            pp += 11;
            if (pp < json.size() && json[pp - 2] == ' ') pp++;
            else if (pp < json.size() && json[pp - 1] != '\"') {
                pp = json.find('\"', pp);
                if (pp != std::string::npos) pp++;
            }
            size_t ep = pp;
            while (ep < json.size()) { if (json[ep] == '\\') ep += 2; else if (json[ep] == '\"') break; else ep++; }
            if (ep > pp && ep <= json.size()) {
                std::string r;
                for (size_t i = pp; i < ep; i++) {
                    if (json[i] == '\\' && i + 1 < ep) {
                        char n = json[++i];
                        r += (n=='n')?'\n':(n=='t')?'\t':(n=='\\')?'\\':(n=='\"')?'\"':n;
                    } else r += json[i];
                }
                return r;
            }
        }
    }
    /* 全局回退 */
    std::string s = "\"content\":\"";
    size_t p = json.find(s);
    if (p == std::string::npos) { s = "\"content\": \""; p = json.find(s); }
    if (p == std::string::npos) return "";
    p += s.size();
    size_t e = p;
    while (e < json.size()) { if (json[e] == '\\') e += 2; else if (json[e] == '\"') break; else e++; }
    if (e >= json.size()) return "";
    std::string r;
    for (size_t i = p; i < e; i++) {
        if (json[i] == '\\' && i + 1 < e) { char n = json[++i]; r += (n=='n')?'\n':(n=='t')?'\t':(n=='\\')?'\\':(n=='\"')?'\"':n; }
        else r += json[i];
    }
    return r;
}

static std::string ExtractError(const std::string& json) {
    size_t p = json.find("\"error\"");
    if (p == std::string::npos) return "";
    size_t m = json.find("\"message\"", p);
    if (m == std::string::npos) return "";
    size_t c = json.find('\"', m + 10);
    if (c == std::string::npos) return "";
    size_t e = json.find('\"', c + 1);
    return (e != std::string::npos) ? json.substr(c + 1, e - c - 1) : "";
}

/* ========== 解析 tool_calls 辅助函数 ========== */

/* 在 JSON 对象中提取指定 key 的字符串值（支持转义引号 \"） */
static std::string ExtractJsonStr(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos + search.size());
    if (colon == std::string::npos) return "";
    colon++;
    while (colon < json.size() && (json[colon] == ' ' || json[colon] == '\t')) colon++;
    if (colon >= json.size() || json[colon] != '"') return "";
    /* 逐字符扫描，正确处理 \\" 转义 */
    size_t end = colon + 1;
    while (end < json.size()) {
        if (json[end] == '\\') {
            end += 2;  /* 跳过转义序列 */
        } else if (json[end] == '"') {
            break;  /* 非转义的闭合引号 */
        } else {
            end++;
        }
    }
    if (end >= json.size()) return "";
    /* 提取并反转义内容 */
    std::string r;
    for (size_t i = colon + 1; i < end; i++) {
        if (json[i] == '\\' && i + 1 < end) {
            char n = json[++i];
            r += (n == '"') ? '"' : (n == '\\') ? '\\' : (n == 'n') ? '\n' : (n == 't') ? '\t' : n;
        } else {
            r += json[i];
        }
    }
    return r;
}

/* 从 API 响应 JSON 中提取 tool_calls，转换为 <tool_call> 标记 */
static std::string ExtractToolCallMarkup(const std::string& json) {
    size_t tc = json.find("\"tool_calls\"");
    if (tc == std::string::npos) return "";

    size_t arr_start = json.find('[', tc);
    if (arr_start == std::string::npos) return "";

    std::string result;
    size_t i = arr_start + 1;
    while (i < json.size()) {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == ',')) i++;
        if (i >= json.size() || json[i] == ']') break;
        if (json[i] != '{') { i++; continue; }

        /* 找到这个 tool_call{} 的结束 */
        size_t depth = 1;
        size_t obj_end = i + 1;
        while (obj_end < json.size() && depth > 0) {
            if (json[obj_end] == '{') depth++;
            else if (json[obj_end] == '}') depth--;
            if (depth > 0) obj_end++;
        }

        std::string obj = json.substr(i, obj_end - i + 1);

        /* 在 tool_call 中找到 function{} 对象 */
        size_t fn = obj.find("\"function\"");
        if (fn != std::string::npos) {
            size_t fn_start = obj.find('{', fn);
            if (fn_start != std::string::npos) {
                size_t fd = 1, fn_end = fn_start + 1;
                while (fn_end < obj.size() && fd > 0) {
                    if (obj[fn_end] == '{') fd++;
                    else if (obj[fn_end] == '}') fd--;
                    if (fd > 0) fn_end++;
                }
                std::string fn_obj = obj.substr(fn_start, fn_end - fn_start + 1);
                std::string name = ExtractJsonStr(fn_obj, "name");
                std::string args = ExtractJsonStr(fn_obj, "arguments");
                if (!name.empty()) {
                    if (!result.empty()) result += "\n";
                    result += "<tool_call>" + name + "|";
                    result += args.empty() ? "{}" : args;
                    result += "</tool_call>";
                }
            }
        }

        i = obj_end + 1;
    }
    return result;
}

/* ========== 实现 ========== */

class LLMClientImpl : public LLMClient {
public:
    LLMClientImpl() = default;
    ~LLMClientImpl() override = default;

    bool Initialize(const std::string& app_id, const std::string& api_key,
                    const std::string& api_secret, bool debug_mode = false) override {
        (void)app_id; (void)api_secret;
        debug_mode_ = debug_mode;
        api_key_ = api_key;
        api_url_ = "https://spark-api-open.xf-yun.com/agent/v1/chat/completions";
        model_ = "spark-x";
        std::cout << kTag << " 初始化完成" << std::endl;
        return true;
    }

    void SetApiUrl(const std::string& url) override { if (!url.empty()) api_url_ = url; }
    void SetSystemPrompt(const std::string& prompt) override { system_prompt_ = prompt; }

    void SetMessages(const std::string& json) override {
        messages_json_ = json;
    }

    void SetTools(const std::vector<ToolDef>& tools) override {
        tools_ = tools;
    }

    bool Chat(const std::string& query) override {
        if (api_key_.empty() || api_key_ == "0000000000000000") {
            if (result_callback_) result_callback_("[模拟] API凭证未配置", true);
            return true;
        }

        /* 构建请求体 */
        std::ostringstream body;
        body << "{\"model\":\"" << model_ << "\",\"messages\":";
        if (!messages_json_.empty()) {
            body << messages_json_;
        } else {
            body << "[";
            if (!system_prompt_.empty())
                body << "{\"role\":\"system\",\"content\":\"" << Escape(system_prompt_) << "\"},";
            body << "{\"role\":\"user\",\"content\":\"" << Escape(query) << "\"}]";
        }

        /* 添加 tools（OpenAI function calling 格式） */
        if (!tools_.empty()) {
            body << ",\"tools\":[";
            for (size_t i = 0; i < tools_.size(); i++) {
                if (i > 0) body << ",";
                body << "{\"type\":\"function\",\"function\":{";
                body << "\"name\":\"" << Escape(tools_[i].name) << "\",";
                body << "\"description\":\"" << Escape(tools_[i].description) << "\",";
                body << "\"parameters\":" << tools_[i].parameters_json;
                body << "}}";
            }
            body << "],\"tool_choice\":\"auto\"";
        }

        body << "}";
        std::string req = body.str();

        if (debug_mode_) {
            /* 截断过长的请求输出（tools 数组可能很大） */
            if (req.size() > 1024) {
                std::cout << kTag << " [调试] 请求 (" << req.size() << " bytes): "
                          << req.substr(0, 256) << "..." << req.substr(req.size() - 128) << std::endl;
            } else {
                std::cout << kTag << " [调试] 请求: " << req << std::endl;
            }
        }

        /* 同步 HTTP POST */
        HttpClient http;
        std::map<std::string, std::string> headers;
        headers["Authorization"] = "Bearer " + api_key_;
        headers["Content-Type"] = "application/json";

        HttpResponse resp;
        if (!http.Post(api_url_, req, headers, resp)) {
            std::cerr << kTag << " HTTP 请求失败" << std::endl;
            if (result_callback_) result_callback_("网络请求失败", true);
            messages_json_.clear();
            return false;
        }

        if (resp.status_code != 200) {
            std::string err = ExtractError(resp.body);
            std::cerr << kTag << " API " << resp.status_code
                      << (err.empty() ? "" : " (" + err + ")") << std::endl;
            if (result_callback_) result_callback_(err.empty() ? "服务错误" : err, true);
            messages_json_.clear();
            return false;
        }

        /* 优先解析 tool_calls（SparkX 返回 tool_calls 时 content 可能为 null） */
        std::string reply = ExtractContent(resp.body);
        std::string tool_calls = ExtractToolCallMarkup(resp.body);

        if (!tool_calls.empty()) {
            /* 工具调用优先，合并文本内容（如有） */
            if (!reply.empty()) reply = reply + "\n" + tool_calls;
            else reply = tool_calls;
        } else if (reply.empty()) {
            reply = "抱歉，我没有理解您的意思";
        }

        std::cout << kTag << " 回复: " << reply.substr(0, 120)
                  << (reply.size() > 120 ? "..." : "") << std::endl;
        if (result_callback_) result_callback_(reply, true);
        messages_json_.clear();
        return true;
    }

    void SetDebugMode(bool enable) override { debug_mode_ = enable; }

private:
    static std::string Escape(const std::string& s) {
        std::string r; r.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"': r+="\\\""; break; case '\\': r+="\\\\"; break;
                case '\n': r+="\\n"; break; case '\r': r+="\\r"; break;
                case '\t': r+="\\t"; break; default: r+=c;
            }
        }
        return r;
    }

    std::string api_key_, api_url_, model_, system_prompt_, messages_json_;
    std::vector<ToolDef> tools_;
    bool debug_mode_ = false;
};

std::unique_ptr<LLMClient> CreateLLMClient() {
    return std::make_unique<LLMClientImpl>();
}
