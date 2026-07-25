/*
 * assistant.cpp
 * AI 语音助手主控制器实现 v2.3
 *
 * 三层架构: action > skill > MCP
 *   action: keyword + LLM 触发，不回注（硬件操作）
 *   skill:  仅 LLM 触发，必回注（SKILL.md → 完整执行）
 *   MCP:    仅 LLM 触发，必回注（JSON-RPC 2.0 标准）
 */

#include "assistant/core/assistant.h"
#include "assistant/cloud/http_client.h"
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <thread>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* kTag = "[Assistant]";

/* 判断文件是否存在 */
static bool FileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

/* ========== 工厂函数声明 ========== */
std::unique_ptr<ASRClient> CreateASRClient();
std::unique_ptr<LLMClient> CreateLLMClient();
std::unique_ptr<TTSClient> CreateTTSClient();
std::unique_ptr<KWSEngine> CreateKWSEngine(const std::string& model_path);

Assistant::Assistant()
    : audio_buffer_(16000 * 2)
{
    state_machine_ = std::make_unique<StateMachine>();
    config_        = std::make_unique<Config>();
    audio_capture_ = std::make_unique<AudioCapture>();
    audio_playback_ = std::make_unique<AudioPlayback>();
    vad_           = std::make_unique<VAD>();
    ir_            = std::make_unique<IRController>();
    sensitive_     = std::make_unique<SensitiveFilter>();
    asr_           = CreateASRClient();
    llm_           = CreateLLMClient();
    tts_           = CreateTTSClient();
    agent_core_    = std::make_unique<agent::AgentCore>();
    action_mgr_    = std::make_unique<agent::ActionManager>();
    skill_mgr_     = std::make_unique<agent::SkillManager>();
    mcp_tools_     = std::make_unique<agent::MCPTools>();
}

Assistant::~Assistant() {
    Stop();
    /* 确保管线线程已退出 */
    if (tts_pipeline_thread_ && tts_pipeline_thread_->joinable()) {
        tts_pipeline_thread_->join();
    }
}

/* ========== 初始化 ========== */

bool Assistant::Initialize(const std::string& config_path) {
    std::cout << kTag << " 初始化中..." << std::endl;

    if (!config_->Load(config_path)) {
        std::cerr << kTag << " 加载配置失败: " << config_path << std::endl;
        return false;
    }

    /* 音频模块 */
    std::string cap_dev = config_->Get("audio", "capture_device", "default");
    std::string play_dev = config_->Get("audio", "playback_device", "default");
    unsigned int sample_rate = config_->GetInt("audio", "sample_rate", 16000);
    unsigned int channels = config_->GetInt("audio", "channels", 1);

    if (!audio_capture_->Open(cap_dev, sample_rate, channels)) {
        std::cerr << kTag << " 打开录音设备失败" << std::endl;
        return false;
    }
    if (!audio_playback_->Open(play_dev, 24000, 2)) {
        std::cerr << kTag << " 打开播放设备失败" << std::endl;
        return false;
    }

    /* VAD */
    unsigned int vad_timeout = config_->GetInt("audio", "vad_silence_timeout_ms", 1500);
    unsigned int min_speech = config_->GetInt("audio", "vad_min_speech_ms", 200);
    conversation_timeout_ms_ = config_->GetInt("audio", "vad_conversation_timeout_ms", 10000);
    vad_->Configure(sample_rate, vad_timeout, min_speech);

    /* KWS */
    std::string model_path = config_->Get("kws", "model_path", "models/kws_model.tflite");
    float kws_threshold = config_->GetFloat("kws", "threshold", 20000.0f);
    bool debug_mode = config_->GetBool("system", "debug_mode", true);
    kws_ = CreateKWSEngine(model_path);
    if (!kws_->Initialize(model_path, kws_threshold)) {
        std::cerr << kTag << " KWS 初始化失败（可忽略）" << std::endl;
    }
    kws_->SetDebugMode(debug_mode);
    kws_->OnWakeWordDetected([this]() { OnWakeWordDetected(); });

    /* IR */
    ir_->Initialize(true);

    /* Agent Prompt */
    std::string prompt_path = config_->Get("agent", "prompt_path", "config/agent_prompt.md");
    agent_core_->LoadPrompt(prompt_path);

    /* Actions（最高优先级） */
    std::string actions_path = config_->Get("agent", "actions_path", "config/actions.json");
    action_mgr_->LoadFromFile(actions_path);
    RegisterActions();

    /* Skills（中间层，SKILL.md） */
    std::string skills_dir = config_->Get("agent", "skills_dir", "skills");
    skill_mgr_->LoadFromDirectory(skills_dir);
    RegisterSkillHandlers();

    /* MCP（JSON-RPC 2.0，config/mcp_tools.json） */
    mcp_tools_->SetMaxRounds(config_->GetInt("agent", "max_tool_rounds", 3));
    std::string mcp_path = config_->Get("agent", "mcp_tools_path", "config/mcp_tools.json");
    if (FileExists(mcp_path)) mcp_tools_->LoadFromFile(mcp_path);
    RegisterMCPBuiltins();

    std::cout << kTag << " [DEBUG] 系统初始化: "
              << action_mgr_->Count() << " actions, "
              << skill_mgr_->Count() << " skills, "
              << mcp_tools_->ToolCount() << " MCP 工具" << std::endl;

    /* 唤醒提示音 */
    wakeup_sound_path_ = config_->Get("agent", "wakeup_sound", "config/wakeup.wav");
    {
        std::ifstream test(wakeup_sound_path_);
        std::cout << kTag << " [DEBUG] 唤醒提示音: " << wakeup_sound_path_
                  << (test.good() ? " ✓" : " ✗") << std::endl;
    }

    /* 云端服务 */
    std::string app_id = config_->Get("cloud", "app_id", "00000000");
    std::string api_key = config_->Get("cloud", "api_key", "");
    std::string api_secret = config_->Get("cloud", "api_secret", "");

    asr_->Initialize(app_id, api_key, api_secret, debug_mode);
    {
        std::string res_id = config_->Get("asr", "res_id", "");
        if (!res_id.empty()) asr_->SetResId(res_id);
    }

    {
        std::string llm_key = config_->Get("llm", "api_key", api_key);
        llm_->Initialize(app_id, llm_key, api_secret, debug_mode);
        std::string llm_url = config_->Get("llm", "api_url", "");
        if (!llm_url.empty()) llm_->SetApiUrl(llm_url);
    }

    {
        std::string tts_voice = config_->Get("tts", "voice", "xiaoyan");
        std::string tts_auth = config_->Get("tts", "auth", "");
        tts_->Initialize(app_id, api_key, api_secret, debug_mode, tts_auth);
        if (!tts_voice.empty()) tts_->SetVoice(tts_voice);
    }

    std::cout << kTag << " [DEBUG] 云端服务初始化完成" << std::endl;

    /* 注册回调 */
    state_machine_->OnStateChanged(
        [this](AssistantState old_st, AssistantState new_st) {
            OnStateChanged(old_st, new_st);
        });
    audio_capture_->OnData(
        [this](const std::vector<int16_t>& data) {
            OnAudioData(data);
        });
    audio_playback_->OnPlaybackDone(
        [this]() { OnPlaybackDone(); });

    std::cout << kTag << " 初始化完成!" << std::endl;
    return true;
}

