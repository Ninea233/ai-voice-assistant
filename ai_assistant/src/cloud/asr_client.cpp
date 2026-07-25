/*
 * asr_client.cpp
 * ASR 客户端实现 — 讯飞中文识别大模型 (WebSocket) + 多模型表决
 *
 * 引擎：mandarin（中文识别，始终运行）
 *        dialect（方言识别，可选，由 config 控制）
 * 表决：confidence（置信度加权）| majority（多数投票）
 *
 * 流程（单引擎）：
 *   Connect(auth_url) → Send JSON start → Send Audio chunks
 *   → Send JSON end → Recv JSON results → Close
 */

#include "assistant/cloud/asr_client.h"
#include "assistant/cloud/ws_client.h"
#include "assistant/cloud/xfyun_auth.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>

/* 前向声明 */
static std::string ExtractWsText(const std::string& json);
static float ExtractResultConfidence(const std::string& json);

static const char* kTag = "[ASRClient]";

/* 音频分块大小（每块约 60ms @16kHz = 960 samples） */
static const size_t kChunkSamples = 960;

/* ========== 简易 JSON 工具 ========== */

/* 提取 JSON 字符串值 */
static std::string ExtractJsonStr(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        search = "\"" + key + "\": \"";
        pos = json.find(search);
        if (pos == std::string::npos) return "";
    }
    pos += search.size();
    size_t end = pos;
    while (end < json.size()) {
        if (json[end] == '\\') end += 2;
        else if (json[end] == '\"') break;
        else end++;
    }
    if (end >= json.size()) return "";
    std::string result;
    for (size_t i = pos; i < end; i++) {
        if (json[i] == '\\' && i + 1 < end) { i++; result += json[i]; }
        else result += json[i];
    }
    return result;
}

/* 提取 JSON 整数值 */
static int ExtractJsonInt(const std::string& json, const std::string& key, int def = 0) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        search = "\"" + key + "\": ";
        pos = json.find(search);
        if (pos == std::string::npos) return def;
    }
    pos = json.find_first_of("0123456789-", pos + search.size());
    if (pos == std::string::npos) return def;
    char* end = nullptr;
    return static_cast<int>(std::strtol(json.c_str() + pos, &end, 10));
}

/* 提取 JSON 浮点值 */
static float ExtractJsonFloat(const std::string& json, const std::string& key, float def = 0.0f) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        search = "\"" + key + "\": ";
        pos = json.find(search);
        if (pos == std::string::npos) return def;
    }
    pos = json.find_first_of("0123456789-.", pos + search.size());
    if (pos == std::string::npos) return def;
    char* end = nullptr;
    return std::strtof(json.c_str() + pos, &end);
}

/* Base64 编码 */
static std::string Base64EncodeData(const uint8_t* data, size_t len) {
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);
    BIO_write(bio, data, static_cast<int>(len));
    BIO_flush(bio);
    char* buf = nullptr;
    long buf_len = BIO_get_mem_data(bio, &buf);
    std::string result(buf, static_cast<size_t>(buf_len));
    BIO_free_all(bio);
    return result;
}

/* ========== ASR 引擎接口 ========== */

struct ASREngineResult {
    std::string text;
    float confidence = 0.0f;
    std::string name;  // "mandarin", "dialect"
};

