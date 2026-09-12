/*
 * llm_client.cpp
 * LLM 客户端 — 讯飞星火 Spark-X2，纯同步 HTTP POST
 */

#include "assistant/cloud/llm_client.h"
#include "assistant/cloud/http_client.h"

#include <cstdlib>
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

/* ========== SSE 流式解析辅助 ========== */

/* 提取 JSON 中指定 key 的整数值 */
static int ExtractJsonInt(const std::string& json, const std::string& key, int def = 0) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return def;
    size_t colon = json.find(':', pos + search.size());
    if (colon == std::string::npos) return def;
    size_t p = json.find_first_of("0123456789-", colon + 1);
    if (p == std::string::npos) return def;
    return static_cast<int>(std::strtol(json.c_str() + p, nullptr, 10));
}

/* 流式 tool_calls 的分片累加器：index → {name, arguments} */
struct ToolCallAccum {
    std::string name;
    std::string arguments;
};

/* 从 SSE data 载荷中提取 delta.content（无文本增量时返回空串） */
static std::string ExtractDeltaContent(const std::string& payload) {
    size_t d = payload.find("\"delta\"");
    if (d == std::string::npos) return "";
    return ExtractJsonStr(payload.substr(d), "content");
}

/* 从 SSE data 载荷中提取 tool_calls 分片并累加（name/arguments 均可能分片到达）。
 * 返回是否解析到至少一个 tool_call 对象。 */
static bool AccumulateToolCallDeltas(const std::string& payload,
                                    std::map<int, ToolCallAccum>& out) {
    size_t tc = payload.find("\"tool_calls\"");
    if (tc == std::string::npos) return false;
    size_t arr = payload.find('[', tc);
    if (arr == std::string::npos) return false;

    bool found = false;
    size_t i = arr + 1;
    while (i < payload.size()) {
        while (i < payload.size() &&
               (payload[i] == ' ' || payload[i] == '\t' ||
                payload[i] == '\n' || payload[i] == '\r' || payload[i] == ',')) {
            i++;
        }
        if (i >= payload.size() || payload[i] == ']') break;
        if (payload[i] != '{') { i++; continue; }

        /* 按花括号配对找出这个 tool_call 对象的范围 */
        size_t depth = 1, end = i + 1;
        while (end < payload.size() && depth > 0) {
            if (payload[end] == '{') depth++;
            else if (payload[end] == '}') depth--;
            if (depth > 0) end++;
        }
        std::string obj = payload.substr(i, end - i + 1);

        int index = ExtractJsonInt(obj, "index", 0);
        std::string name, args;
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
                name = ExtractJsonStr(fn_obj, "name");
                args = ExtractJsonStr(fn_obj, "arguments");
            }
        }

        ToolCallAccum& accum = out[index];
        if (!name.empty()) accum.name += name;
        accum.arguments += args;

        found = true;
        i = end + 1;
    }
    return found;
}