void Assistant::Start() {
    if (running_) return;
    running_ = true;
    audio_capture_->Start();
    std::cout << kTag << " 助手已启动，等待唤醒词 \"小九小九\"..." << std::endl;
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Assistant::Stop() {
    running_ = false;
    audio_capture_->Stop();
    audio_playback_->Stop();
}

void Assistant::TriggerWakeup() {
    if (state_machine_->CurrentState() == AssistantState::SLEEP) {
        state_machine_->TransitionTo(AssistantState::WAKEUP);
    }
}

void Assistant::ForceSleep() {
    state_machine_->TransitionTo(AssistantState::SLEEP);
}

/* ========== Action / Skill / MCP 注册 ========== */

void Assistant::RegisterActions() {
    action_mgr_->RegisterHandler("action.", [this](const std::string& action) -> std::string {
        if (action == "action.vol_up")   return "好的，音量已调大";
        if (action == "action.vol_down") return "好的，音量已调小";

        if (action == "action.light_on")  { ir_->Send("light_on");  return "好的，已开灯"; }
        if (action == "action.light_off") { ir_->Send("light_off"); return "好的，已关灯"; }

        if (action == "action.ac_on")       { ir_->Send("ac_on");       return "好的，已打开空调"; }
        if (action == "action.ac_off")      { ir_->Send("ac_off");      return "好的，已关闭空调"; }
        if (action == "action.ac_toggle")   { ir_->Send("ac_toggle");   return "好的，已切换空调"; }
        if (action == "action.ac_temp_up")  { ir_->Send("ac_temp_up");  return "好的，温度已调高"; }
        if (action == "action.ac_temp_down"){ ir_->Send("ac_temp_down");return "好的，温度已调低"; }

        if (action == "action.sleep") return "";

        return "";
    });
}

void Assistant::RegisterMCPBuiltins() {
    /* ── mcp.get_weather — wttr.in JSON 格式（含预报）── */
    mcp_tools_->RegisterBuiltin("get_weather", [](const agent::ToolCall& call) -> agent::ToolResult {
        agent::ToolResult r;
        auto it = call.arguments.find("city");
        std::string city = (it != call.arguments.end()) ? it->second : "杭州";

        HttpClient http;
        std::map<std::string, std::string> headers;
        HttpResponse resp;
        /* 使用 JSON 格式，包含当天气温和未来预报 */
        std::string url = "http://wttr.in/" + city + "?format=j1";
        std::cout << kTag << " [MCP] weather: " << url << std::endl;

        if (http.Get(url, headers, resp) && resp.status_code == 200 && !resp.body.empty()) {
            std::string body = resp.body;
            std::ostringstream out;

            /* 解析当前天气 */
            std::string tmp = agent::MCPTools::ExtractJsonString(body, "temp_C");
            std::string humidity = agent::MCPTools::ExtractJsonString(body, "humidity");
            std::string wind = agent::MCPTools::ExtractJsonString(body, "windspeedKmph");
            std::string desc = agent::MCPTools::ExtractJsonString(body, "weatherDesc");
            /* 提取 weatherDesc 数组中的描述 */
            if (!desc.empty() && desc[0] == '[') {
                size_t v = desc.find("\"value\"");
                if (v != std::string::npos) {
                    size_t q1 = desc.find('"', v + 8);
                    size_t q2 = (q1 != std::string::npos) ? desc.find('"', q1 + 1) : std::string::npos;
                    if (q1 != std::string::npos && q2 != std::string::npos)
                        desc = desc.substr(q1 + 1, q2 - q1 - 1);
                }
            }

            out << city << "当前天气：" << desc << "，温度" << tmp << "°C";
            if (!humidity.empty()) out << "，湿度" << humidity << "%";
            if (!wind.empty()) out << "，风力" << wind << "km/h";

            /* 解析未来预报（weather 数组中的对象） */
            auto forecast_days = agent::MCPTools::ExtractJsonObjects(body, "weather");
            for (size_t i = 0; i < forecast_days.size() && i < 3; i++) {
                std::string date = agent::MCPTools::ExtractJsonString(forecast_days[i], "date");
                std::string tmax = agent::MCPTools::ExtractJsonString(forecast_days[i], "tempMaxC");
                std::string tmin = agent::MCPTools::ExtractJsonString(forecast_days[i], "tempMinC");
                if (!date.empty()) {
                    std::string mmdd = (date.size() >= 10) ? date.substr(5, 5) : date;
                    out << "；" << mmdd << " " << tmax << "°C/" << tmin << "°C";
                }
            }

            r.content = out.str();
            r.success = true;
        } else {
            r.success = false;
            r.error = "天气服务暂不可用";
        }
        return r;
    });

    /* ── mcp.get_time — 系统 `date` 命令（BusyBox 用 POSIX TZ）── */
    mcp_tools_->RegisterBuiltin("get_time", [](const agent::ToolCall& call) -> agent::ToolResult {
        agent::ToolResult r;
        /* IANA → POSIX 时区映射（BusyBox 不认识 IANA 名） */
        static const char* kTzMap[][2] = {
            {"Asia/Shanghai", "CST-8"},
            {"Asia/Beijing",  "CST-8"},
            {"America/New_York", "EST5EDT"},
            {"America/Chicago",  "CST6CDT"},
            {"America/Denver",   "MST7MDT"},
            {"America/Los_Angeles", "PST8PDT"},
            {"Europe/London",    "GMT0BST"},
            {"Europe/Berlin",    "CET-1CEST"},
            {"Europe/Paris",     "CET-1CEST"},
            {"Europe/Moscow",    "MSK-3"},
            {"Asia/Tokyo",       "JST-9"},
            {"Asia/Seoul",       "KST-9"},
            {"Asia/Singapore",   "SGT-8"},
            {"Asia/Hong_Kong",   "HKT-8"},
            {"Australia/Sydney", "AEST-10AEDT"},
            {"Pacific/Auckland", "NZST-12NZDT"},
            {"UTC",              "UTC"},
            {"Etc/UTC",          "UTC"},
            {"Etc/GMT",          "GMT0"},
        };
        auto it = call.arguments.find("timezone");
        std::string tz_arg = (it != call.arguments.end()) ? it->second : "Asia/Shanghai";
        std::string posix_tz = "CST-8";  /* 默认北京时间 */
        std::string display_name = "Asia/Shanghai";
        for (size_t i = 0; i < sizeof(kTzMap)/sizeof(kTzMap[0]); i++) {
            if (tz_arg == kTzMap[i][0]) {
                posix_tz = kTzMap[i][1];
                display_name = tz_arg;
                break;
            }
        }

        std::string cmd = "TZ=" + posix_tz + " date \"+%Y年%m月%d日 %H:%M:%S\" 2>/dev/null";
        FILE* fp = popen(cmd.c_str(), "r");
        if (!fp) { r.error = "时间命令执行失败"; return r; }
        char buf[128] = {};
        if (!fgets(buf, sizeof(buf), fp)) { pclose(fp); r.error = "时间命令无输出"; return r; }
        pclose(fp);

        std::string result(buf);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
        if (result.empty()) { r.error = "时间为空"; return r; }

        r.content = result + " (" + display_name + ")";
        r.success = true;
        std::cout << kTag << " [MCP] time: " << r.content << std::endl;
        return r;
    });
}

void Assistant::RegisterSkillHandlers() {
    /* Skill 处理器：LLM 调用 skill.* 后，返回数据供 LLM 生成回复 */
    skill_mgr_->RegisterHandler("skill.", [this](const std::string& action) -> std::string {
        /* 今日简报 — 内部调用 MCP 获取天气+时间 */
        if (action == "skill.daily_briefing") {
            std::ostringstream briefing;
            /* 获取天气 */
            agent::ToolCall wtc;
            wtc.name = "mcp.get_weather";
            wtc.arguments["city"] = "杭州";
            agent::ToolResult wr = mcp_tools_->ExecuteTool(wtc);
            /* 获取时间 */
            agent::ToolCall ttc;
            ttc.name = "mcp.get_time";
            agent::ToolResult tr = mcp_tools_->ExecuteTool(ttc);
            briefing << "【今日简报数据】\n";
            briefing << "时间：" << (tr.success ? tr.content : "未知") << "\n";
            briefing << "天气：" << (wr.success ? wr.content : "未知") << "\n";
            briefing << "请基于以上数据，根据 SKILL.md 中的回复模板，生成简洁自然的今日简报回复。";
            return briefing.str();
        }
        return "";
    });
}

/* ========== 工具执行（action > skill > MCP） ========== */

Assistant::ToolExecResult Assistant::ExecuteSingleTool(const agent::ToolCall& call) {
    ToolExecResult result;

    /* action：直接执行，不回注 */
    if (call.name.find("action.") == 0) {
        /* action.set_preference — 添加/修改偏好 */
        if (call.name == "action.set_preference") {
            auto kit = call.arguments.find("key");
            auto vit = call.arguments.find("value");
            if (kit != call.arguments.end() && vit != call.arguments.end()) {
                agent_core_->AddMutableItem(kit->second + "：" + vit->second);
                result.success = true;
                result.content = "已记住：" + kit->second + "=" + vit->second;
            } else {
                result.success = false;
                result.content = "请指定偏好名称和值";
            }
            result.reenter_llm = false;
            return result;
        }
        /* action.delete_preference — 删除偏好 */
        if (call.name == "action.delete_preference") {
            auto kit = call.arguments.find("key");
            if (kit != call.arguments.end()) {
                bool removed = agent_core_->RemoveMutableItem(kit->second);
                result.success = true;
                result.content = removed ? "已删除偏好：" + kit->second : "未找到相关偏好";
            } else {
                result.success = false;
                result.content = "请指定要删除的偏好名称";
            }
            result.reenter_llm = false;
            return result;
        }
        agent::ActionResult ar = action_mgr_->Execute(call.name);
        result.success = ar.handled;
        result.content = ar.response;
        result.reenter_llm = false;
        return result;
    }

    /* skill：必回注（至少返回执行结果） */
    if (call.name.find("skill.") == 0) {
        agent::SkillResult sr = skill_mgr_->Execute(call.name);
        result.success = sr.handled;
        /* 返回执行结果 + SKILL.md 正文供 LLM 使用 */
        result.content = sr.response;
        if (!sr.full_body.empty()) {
            result.content += "\n\n[SKILL.md 指令]\n" + sr.full_body;
        }
        result.reenter_llm = true;  /* skill 一定回注 */
        return result;
    }

    /* MCP：JSON-RPC 2.0 协议，必回注 */
    if (call.name.find("mcp.") == 0) {
        agent::ToolResult r = mcp_tools_->ExecuteTool(call);
        result.success = r.success;
        result.content = r.success ? r.content : r.error;
        result.reenter_llm = true;  /* MCP 一定回注 */
        return result;
    }

    result.success = false;
    result.content = "未知调用: " + call.name;
    result.reenter_llm = false;
    return result;
}

std::string Assistant::ExecuteToolCalls(const std::vector<agent::ToolCall>& calls) {
    std::ostringstream results;
    for (size_t i = 0; i < calls.size(); i++) {
        const agent::ToolCall& call = calls[i];
        std::cout << kTag << " [ToolCall] " << call.name << std::endl;

        ToolExecResult r = ExecuteSingleTool(call);
        if (r.success) {
            results << "[结果: " << call.name << "] " << r.content << "\n";
        } else {
            results << "[错误: " << call.name << "] " << r.content << "\n";
        }
    }
    return results.str();
}

std::string Assistant::StripToolCalls(const std::string& text) {
    std::string result = text;
    const std::string kOpenTag = "<tool_call>";
    const std::string kCloseTag = "</tool_call>";

    size_t pos = 0;
    while (true) {
        size_t start = result.find(kOpenTag, pos);
        if (start == std::string::npos) break;
        size_t end = result.find(kCloseTag, start);
        if (end == std::string::npos) break;
        result.erase(start, end - start + kCloseTag.size());
        pos = start;
    }

    /* 清理多余换行 */
    while (result.find("\n\n\n") != std::string::npos) {
        size_t p = result.find("\n\n\n");
        result.erase(p, 2);
    }

    size_t first = result.find_first_not_of(" \t\r\n");
    size_t last = result.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return result.substr(first, last - first + 1);
}

/* ========== TTS ========== */

void Assistant::SafeTTS(const std::string& text) {
    if (text.empty()) {
        state_machine_->TransitionTo(AssistantState::LISTENING);
        return;
    }
    tts_->OnAudio([this](const std::vector<int16_t>& pcm) {
        OnTTSAudio(pcm);
    });
    if (!tts_->Synthesize(text)) {
        std::cerr << kTag << " TTS 合成失败，回到聆听态" << std::endl;
        state_machine_->TransitionTo(AssistantState::LISTENING);
    }
}

/* 检查 UTF-8 句子结束标点 */
static bool IsSentenceEnd(const std::string& text, size_t pos) {
    unsigned char c = static_cast<unsigned char>(text[pos]);

    /* ASCII 句子结束符 */
    if (c == '\n' || c == '!' || c == '?' || c == ';') return true;

    /* UTF-8 中文标点（3字节序列） */
    if (pos + 2 < text.size()) {
        unsigned char c1 = static_cast<unsigned char>(text[pos]);
        unsigned char c2 = static_cast<unsigned char>(text[pos + 1]);
        unsigned char c3 = static_cast<unsigned char>(text[pos + 2]);

        if (c1 == 0xE3 && c2 == 0x80 && c3 == 0x82) return true;  /* 。*/
        if (c1 == 0xEF && c2 == 0xBC && c3 == 0x81) return true;  /* ！*/
        if (c1 == 0xEF && c2 == 0xBC && c3 == 0x9F) return true;  /* ？*/
        if (c1 == 0xEF && c2 == 0xBC && c3 == 0x9B) return true;  /* ；*/
    }
    return false;
}

std::vector<std::string> Assistant::SplitSentences(const std::string& text) {
    std::vector<std::string> sentences;
    std::string current;

    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = static_cast<unsigned char>(text[i]);

        /* UTF-8 字符字节长度 */
        size_t char_len = 1;
        if ((c & 0x80) == 0) {
            char_len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            char_len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            char_len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            char_len = 4;
        }

        if (IsSentenceEnd(text, i)) {
            if (!current.empty()) {
                size_t start = current.find_first_not_of(" \t\r\n");
                if (start != std::string::npos) {
                    sentences.push_back(current.substr(start));
                }
            }
            current.clear();
            i += (c < 0x80) ? 1 : 3;
            continue;
        }

        for (size_t j = 0; j < char_len && i + j < text.size(); j++) {
            current += text[i + j];
        }
        i += char_len;
    }

    /* 剩余内容 */
    if (!current.empty()) {
        size_t start = current.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) {
            sentences.push_back(current.substr(start));
        }
    }

    /* 合并短句 */
    std::vector<std::string> merged;
    for (size_t i = 0; i < sentences.size(); i++) {
        std::string s = sentences[i];
        size_t char_count = 0;
        for (size_t j = 0; j < s.size(); j++) {
            unsigned char byte = static_cast<unsigned char>(s[j]);
            if (byte >= 0x80) {
                char_count++;
                while (j + 1 < s.size() && (static_cast<unsigned char>(s[j+1]) & 0xC0) == 0x80) j++;
            } else if (byte > 32 && byte != 127) {
                char_count++;
            }
        }
        if (char_count <= 2 && !merged.empty()) {
            merged.back() += s;
        } else {
            merged.push_back(s);
        }
    }

    return merged;
}