/* 单引擎 ASR 识别 */
static ASREngineResult RecognizeEngine(
    const std::string& name,
    const std::string& auth_url,
    const std::string& app_id,
    const std::string& language,      // "zh_cn"
    const std::string& accent,        // "mandarin"
    const std::vector<int16_t>& audio_data,
    bool debug_mode,
    std::function<void(const std::string&)> debug_log,
    const std::string& auth_date = "",   // RFC 1123 日期，设为 WebSocket Date 头
    const std::string& res_id = "") {    // 中文识别大模型资源 ID

    ASREngineResult result;
    result.name = name;

    auto log = [&](const std::string& msg) {
        if (debug_mode && debug_log) debug_log(msg);
    };

    log("[引擎 " + name + "] 开始识别 (" + std::to_string(audio_data.size()) + " samples)");

    /* 连接 WebSocket */
    WebSocketClient ws;
    ws.SetDebugMode(debug_mode);
    if (debug_mode) {
        ws.SetDebugCallback([name, &log](const std::string& msg) {
            log("[引擎 " + name + "|WS] " + msg);
        });
    }

    /* 讯飞 HMAC 认证需要 Date/x-date 头与签名中的日期一致 */
    if (!auth_date.empty()) {
        ws.SetHeader("Date", auth_date);
        ws.SetHeader("x-date", auth_date);    /* 部分 Kong 配置只认 x-date */
    }

    if (!ws.Connect(auth_url)) {
        std::cerr << kTag << " [引擎 " << name << "] WebSocket 连接失败" << std::endl;
        return result;
    }

    log("[引擎 " + name + "] WebSocket 连接成功");

    /* 1. 发送开始帧 */
    std::ostringstream start_json;
    start_json << "{"
               << "\"common\":{\"app_id\":\"" << app_id << "\"},"
               << "\"business\":{"
               << "\"domain\":\"iat\","
               << "\"language\":\"" << language << "\","
               << "\"accent\":\"" << accent << "\","
               << "\"vad_eos\":2000,"
               << "\"dwa\":\"wpgs\"";
    if (!res_id.empty()) {
        start_json << ",\"res_id\":\"" << res_id << "\"";
    }
    start_json << "},"
               << "\"data\":{"
               << "\"status\":0,"
               << "\"format\":\"audio/L16;rate=16000\","
               << "\"encoding\":\"raw\""
               << "}}";

    log("[引擎 " + name + "] 发送开始帧");
    if (!ws.SendText(start_json.str())) {
        std::cerr << kTag << " [引擎 " << name << "] 发送开始帧失败" << std::endl;
        ws.Close();
        return result;
    }

    /* 2. 发送音频数据 */
    size_t total_samples = audio_data.size();
    size_t offset = 0;

    /* 音频帧在 WebSocket 中作为二进制帧发送，带业务 JSON 包裹 */
    while (offset < total_samples) {
        size_t chunk = std::min(kChunkSamples, total_samples - offset);
        std::vector<int16_t> chunk_data(audio_data.begin() + offset,
                                         audio_data.begin() + offset + chunk);

        std::string audio_b64 = Base64EncodeData(
            reinterpret_cast<const uint8_t*>(chunk_data.data()),
            chunk_data.size() * sizeof(int16_t));

        int status = (offset + chunk >= total_samples) ? 2 : 1;

        std::ostringstream audio_json;
        audio_json << "{"
                   << "\"data\":{"
                   << "\"status\":" << status << ","
                   << "\"format\":\"audio/L16;rate=16000\","
                   << "\"encoding\":\"raw\","
                   << "\"audio\":\"" << audio_b64 << "\""
                   << "}}";

        if (!ws.SendText(audio_json.str())) {
            std::cerr << kTag << " [引擎 " << name << "] 发送音频失败 (offset="
                      << offset << ")" << std::endl;
            if (offset > 0) {
                /* 已发送部分音频，服务器可能已处理完毕关闭连接，尝试接收响应 */
                log("[引擎 " + name + "] 部分音频已发送，尝试接收服务器响应...");
                break;
            }
            ws.Close();
            return result;
        }

        offset += chunk;
    }

    log("[引擎 " + name + "] 音频发送完成, 等待识别结果...");

    /* 3. 接收识别结果 */
    std::string full_text;
    float best_confidence = 0.0f;
    bool got_result = false;

    while (true) {
        std::string resp_text;
        if (!ws.RecvText(resp_text)) {
            break;
        }

        log("[引擎 " + name + "] 收到响应 (" + std::to_string(resp_text.size()) + "B)");

        /* 解析结果 */
        int code = ExtractJsonInt(resp_text, "code", 0);
        if (code != 0) {
            std::string message = ExtractJsonStr(resp_text, "message");
            std::cerr << kTag << " [引擎 " << name << "] API 错误: code="
                      << code << " msg=" << message << std::endl;
            break;
        }

        /* 提取文本（从 ws 数组中拼接） */
        std::string partial = ExtractWsText(resp_text);

        /* 提取置信度 */
        float conf = ExtractResultConfidence(resp_text);

        if (!partial.empty()) {
            got_result = true;
            /* wpgs 模式下每帧返回累积文本，保留最长结果（末帧可能是仅标点的增量） */
            if (partial.size() >= full_text.size()) {
                full_text = partial;
            }
            if (conf > best_confidence) best_confidence = conf;

            log("[引擎 " + name + "] 中间结果: \"" + partial
                + "\" (confidence=" + std::to_string(conf) + ")");
        }

        /* 检查是否结束 */
        if (ExtractJsonInt(resp_text, "status", 0) == 2) {
            break;
        }
    }

    ws.Close();

    if (!got_result) {
        log("[引擎 " + name + "] 无识别结果");
        return result;
    }

    result.text = full_text;
    result.confidence = (best_confidence > 0.0f) ? best_confidence : 0.5f;

    log("[引擎 " + name + "] 识别完成: \"" + full_text
        + "\" (confidence=" + std::to_string(result.confidence) + ")");

    return result;
}