/* 把累加后的 tool_calls 转成内部 <tool_call> 标记格式 */
static std::string BuildToolCallMarkup(const std::map<int, ToolCallAccum>& tools) {
    std::string result;
    for (std::map<int, ToolCallAccum>::const_iterator it = tools.begin();
         it != tools.end(); ++it) {
        if (it->second.name.empty()) continue;
        if (!result.empty()) result += "\n";
        result += "<tool_call>" + it->second.name + "|";
        result += it->second.arguments.empty() ? "{}" : it->second.arguments;
        result += "</tool_call>";
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

    void SetDebugMode(bool enable) override { debug_mode_ = enable; }
    void SetStreaming(bool enable) override { streaming_ = enable; }

    bool Chat(const std::string& query) override {
        if (api_key_.empty() || api_key_ == "0000000000000000") {
            if (result_callback_) result_callback_("[模拟] API凭证未配置", true);
            return true;
        }

        /* 流式优先；服务端不支持 SSE 时自动回退非流式 */
        if (streaming_ && ChatStream(query)) return true;
        return ChatSync(query);
    }

private:
    /* 组装请求体（stream 决定是否附 "stream":true） */
    std::string BuildBody(const std::string& query, bool stream) const {
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

        if (stream) body << ",\"stream\":true";
        body << "}";
        return body.str();
    }

    /* 记录调试请求（tools 数组可能很长，超长时截断首尾） */
    void LogRequest(const std::string& req) const {
        if (!debug_mode_) return;
        if (req.size() > 1024) {
            std::cout << kTag << " [调试] 请求 (" << req.size() << " bytes): "
                      << req.substr(0, 256) << "..." << req.substr(req.size() - 128) << std::endl;
        } else {
            std::cout << kTag << " [调试] 请求: " << req << std::endl;
        }
    }

    /* 统一出口：合并正文与工具调用标记后回调最终结果 */
    void DeliverReply(const std::string& content, const std::string& tool_markup) {
        std::string reply = content;
        if (!tool_markup.empty()) {
            reply = reply.empty() ? tool_markup : reply + "\n" + tool_markup;
        } else if (reply.empty()) {
            reply = "抱歉，我没有理解您的意思";
        }

        std::cout << kTag << " 回复: " << reply.substr(0, 120)
                  << (reply.size() > 120 ? "..." : "") << std::endl;
        if (result_callback_) result_callback_(reply, true);
    }

    /* 处理一行 SSE；返回 true 表示收到 [DONE]（应停止接收） */
    bool HandleSseLine(const std::string& raw_line, std::string& content,
                       std::map<int, ToolCallAccum>& tool_calls, bool& got_data,
                       size_t& forwarded, bool& blocked) {
        std::string line = raw_line;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ':') return false; /* 注释/心跳 */

        static const std::string kPrefix = "data:";
        if (line.compare(0, kPrefix.size(), kPrefix) != 0) return false;

        std::string payload = line.substr(kPrefix.size());
        size_t b = payload.find_first_not_of(" \t");
        if (b == std::string::npos) return false;
        payload = payload.substr(b);
        if (payload == "[DONE]") return true;

        std::string delta = ExtractDeltaContent(payload);
        if (!delta.empty()) {
            content += delta;
            ForwardContent(content, forwarded, blocked);
        }
        bool got_tool = AccumulateToolCallDeltas(payload, tool_calls);

        /* 只有「解析出实际内容」才算流式有效：首块的 role 心跳不应挡住非流式回退 */
        if (!delta.empty() || got_tool) got_data = true;
        return false;
    }

    /* 只把「确定不是工具调用标记」的正文增量回调给上层。
     * 部分模型会把 <tool_call>...</tool_call> 当普通文本流式输出，
     * 若原样转发会被切句并送去 TTS 合成；标记还可能跨 chunk 到达，
     * 故把末尾「可能是标记前缀」的若干字节暂时压住不转发。
     * forwarded 为已转发字节数，blocked 表示已确认进入标记区。 */
    void ForwardContent(const std::string& content, size_t& forwarded, bool& blocked) {
        static const std::string kMarker = "<tool_call";
        if (blocked) return;

        size_t marker = content.find(kMarker, forwarded);
        if (marker != std::string::npos) {
            if (marker > forwarded && result_callback_) {
                result_callback_(content.substr(forwarded, marker - forwarded), false);
            }
            forwarded = marker;
            blocked = true;
            return;
        }

        /* 末尾至多 kMarker.size()-1 字节可能是标记前缀 → 暂不转发 */
        size_t max_hold = content.size() - forwarded;
        if (max_hold > kMarker.size() - 1) max_hold = kMarker.size() - 1;

        size_t hold = 0;
        for (size_t k = max_hold; k >= 1; --k) {
            if (content.compare(content.size() - k, k, kMarker, 0, k) == 0) {
                hold = k;
                break;
            }
        }

        size_t end = content.size() - hold;
        if (end > forwarded && result_callback_) {
            result_callback_(content.substr(forwarded, end - forwarded), false);
        }
        forwarded = end;
    }

    /* 流结束时把压住的尾部正文补发出去（未进入标记区时） */
    void FlushForwardedContent(const std::string& content, size_t& forwarded, bool blocked) {
        if (blocked || forwarded >= content.size()) return;
        if (result_callback_) {
            result_callback_(content.substr(forwarded), false);
        }
        forwarded = content.size();
    }

    /* SSE 流式请求；返回 false 表示应回退到非流式（未产生任何增量输出） */
    bool ChatStream(const std::string& query) {
        std::string req = BuildBody(query, true);
        LogRequest(req);

        HttpClient http;
        std::map<std::string, std::string> headers;
        headers["Authorization"] = "Bearer " + api_key_;
        headers["Content-Type"] = "application/json";
        headers["Accept"] = "text/event-stream";

        HttpResponse resp;
        std::string pending_line;                 /* 跨 chunk 的半行缓冲 */
        std::string content;                      /* 累积正文 */
        std::map<int, ToolCallAccum> tool_calls;  /* 分片重组的工具调用 */
        bool got_data = false;
        size_t forwarded = 0;                     /* 已回调给上层的正文字节数 */
        bool blocked = false;                     /* 是否已进入 <tool_call> 标记区 */

        bool ok = http.PostStream(api_url_, req, headers,
            [&](const std::string& chunk) -> bool {
                pending_line += chunk;
                size_t pos;
                while ((pos = pending_line.find('\n')) != std::string::npos) {
                    std::string line = pending_line.substr(0, pos);
                    pending_line.erase(0, pos + 1);
                    if (HandleSseLine(line, content, tool_calls, got_data,
                                      forwarded, blocked)) {
                        return false; /* 收到 [DONE]，主动结束接收 */
                    }
                }
                return true;
            }, resp);

        if (!ok) {
            std::cerr << kTag << " 流式 HTTP 请求失败，回退非流式" << std::endl;
            return false;
        }
        if (resp.status_code != 200) {
            /* 未产生任何增量，安全回退非流式以复用其错误处理 */
            std::cerr << kTag << " 流式请求被拒 (HTTP " << resp.status_code
                      << ")，回退非流式" << std::endl;
            return false;
        }

        if (!pending_line.empty()) {
            HandleSseLine(pending_line, content, tool_calls, got_data, forwarded, blocked);
        }

        if (!got_data) {
            /* 服务端忽略了 stream 参数，返回的是普通 JSON → 回退非流式解析 */
            std::cout << kTag << " 响应非 SSE 格式，回退非流式" << std::endl;
            return false;
        }

        /* 补发被前缀判定压住的尾部正文（仅未进入标记区时） */
        FlushForwardedContent(content, forwarded, blocked);

        DeliverReply(content, BuildToolCallMarkup(tool_calls));
        messages_json_.clear();
        return true;
    }

    /* 非流式请求（回退路径） */
    bool ChatSync(const std::string& query) {
        std::string req = BuildBody(query, false);
        LogRequest(req);

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
        DeliverReply(ExtractContent(resp.body), ExtractToolCallMarkup(resp.body));
        messages_json_.clear();
        return true;
    }

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
    bool streaming_ = false;
};

std::unique_ptr<LLMClient> CreateLLMClient() {
    return std::make_unique<LLMClientImpl>();
}