/* ── TTS 流式管线 ── */

void Assistant::StartStreamingPipeline(const std::string& text) {
    auto sentences = SplitSentences(text);
    if (sentences.empty()) {
        std::cout << kTag << " 流式TTS：无有效句子" << std::endl;
        state_machine_->TransitionTo(AssistantState::LISTENING);
        return;
    }

    std::cout << kTag << " 流式TTS管线：共 " << sentences.size() << " 句" << std::endl;

    /* 进入播放态，暂停录音 */
    audio_capture_->Pause();
    state_machine_->TransitionTo(AssistantState::SPEAKING);

    /* 重置管线状态 */
    {
        std::lock_guard<std::mutex> lock(tts_queue_mutex_);
        tts_pcm_queue_.clear();
        tts_queue_read_idx_ = 0;
        tts_all_synthesized_ = false;
    }

    /* 启动后台播放线程（消费者） */
    tts_pipeline_thread_ = std::make_unique<std::thread>(&Assistant::TTSPlaybackThread, this);

    /* 当前线程逐句合成（生产者） */
    for (size_t i = 0; i < sentences.size(); i++) {
        if (sleep_requested_) break;

        std::cout << kTag << " TTS [" << (i + 1) << "/" << sentences.size()
                  << "]: \"" << sentences[i] << "\"" << std::endl;

        bool ok = false;
        std::vector<int16_t> pcm;

        tts_->OnAudio([&](const std::vector<int16_t>& p) {
            pcm = p;
            ok = true;
        });

        if (tts_->Synthesize(sentences[i]) && ok && !pcm.empty()) {
            std::lock_guard<std::mutex> lock(tts_queue_mutex_);
            tts_pcm_queue_.push_back(std::move(pcm));
            tts_queue_cv_.notify_one();  /* 通知播放线程有新数据 */
        } else {
            std::cerr << kTag << " TTS 合成失败: \"" << sentences[i] << "\"" << std::endl;
        }
    }

    /* 标记合成完成 */
    {
        std::lock_guard<std::mutex> lock(tts_queue_mutex_);
        tts_all_synthesized_ = true;
        tts_queue_cv_.notify_one();
    }

    /* 等待播放线程完成 */
    if (tts_pipeline_thread_ && tts_pipeline_thread_->joinable()) {
        tts_pipeline_thread_->join();
    }
    tts_pipeline_thread_.reset();

    /* 全部播完 */
    std::cout << kTag << " 流式TTS完成" << std::endl;
    if (sleep_requested_) {
        sleep_requested_ = false;
        ForceSleep();
    } else {
        state_machine_->TransitionTo(AssistantState::LISTENING);
    }
}