/* 从 ASR 结果 JSON 中提取 ws 拼接文本 */
static std::string ExtractWsText(const std::string& json) {
    std::string text;
    size_t pos = 0;
    while (true) {
        /* 查找 "w":" 或 "w": " */
        size_t ws_pos = json.find("\"w\":\"", pos);
        if (ws_pos == std::string::npos) {
            ws_pos = json.find("\"w\": \"", pos);
            if (ws_pos == std::string::npos) break;
            ws_pos += 5;
        } else {
            ws_pos += 5;
        }
        size_t end = ws_pos;
        while (end < json.size()) {
            if (json[end] == '\\') end += 2;
            else if (json[end] == '\"') break;
            else end++;
        }
        if (end >= json.size()) break;
        text += json.substr(ws_pos, end - ws_pos);
        pos = end + 1;
    }
    return text;
}

/* 从 ASR 结果中提取置信度（取第一个 cw 的 sc） */
static float ExtractResultConfidence(const std::string& json) {
    float conf = ExtractJsonFloat(json, "sc", 0.0f);
    return conf;
}

/* ========== 表决策略 ========== */

/* 置信度加权表决 */
static ASREngineResult VoteByConfidence(
    const std::vector<ASREngineResult>& results) {

    if (results.empty()) return {};
    if (results.size() == 1) return results[0];

    /* 找最高置信度 */
    size_t best_idx = 0;
    float best_conf = results[0].confidence;
    for (size_t i = 1; i < results.size(); i++) {
        if (results[i].confidence > best_conf) {
            best_conf = results[i].confidence;
            best_idx = i;
        }
    }

    /* 如果最高置信度显著高于其他（>0.15），直接采用 */
    for (size_t i = 0; i < results.size(); i++) {
        if (i != best_idx && results[i].text == results[best_idx].text) {
            /* 文本一致，累加置信度 */
            best_conf += results[i].confidence;
        }
    }

    ASREngineResult voted = results[best_idx];
    voted.confidence = std::min(best_conf, 1.0f);
    voted.name = "voting";

    return voted;
}

/* ========== ASR 实现（支持多引擎表决） ========== */

class ASRClientImpl : public ASRClient {
public:
    ASRClientImpl() = default;
    ~ASRClientImpl() override = default;

    bool Initialize(const std::string& app_id,
                    const std::string& api_key,
                    const std::string& api_secret,
                    bool debug_mode = false) override {
        app_id_ = app_id;
        api_key_ = api_key;
        api_secret_ = api_secret;
        debug_mode_ = debug_mode;

        /* 默认 ASR 地址 */
        asr_main_url_ = "wss://iat-api.xfyun.cn/v2/iat";

        /* 方言引擎默认关闭 */
        dialect_enabled_ = false;
        dialect_url_ = "wss://iat-api.xfyun.cn/v2/iat";

        voting_strategy_ = "confidence";

        std::cout << kTag << " 初始化完成"
                  << " (dialect=" << (dialect_enabled_ ? "on" : "off")
                  << ", debug=" << (debug_mode_ ? "on" : "off") << ")"
                  << std::endl;
        return true;
    }