void Assistant::TTSPlaybackThread() {
    std::cout << kTag << " [管线] 播放线程启动" << std::endl;

    while (true) {
        std::vector<int16_t> pcm;
        bool has_data = false;

        {
            std::unique_lock<std::mutex> lock(tts_queue_mutex_);
            if (tts_queue_read_idx_ < tts_pcm_queue_.size()) {
                pcm = std::move(tts_pcm_queue_[tts_queue_read_idx_++]);
                has_data = true;
            } else if (tts_all_synthesized_) {
                /* 队列已空且合成已完成 → 退出 */
                break;
            } else {
                /* 等待更多数据（最多 200ms，避免死锁） */
                tts_queue_cv_.wait_for(lock, std::chrono::milliseconds(200));
                continue;
            }
        }

        if (has_data && !pcm.empty()) {
            /* 同步播放（blocking aplay） */
            audio_playback_->Play(pcm);
        }
    }

    std::cout << kTag << " [管线] 播放线程退出" << std::endl;
}

/* ========== 状态机回调 ========== */

void Assistant::OnStateChanged(AssistantState old_state, AssistantState new_state) {
    std::cout << kTag << " 状态: "
              << StateName(old_state) << " → " << StateName(new_state) << std::endl;

    switch (new_state) {
    case AssistantState::SLEEP:
        kws_->Reset();
        vad_->Reset();
        audio_buffer_.clear();
        agent_core_->ClearHistory();
        sleep_requested_ = false;
        /* 确保录音已启动：SafeTTS 期间录音被暂停，恢复后才能持续监听唤醒词 */
        if (!audio_capture_->IsRunning()) {
            std::cout << kTag << " [DEBUG] SLEEP: 恢复录音以监听唤醒词" << std::endl;
            audio_capture_->Start();
        }
        break;

    case AssistantState::WAKEUP:
        audio_capture_->Pause();
        PlayWakeupSound();
        state_machine_->TransitionTo(AssistantState::LISTENING);
        break;

    case AssistantState::LISTENING:
        vad_->Reset();
        listening_silence_samples_ = 0;
        audio_buffer_.clear();
        audio_buffer_.reserve(16000 * 10);
        if (!audio_capture_->IsRunning()) audio_capture_->Start();
        break;

    case AssistantState::PROCESSING:
    case AssistantState::SPEAKING:
        break;
    }
}

void Assistant::OnWakeWordDetected() {
    std::cout << kTag << " 检测到唤醒词!" << std::endl;
    TriggerWakeup();
}

void Assistant::PlayWakeupSound() {
    if (!FileExists(wakeup_sound_path_)) {
        std::cout << kTag << " 唤醒提示音文件不存在" << std::endl;
        return;
    }
    std::string cmd = "aplay -q " + wakeup_sound_path_ + " 2>&1";
    system(cmd.c_str());
}

void Assistant::OnAudioData(const std::vector<int16_t>& data) {
    AssistantState state = state_machine_->CurrentState();

    if (state == AssistantState::SLEEP) {
        kws_->ProcessAudio(data);
    } else if (state == AssistantState::LISTENING) {
        audio_buffer_.insert(audio_buffer_.end(), data.begin(), data.end());
        bool has_speech = vad_->Process(data);
        if (has_speech) {
            listening_silence_samples_ = 0;
        } else {
            listening_silence_samples_ += data.size();
        }
        if (vad_->IsSpeechEnd()) {
            OnSpeechEnd();
            return;
        }
        unsigned int timeout_samples = 16000 * conversation_timeout_ms_ / 1000;
        if (listening_silence_samples_ > timeout_samples) {
            std::cout << kTag << " 静默超时，进入休眠" << std::endl;
            ForceSleep();
        }
    }
}