    /* 设置方言引擎 */
    bool SetDialect(const std::string& dialect) override {
        if (dialect.empty() || dialect == "none") {
            dialect_enabled_ = false;
        } else {
            dialect_enabled_ = true;
            /* 方言类型：canton（粤语）、sichuan（四川话）等 */
            dialect_accent_ = dialect;
        }
        if (debug_mode_) {
            std::cout << kTag << " [调试] 方言: " << (dialect_enabled_ ? dialect : "关闭") << std::endl;
        }
        return true;
    }

    bool StartRecognition() override {
        std::cout << kTag << " 开始流式识别" << std::endl;
        asr_stream_buf_.clear();
        asr_stream_partial_.clear();

        if (api_key_.empty() || api_key_ == "0000000000000000") {
            /* 凭证未配置，模拟模式：仅累积音频，Stop 时返回模拟文本 */
            asr_streaming_ = true;
            return true;
        }

        /* 构建认证 URL */
        std::string auth_date;
        std::string auth_url = xfyun::BuildAuthUrl(
            api_key_, api_secret_, asr_main_url_, &auth_date);

        /* 打开 WebSocket */
        asr_ws_ = std::unique_ptr<WebSocketClient>(new WebSocketClient());
        asr_ws_->SetDebugMode(debug_mode_);
        if (debug_mode_) {
            asr_ws_->SetDebugCallback([this](const std::string& msg) {
                std::cout << kTag << " [WS] " << msg << std::endl;
            });
        }
        asr_ws_->SetHeader("Date", auth_date);
        asr_ws_->SetHeader("x-date", auth_date);

        if (!asr_ws_->Connect(auth_url)) {
            std::cerr << kTag << " WebSocket 连接失败" << std::endl;
            asr_ws_.reset();
            return false;
        }

        /* 发送开始帧 */
        std::ostringstream start_json;
        start_json << "{"
                   << "\"common\":{\"app_id\":\"" << app_id_ << "\"},"
                   << "\"business\":{"
                   << "\"domain\":\"iat\","
                   << "\"language\":\"zh_cn\","
                   << "\"accent\":\"mandarin\","
                   << "\"vad_eos\":2000,"
                   << "\"dwa\":\"wpgs\"";
        if (!res_id_.empty()) {
            start_json << ",\"res_id\":\"" << res_id_ << "\"";
        }
        start_json << "},"
                   << "\"data\":{"
                   << "\"status\":0,"
                   << "\"format\":\"audio/L16;rate=16000\","
                   << "\"encoding\":\"raw\""
                   << "}}";

        if (!asr_ws_->SendText(start_json.str())) {
            std::cerr << kTag << " 发送开始帧失败" << std::endl;
            asr_ws_->Close();
            asr_ws_.reset();
            return false;
        }

        asr_streaming_ = true;
        std::cout << kTag << " 流式识别已就绪" << std::endl;
        return true;
    }

    bool SendAudio(const std::vector<int16_t>& audio_data) override {
        if (!asr_streaming_) return false;
        if (audio_data.empty()) return true;

        /* 凭证未配置 → 模拟模式 */
        if (!asr_ws_) {
            asr_stream_buf_.insert(asr_stream_buf_.end(),
                                   audio_data.begin(), audio_data.end());
            return true;
        }

        /* Base64 编码并发送 */
        std::string audio_b64 = Base64EncodeData(
            reinterpret_cast<const uint8_t*>(audio_data.data()),
            audio_data.size() * sizeof(int16_t));

        std::ostringstream audio_json;
        audio_json << "{"
                   << "\"data\":{"
                   << "\"status\":1,"
                   << "\"format\":\"audio/L16;rate=16000\","
                   << "\"encoding\":\"raw\","
                   << "\"audio\":\"" << audio_b64 << "\""
                   << "}}";

        if (!asr_ws_->SendText(audio_json.str())) {
            std::cerr << kTag << " 发送音频帧失败" << std::endl;
            return false;
        }

        /* 注：不在此处同步读取部分结果（WebSocket Recv 会阻塞）。
         * 部分结果将在 StopRecognition 中一次性读取。 */
        return true;
    }