void Assistant::OnSpeechEnd() {
    if (state_machine_->CurrentState() != AssistantState::LISTENING) return;
    std::cout << kTag << " 语音结束，" << audio_buffer_.size() << " samples" << std::endl;
    state_machine_->TransitionTo(AssistantState::PROCESSING);
    ASRResult asr_result = asr_->Recognize(audio_buffer_);
    ProcessResult(asr_result.text);
}

void Assistant::OnTTSAudio(const std::vector<int16_t>& pcm) {
    state_machine_->TransitionTo(AssistantState::SPEAKING);
    audio_capture_->Pause();
    audio_playback_->PlayAsync(pcm);
}

void Assistant::OnPlaybackDone() {
    /* SafeTTS 的异步播放完成后回到聆听态 */
    if (sleep_requested_) {
        sleep_requested_ = false;
        ForceSleep();
    } else {
        state_machine_->TransitionTo(AssistantState::LISTENING);
    }
}

/* ========== 核心路由 ========== */

void Assistant::ProcessResult(const std::string& asr_text) {
    if (asr_text.empty()) {
        state_machine_->TransitionTo(AssistantState::LISTENING);
        return;
    }

    /* 去除首尾空白和标点，提高关键词匹配容错 */
    std::string text = asr_text;
    auto IsPunctOrSpace = [](const std::string& s, size_t pos) -> bool {
        unsigned char c = static_cast<unsigned char>(s[pos]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == '.')
            return true;
        /* UTF-8 全角标点：， = E3 BC 8C, 。 = E3 80 82 */
        if (pos + 2 < s.size()) {
            unsigned char c1 = static_cast<unsigned char>(s[pos]);
            unsigned char c2 = static_cast<unsigned char>(s[pos + 1]);
            unsigned char c3 = static_cast<unsigned char>(s[pos + 2]);
            if (c1 == 0xE3) {
                if (c2 == 0xBC && c3 == 0x8C) return true;  /* ，*/
                if (c2 == 0x80 && c3 == 0x82) return true;  /* 。*/
            }
        }
        return false;
    };
    while (!text.empty() && IsPunctOrSpace(text, 0)) {
        size_t skip = (static_cast<unsigned char>(text[0]) <= 0x7F) ? 1 : 3;
        text.erase(0, skip);
    }
    while (!text.empty()) {
        /* 找最后一个 UTF-8 字符的起始位置 */
        size_t last = text.size() - 1;
        while (last > 0 && (static_cast<unsigned char>(text[last]) & 0xC0) == 0x80) last--;
        if (!IsPunctOrSpace(text, last)) break;
        text.erase(last);
    }

    std::cout << kTag << " ASR: \"" << text << "\"" << std::endl;

    /* ── 第1步：偏好修改检测 ── */
    agent::PreferenceResult pref = agent_core_->DetectAndApplyPreference(text);
    if (pref.modified) {
        std::cout << kTag << " 偏好修改: " << pref.message << std::endl;
        SafeTTS(pref.message);
        return;
    }

    /* ── 第2步：Action 关键词匹配（最高优先级，直接执行，不回注）── */
    agent::ActionResult action = action_mgr_->Match(text);
    if (action.handled) {
        std::cout << kTag << " 匹配 action: " << action.action << std::endl;

        /* 休眠特殊处理 */
        if (action.action == "action.sleep") {
            sleep_requested_ = true;
            SafeTTS("好的，小九去休息了");
            return;
        }

        /* action 直接执行 + TTS，不回注 LLM */
        agent_core_->AddTurn("user", text);
        agent_core_->AddTurn("assistant", action.response);
        SafeTTS(action.response);
        return;
    }

    /* ── 第3步：LLM Agent 循环（处理 skill、MCP、纯对话）── */
    std::cout << kTag << " 进入 LLM Agent 对话（上下文 "
              << agent_core_->HistorySize() << " 轮）" << std::endl;

    /* 发送工具定义（原生 function calling API） */
    llm_->SetTools(BuildToolDefs());

    agent_core_->AddTurn("user", text);
    std::string system_prompt = BuildSystemPrompt();
    int max_rounds = mcp_tools_->MaxRounds();
    std::string final_answer;

    for (int round = 0; round <= max_rounds; round++) {
        std::string messages_json = agent_core_->BuildMessagesJson("", system_prompt);
        llm_->SetMessages(messages_json);

        std::string llm_response;
        llm_->OnResult([&](const std::string& text, bool is_final) {
            if (is_final) llm_response = text;
        });

        if (!llm_->Chat("") || llm_response.empty()) {
            final_answer = "抱歉，网络不太好，请再说一遍";
            break;
        }

        std::cout << kTag << " [Agent R" << round << "] " << llm_response.substr(0, 80) << std::endl;

        /* 无工具调用 → 最终回复 */
        if (!mcp_tools_->ContainsToolCall(llm_response)) {
            final_answer = llm_response;
            break;
        }

        /* 解析工具调用 */
        auto calls = mcp_tools_->ParseToolCalls(llm_response);
        if (calls.empty()) {
            final_answer = StripToolCalls(llm_response);
            break;
        }

        std::cout << kTag << " [Agent R" << round << "] " << calls.size() << " 个工具调用" << std::endl;

        /* 优先级过滤：action > skill > MCP */
        std::vector<agent::ToolCall> filtered_calls;
        for (size_t i = 0; i < calls.size(); i++) {
            /* MCP 跳过规则：skill 或 action 已覆盖同功能 MCP */
            if (calls[i].name.find("mcp.") == 0) {
                std::string mcp_name = calls[i].name.substr(4);
                bool covered = false;
                for (size_t j = 0; j < calls.size(); j++) {
                    /* skill 覆盖的 MCP */
                    if (calls[j].name.find("skill.") == 0) {
                        const agent::SkillDef* s = skill_mgr_->FindSkill(calls[j].name);
                        if (s) {
                            for (size_t k = 0; k < s->mcp_tools.size(); k++) {
                                if (s->mcp_tools[k] == "mcp." + mcp_name) {
                                    covered = true; break;
                                }
                            }
                        }
                    }
                    /* action 不拦截 MCP（action 是硬件操作，不覆盖数据 MCP） */
                }
                if (covered) {
                    std::cout << kTag << " [Agent] 优先级过滤，跳过 " << calls[i].name << std::endl;
                    continue;
                }
            }
            filtered_calls.push_back(calls[i]);
        }

        /* 分别收集 action 和 skill/MCP 调用 */
        std::vector<agent::ToolCall> action_calls;
        std::vector<agent::ToolCall> other_calls;
        for (size_t i = 0; i < filtered_calls.size(); i++) {
            if (filtered_calls[i].name.find("action.") == 0) {
                action_calls.push_back(filtered_calls[i]);
            } else {
                other_calls.push_back(filtered_calls[i]);
            }
        }

        /* 执行所有 action */
        std::ostringstream action_results;
        for (size_t i = 0; i < action_calls.size(); i++) {
            ToolExecResult r = ExecuteSingleTool(action_calls[i]);
            if (r.success) {
                if (i > 0) action_results << "\n";
                action_results << r.content;
            }
        }

        /* 仅有 action（无 skill/MCP）→ 直接 TTS，不回注 */
        if (other_calls.empty() && !action_calls.empty()) {
            final_answer = action_results.str();
            break;
        }

        /* 有 skill/MCP → 执行后回注 LLM */
        std::string clean_text = StripToolCalls(llm_response);
        std::string tool_results = ExecuteToolCalls(other_calls);

        /* 先记录 LLM 本轮回复 */
        std::string llm_text = clean_text.empty() ? "（调用工具）" : clean_text;
        agent_core_->AddTurn("assistant", llm_text);

        /* 注入工具结果作为 user 消息（保持 user/assistant 交替） */
        std::ostringstream user_msg;
        if (!action_results.str().empty()) {
            user_msg << "[action 结果]\n" << action_results.str() << "\n";
        }
        user_msg << "[工具返回]\n" << tool_results;
        agent_core_->AddTurn("user", user_msg.str());

        /* 达到最大轮数 → 请求最终回复 */
        if (round >= max_rounds) {
            messages_json = agent_core_->BuildMessagesJson(
                "请根据以上工具结果，生成简洁的最终回复（不超过50字）。", system_prompt);
            llm_->SetMessages(messages_json);
            std::string last_response;
            llm_->OnResult([&](const std::string& text, bool is_final) {
                if (is_final) last_response = text;
            });
            if (llm_->Chat("") && !last_response.empty()) {
                final_answer = last_response;
            } else {
                final_answer = tool_results;
            }
            break;
        }
    }

    /* 记录最终回复 */
    agent_core_->AddTurn("assistant", final_answer);

    std::cout << kTag << " 最终回复: " << final_answer << std::endl;

    /* TTS 流式管线播报 */
    StartStreamingPipeline(final_answer);
}

/* ========== System Prompt 构建 ========== */

std::string Assistant::BuildSystemPrompt() const {
    std::string prompt = agent_core_->GetSystemPrompt();

    /* 替换 <actions> 占位符 — 全部 action 可见（keyword + LLM 均可触发，直接执行） */
    {
        std::string actions_desc = action_mgr_->GetActionsDescription();
        size_t pos = prompt.find("<actions>");
        size_t end = prompt.find("</actions>");
        if (pos != std::string::npos && end != std::string::npos) {
            prompt = prompt.substr(0, pos) + "\n" + actions_desc + "\n" + prompt.substr(end + 10);
        }
    }

    /* 替换 <skills> 占位符 — 仅发送描述摘要（Phase 1） */
    {
        std::string skills_desc = skill_mgr_->GetSkillsSummary();
        size_t pos = prompt.find("<skills>");
        size_t end = prompt.find("</skills>");
        if (pos != std::string::npos && end != std::string::npos) {
            prompt = prompt.substr(0, pos) + "\n" + skills_desc + "\n" + prompt.substr(end + 9);
        }
    }

    /* 替换 <mcp_tools> 占位符 — 全部 MCP 工具可见（JSON-RPC 2.0 格式） */
    {
        std::string tools_desc = mcp_tools_->GetToolsDescription();
        size_t pos = prompt.find("<mcp_tools>");
        size_t end = prompt.find("</mcp_tools>");
        if (pos != std::string::npos && end != std::string::npos) {
            prompt = prompt.substr(0, pos) + "\n" + tools_desc + "\n" + prompt.substr(end + 12);
        }
    }



    return prompt;
}