    std::string StopRecognition() override {
        if (!asr_streaming_) return "";
        asr_streaming_ = false;

        /* 凭证未配置 → 模拟模式 */
        if (!asr_ws_) {
            std::cout << kTag << " [模拟] 流式识别完成 ("
                      << (asr_stream_buf_.size() / 16.0) << "s)" << std::endl;
            std::string mock = "今天天气怎么样";
            if (result_callback_) {
                result_callback_(mock, true, 1.0f);
            }
            return mock;
        }

        /* 发送结束帧 */
        std::ostringstream end_json;
        end_json << "{"
                 << "\"data\":{"
                 << "\"status\":2,"
                 << "\"format\":\"audio/L16;rate=16000\","
                 << "\"encoding\":\"raw\","
                 << "\"audio\":\"\""
                 << "}}";

        asr_ws_->SendText(end_json.str());

        /* 接收所有剩余的识别结果 */
        std::string full_text = asr_stream_partial_;
        float best_conf = 0.0f;

        while (true) {
            std::string resp_text;
            if (!asr_ws_->RecvText(resp_text) || resp_text.empty()) break;

            std::string ws_text = ExtractWsText(resp_text);
            float conf = ExtractResultConfidence(resp_text);

            if (!ws_text.empty()) {
                full_text = ws_text;  /* wpgs 模式用最新结果覆盖 */
                if (conf > best_conf) best_conf = conf;
            }
        }

        asr_ws_->Close();
        asr_ws_.reset();

        std::cout << kTag << " 流式识别完成: \"" << full_text
                  << "\" (置信度 " << best_conf << ")" << std::endl;

        if (result_callback_) {
            result_callback_(full_text, true, best_conf);
        }

        return full_text;
    }