/* ========== 工具定义构建（原生 function calling API） ========== */

/* JSON 字符串转义（独立函数，非类方法） */
static std::string JsonEscape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default: r += c;
        }
    }
    return r;
}

static std::string BuildParamsSchema(const std::vector<agent::ParamSchema>& params) {
    if (params.empty()) {
        return "{\"type\":\"object\",\"properties\":{},\"required\":[]}";
    }
    std::ostringstream ss;
    ss << "{\"type\":\"object\",\"properties\":{";
    for (size_t i = 0; i < params.size(); i++) {
        if (i > 0) ss << ",";
        ss << "\"" << params[i].name << "\":{";
        ss << "\"type\":\"" << params[i].type << "\",";
        ss << "\"description\":\"" << JsonEscape(params[i].description) << "\"";
        if (!params[i].default_value.empty()) {
            ss << ",\"default\":\"" << JsonEscape(params[i].default_value) << "\"";
        }
        ss << "}";
    }
    ss << "},\"required\":[";
    bool first = true;
    for (size_t i = 0; i < params.size(); i++) {
        if (params[i].required) {
            if (!first) ss << ",";
            first = false;
            ss << "\"" << params[i].name << "\"";
        }
    }
    ss << "]}";
    return ss.str();
}

std::vector<LLMClient::ToolDef> Assistant::BuildToolDefs() const {
    std::vector<LLMClient::ToolDef> defs;

    /* 1. MCP 工具 */
    {
        const auto& mcp_tools = mcp_tools_->GetTools();
        for (const auto& t : mcp_tools) {
            LLMClient::ToolDef def;
            def.name = "mcp." + t.name;
            def.description = t.description;
            def.parameters_json = BuildParamsSchema(t.params);
            defs.push_back(std::move(def));
        }
    }

    /* 2. Skills */
    {
        const auto& skills = skill_mgr_->GetSkills();
        for (const auto& s : skills) {
            LLMClient::ToolDef def;
            def.name = s.action.empty() ? "skill." + s.name : s.action;
            def.description = s.description;
            def.parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}";
            defs.push_back(std::move(def));
        }
    }

    /* 3. Actions（带参 action 有参数，其余无参） */
    {
        const auto& actions = action_mgr_->GetActions();
        for (const auto& a : actions) {
            LLMClient::ToolDef def;
            def.name = a.action;
            def.description = a.description;
            if (a.action == "action.set_preference") {
                def.parameters_json = "{\"type\":\"object\",\"properties\":{"
                    "\"key\":{\"type\":\"string\",\"description\":\"偏好名称\"},"
                    "\"value\":{\"type\":\"string\",\"description\":\"偏好值\"}"
                    "},\"required\":[\"key\",\"value\"]}";
            } else if (a.action == "action.delete_preference") {
                def.parameters_json = "{\"type\":\"object\",\"properties\":{"
                    "\"key\":{\"type\":\"string\",\"description\":\"要删除的偏好名称或关键词\"}"
                    "},\"required\":[\"key\"]}";
            } else {
                def.parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}";
            }
            defs.push_back(std::move(def));
        }
    }

    return defs;
}