    /* 非流式完整识别 */
    ASRResult Recognize(const std::vector<int16_t>& audio_data) override {
        if (audio_data.empty()) {
            std::cerr << kTag << " 音频数据为空" << std::endl;
            return {};
        }

        std::cout << kTag << " 识别 " << audio_data.size()
                  << " samples (" << (audio_data.size() / 16.0) << "s)" << std::endl;

        /* 凭证为空时回退 */
        if (api_key_.empty() || api_key_ == "0000000000000000") {
            std::cout << kTag << " [回退] API 凭证未配置，返回模拟文本" << std::endl;
            ASRResult mock;
            mock.text = "今天天气怎么样";
            mock.confidence = 1.0f;
            mock.provider = "mock";
            if (result_callback_) {
                result_callback_(mock.text, true, mock.confidence);
            }
            return mock;
        }

        /* 保存调试音频（固定文件名，每次覆盖，最多保留一条） */
        if (debug_mode_) {
            std::string filename = "asr_debug_input.pcm";
            std::ofstream ofs(filename, std::ios::binary);
            if (ofs.is_open()) {
                ofs.write(reinterpret_cast<const char*>(audio_data.data()),
                          audio_data.size() * sizeof(int16_t));
                ofs.close();
                std::cout << kTag << " [调试] 已保存输入音频: " << filename
                          << " (" << (audio_data.size() / 16.0) << "s)" << std::endl;
            }
        }

        /* 构建认证 URL（同时获取签名用的日期，用于设置 Date 头） */
        std::string auth_date;
        std::string main_auth_url = xfyun::BuildAuthUrl(
            api_key_, api_secret_, asr_main_url_, &auth_date);

        /* 结果收集 */
        std::vector<ASREngineResult> engine_results;

        /* ----- 引擎 1: 中文识别（主引擎）----- */
        {
            ASREngineResult r = RecognizeEngine(
                "mandarin", main_auth_url, app_id_,
                "zh_cn", "mandarin", audio_data,
                debug_mode_, [this](const std::string& msg) {
                    if (debug_mode_) std::cout << kTag << " [调试] " << msg << std::endl;
                }, auth_date, res_id_);
            if (!r.text.empty()) {
                engine_results.push_back(r);
            }
        }

        /* ----- 引擎 2: 方言识别（可选）----- */
        if (dialect_enabled_) {
            std::string dialect_auth_date;
            std::string dialect_auth_url = xfyun::BuildAuthUrl(
                api_key_, api_secret_, dialect_url_, &dialect_auth_date);

            /* 方言 accent 映射 */
            std::string accent = "mandarin";
            if (dialect_accent_ == "canton" || dialect_accent_ == "cantonese") accent = "cantonese";
            else if (dialect_accent_ == "sichuan") accent = "sichuan";
            else if (dialect_accent_ == "henan") accent = "henan";

            ASREngineResult r = RecognizeEngine(
                "dialect", dialect_auth_url, app_id_,
                "zh_cn", accent, audio_data,
                debug_mode_, [this](const std::string& msg) {
                    if (debug_mode_) std::cout << kTag << " [调试] " << msg << std::endl;
                }, dialect_auth_date);
            if (!r.text.empty()) {
                engine_results.push_back(r);
            }
        }

        /* ----- 表决 ----- */
        ASRResult final_result;

        if (engine_results.empty()) {
            std::cerr << kTag << " 所有引擎均未返回结果" << std::endl;
            return final_result;
        }

        /* 投票 */
        if (engine_results.size() == 1) {
            final_result.text = engine_results[0].text;
            final_result.confidence = engine_results[0].confidence;
            final_result.provider = engine_results[0].name;
        } else {
            ASREngineResult voted = VoteByConfidence(engine_results);
            final_result.text = voted.text;
            final_result.confidence = voted.confidence;
            final_result.provider = voted.name;
        }

        std::cout << kTag << " 识别结果: \"" << final_result.text << "\""
                  << " (confidence=" << final_result.confidence
                  << ", provider=" << final_result.provider << ")"
                  << std::endl;

        /* 回调通知 */
        if (result_callback_) {
            result_callback_(final_result.text, true, final_result.confidence);
        }

        return final_result;
    }

    void SetDebugMode(bool enable) override {
        debug_mode_ = enable;
    }

    /* 从外部设置方言引擎启用状态 */
    void SetDialectEnabled(bool enabled, const std::string& accent = "") {
        dialect_enabled_ = enabled;
        if (!accent.empty()) dialect_accent_ = accent;
    }

    /* 设置表决策略 */
    void SetVotingStrategy(const std::string& strategy) {
        voting_strategy_ = strategy;
    }

    /* 设置 ASR 地址 */
    void SetMainUrl(const std::string& url) { asr_main_url_ = url; }
    void SetDialectUrl(const std::string& url) { dialect_url_ = url; }

    /* 设置中文识别大模型资源 ID（res_id） */
    void SetResId(const std::string& id) override { res_id_ = id; }

private:
    std::string app_id_;
    std::string api_key_;
    std::string api_secret_;
    std::string res_id_;        /* 中文识别大模型资源 ID */
    std::string asr_main_url_;
    std::string dialect_url_;
    std::string dialect_accent_ = "mandarin";
    std::string voting_strategy_ = "confidence";
    bool dialect_enabled_ = false;
    bool debug_mode_ = false;

    /* 流式识别状态 */
    std::unique_ptr<WebSocketClient> asr_ws_;      /* 持久 WebSocket */
    std::vector<int16_t> asr_stream_buf_;           /* 模拟模式音频缓冲 */
    std::string asr_stream_partial_;                /* 累积分部分文本 */
    bool asr_streaming_ = false;                    /* 流式会话是否活跃 */
};

/* ========== 工厂函数 ========== */

std::unique_ptr<ASRClient> CreateASRClient() {
    return std::make_unique<ASRClientImpl>();
}
