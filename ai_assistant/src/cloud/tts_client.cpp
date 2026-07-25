/*
 * tts_client.cpp
 * TTS 客户端 — 支持 SparkChain SDK 和 WebSocket 两种实现
 *
 * 实现选择：
 *   如果编译时定义了 ENABLE_SPARKCHAIN_SDK，使用 SDK 实现
 *   否则使用 WebSocket 实现（无额外依赖）
 *
 * SDK 文档：https://www.xfyun.cn/doc/spark/smart_tts_linux.html
 */

#include "assistant/cloud/tts_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

static const char* kTag = "[TTSClient]";

/* =========================================================
 * 实现 1: SparkChain SDK（编译时启用 ENABLE_SPARKCHAIN_SDK）
 * ========================================================= */
#ifdef ENABLE_SPARKCHAIN_SDK

#include "sparkchain.h"
#include "sc_tts.h"
namespace SDK = SparkChain;

/* SDK 全局初始化引用计数 */
static std::atomic<int> g_sdk_refcount{0};
static std::mutex g_sdk_mutex;

class TTSClientSDK : public TTSClient {
public:
    TTSClientSDK() = default;
    ~TTSClientSDK() override { UninitSDK(); }

    bool Initialize(const std::string& app_id,
                    const std::string& api_key,
                    const std::string& api_secret,
                    bool debug_mode = false,
                    const std::string& auth_token = "") override {
        (void)auth_token;
        debug_mode_ = debug_mode;
        app_id_ = app_id;
        api_key_ = api_key;
        api_secret_ = api_secret;
        voice_ = "xiaoyan";

        if (!InitSDK()) {
            std::cerr << kTag << " SDK 初始化失败" << std::endl;
            return false;
        }

        std::cout << kTag << " [SparkChain SDK] 初始化完成"
                  << " (debug=" << (debug_mode_ ? "on" : "off") << ")"
                  << std::endl;
        return true;
    }

    void SetVoice(const std::string& voice_name) override {
        voice_ = voice_name;
        if (debug_mode_) {
            std::cout << kTag << " [调试] 设置音色: " << voice_ << std::endl;
        }
    }

    bool Synthesize(const std::string& text) override {
        if (text.empty()) {
            std::cerr << kTag << " 合成文本为空" << std::endl;
            return false;
        }

        std::cout << kTag << " 合成: \"" << text.substr(0, 40) << "...\""
                  << " (" << text.size() << "字符)" << std::endl;

        /* 准备回调 */
        finish_ = false;
        all_pcm_.clear();

        /* 创建 TTS 实例 */
        SDK::OnlineTTS tts(voice_);
        Callbacks cbs;
        cbs.client = this;
        tts.registerCallbacks(&cbs);
        tts.setParams("rdn", "1");

        /* 调试模式：设置日志级别 */
        if (debug_mode_) {
            tts.setParams("logLevel", 1);
        }

        /* 异步合成 */
        int ret = tts.arun(text, nullptr);
        if (ret != 0) {
            std::cerr << kTag << " 合成请求失败: ret=" << ret << std::endl;
            FallbackSilence();
            return false;
        }

        /* 等待结果（最长 30 秒） */
        int wait_count = 0;
        while (!finish_ && wait_count < 300) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wait_count++;
        }

        if (!finish_) {
            std::cerr << kTag << " 合成超时" << std::endl;
            tts.stop();
            FallbackSilence();
            return false;
        }

        if (all_pcm_.empty()) {
            std::cerr << kTag << " 未收到音频数据" << std::endl;
            FallbackSilence();
            return false;
        }

        std::cout << kTag << " 合成完成: "
                  << all_pcm_.size() << " samples @16kHz "
                  << "(" << (all_pcm_.size() / 16000.0) << "s)" << std::endl;

        /* 调试保存 */
        if (debug_mode_) {
            SaveDebugPCM(all_pcm_);
        }

        if (audio_callback_) {
            audio_callback_(all_pcm_);
        }
        return true;
    }

    void SetDebugMode(bool enable) override {
        debug_mode_ = enable;
    }

private:
    /* SDK 全局初始化 */
    bool InitSDK() {
        std::lock_guard<std::mutex> lock(g_sdk_mutex);
        if (g_sdk_refcount.load() > 0) {
            g_sdk_refcount++;
            return true;
        }

        SDK::SparkChainConfig* config = SDK::SparkChainConfig::builder();
        if (!config) {
            std::cerr << kTag << " SparkChainConfig::builder() 失败" << std::endl;
            return false;
        }

        config->appID(app_id_.c_str())
              ->apiKey(api_key_.c_str())
              ->apiSecret(api_secret_.c_str())
              ->workDir("./")
              ->logPath("sparkchain.log")
              ->logLevel(debug_mode_ ? 1 : 3);

        int ret = SDK::SparkChain::init(config);
        if (ret != 0) {
            std::cerr << kTag << " SparkChain::init() 失败: ret=" << ret << std::endl;
            return false;
        }

        g_sdk_refcount = 1;
        std::cout << kTag << " SparkChain SDK v"
                  << SDK::getSparkChainVersion() << " 初始化成功" << std::endl;
        return true;
    }

    void UninitSDK() {
        std::lock_guard<std::mutex> lock(g_sdk_mutex);
        if (g_sdk_refcount.load() > 0) {
            g_sdk_refcount--;
            if (g_sdk_refcount.load() == 0) {
                SDK::SparkChain::unInit();
                std::cout << kTag << " SparkChain SDK 已卸载" << std::endl;
            }
        }
    }

    void FallbackSilence() {
        if (audio_callback_) {
            audio_callback_(std::vector<int16_t>(8000, 0));
        }
    }

    void SaveDebugPCM(const std::vector<int16_t>& pcm) {
        /* 固定文件名，每次覆盖，最多保留一条 */
        std::string filename = "tts_debug.pcm";
        std::ofstream ofs(filename, std::ios::binary);
        if (ofs.is_open()) {
            ofs.write(reinterpret_cast<const char*>(pcm.data()),
                      pcm.size() * sizeof(int16_t));
            ofs.close();
            std::cout << kTag << " [调试] 已保存 PCM: " << filename
                      << " (" << (pcm.size() / 16000.0) << "s)" << std::endl;
        }
    }

    /* 回调类 */
    class Callbacks : public SDK::TTSCallbacks {
    public:
        TTSClientSDK* client = nullptr;

        void onResult(SDK::TTSResult* result, void* usrTag) override {
            (void)usrTag;
            if (!client || !result) return;

            const char* data = result->data();
            size_t len = result->len();
            int status = result->status();

            if (data && len > 0) {
                /* 将音频数据转换为 int16 PCM */
                size_t samples = len / 2;
                std::vector<int16_t> pcm(samples);
                for (size_t i = 0; i < samples && i * 2 + 1 < len; i++) {
                    pcm[i] = static_cast<int16_t>(
                        static_cast<uint16_t>(data[i * 2]) |
                        (static_cast<uint16_t>(data[i * 2 + 1]) << 8));
                }
                client->all_pcm_.insert(client->all_pcm_.end(),
                                        pcm.begin(), pcm.end());
            }

            if (status == 2) {
                client->finish_ = true;
            }
        }

        void onError(SDK::TTSError* error, void* usrTag) override {
            (void)usrTag;
            if (!error) return;
            std::cerr << kTag << " SDK 错误: code=" << error->code()
                      << " msg=" << error->errMsg()
                      << " sid=" << error->sid() << std::endl;
            if (client) client->finish_ = true;
        }
    };

    std::string app_id_;
    std::string api_key_;
    std::string api_secret_;
    std::string voice_;
    bool debug_mode_ = false;
    bool finish_ = false;
    std::vector<int16_t> all_pcm_;
};

std::unique_ptr<TTSClient> CreateTTSClient() {
    return std::make_unique<TTSClientSDK>();
}

/* =========================================================
 * 实现 2: WebSocket 直调用（后备，无需 SDK）
 * ========================================================= */
#else

#include "assistant/cloud/ws_client.h"
#include "assistant/cloud/xfyun_auth.h"
#include <openssl/evp.h>

/* Base64 解码 */
static std::vector<uint8_t> Base64Decode(const std::string& input) {
    if (input.empty()) return {};
    BIO* bio = BIO_new_mem_buf(input.data(), static_cast<int>(input.size()));
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);
    std::vector<uint8_t> result(input.size());
    int len = BIO_read(bio, result.data(), static_cast<int>(result.size()));
    BIO_free_all(bio);
    if (len < 0) len = 0;
    result.resize(static_cast<size_t>(len));
    return result;
}

static std::string TextToBase64(const std::string& text) {
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);
    BIO_write(bio, text.data(), static_cast<int>(text.size()));
    BIO_flush(bio);
    char* buf = nullptr;
    long len = BIO_get_mem_data(bio, &buf);
    std::string result(buf, static_cast<size_t>(len));
    BIO_free_all(bio);
    return result;
}

/* 简易 JSON 提取 */
static std::string ExtractJsonStr(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) { search = "\"" + key + "\": \""; pos = json.find(search); }
    if (pos == std::string::npos) return "";
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

static int ExtractJsonInt(const std::string& json, const std::string& key, int def = 0) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) { search = "\"" + key + "\": "; pos = json.find(search); }
    if (pos == std::string::npos) return def;
    pos = json.find_first_of("0123456789-", pos + search.size());
    if (pos == std::string::npos) return def;
    char* end = nullptr;
    return static_cast<int>(std::strtol(json.c_str() + pos, &end, 10));
}

class TTSClientWS : public TTSClient {
public:
    TTSClientWS() = default;
    ~TTSClientWS() override = default;

    bool Initialize(const std::string& app_id,
                    const std::string& api_key,
                    const std::string& api_secret,
                    bool debug_mode = false,
                    const std::string& auth_token = "") override {
        app_id_ = app_id;
        api_key_ = api_key;
        api_secret_ = api_secret;
        debug_mode_ = debug_mode;
        auth_token_ = auth_token;
        tts_url_ = "wss://cbm01.cn-huabei-1.xf-yun.com/v1/private/mcd9m97e6";
        voice_ = "x6_lingyuyan_pro";
        std::cout << kTag << " [WebSocket] 初始化完成"
                  << " (debug=" << (debug_mode_ ? "on" : "off")
                  << ", auth=" << (auth_token_.empty() ? "hmac" : "x-api-key") << ")"
                  << std::endl;
        return true;
    }

    void SetVoice(const std::string& voice_name) override { voice_ = voice_name; }

    bool Synthesize(const std::string& text) override {
        if (text.empty()) return false;
        std::cout << kTag << " 合成: \"" << text.substr(0, 40) << "...\""
                  << " (" << text.size() << "字符)" << std::endl;

        if (!auth_token_.empty()) {
            /* 使用 x-api-key 头认证（超拟人合成 APIPassword 方式） */
            if (debug_mode_) {
                std::cout << kTag << " [调试] 使用 x-api-key 认证" << std::endl;
            }
        } else if (api_key_.empty() || api_key_ == "0000000000000000") {
            if (audio_callback_) audio_callback_(std::vector<int16_t>(8000, 0));
            return true;
        }

        WebSocketClient ws;
        ws.SetDebugMode(debug_mode_);

        std::string connect_url;
        if (!auth_token_.empty()) {
            /* 方式一：x-api-key 简单认证，直接使用原始 URL */
            connect_url = tts_url_;
            ws.SetHeader("x-api-key", auth_token_);
        } else {
            /* 方式二：HMAC-SHA256 签名认证，需设置 Date 头 */
            std::string auth_date;
            connect_url = xfyun::BuildAuthUrl(api_key_, api_secret_, tts_url_, &auth_date);
            if (!auth_date.empty()) {
                ws.SetHeader("Date", auth_date);
                ws.SetHeader("x-date", auth_date);
            }
        }

        if (!ws.Connect(connect_url)) {
            std::cerr << kTag << " WebSocket 连接失败" << std::endl;
            if (audio_callback_) audio_callback_(std::vector<int16_t>(8000, 0));
            return false;
        }

        /* 构建合成请求（超拟人合成使用 header/parameter/payload 新版格式） */
        std::string text_b64 = TextToBase64(text);
        std::ostringstream req;
        req << "{\"header\":{\"app_id\":\"" << app_id_ << "\",\"status\":1},"
            << "\"parameter\":{\"tts\":{"
            << "\"vcn\":\"" << voice_ << "\",\"speed\":50,\"pitch\":50,\"volume\":50"
            << "}},"
            << "\"payload\":{\"text\":{\"text\":\"" << text_b64 << "\",\"status\":2}}}";

        if (debug_mode_) {
            std::cout << kTag << " [调试] 请求: " << req.str().substr(0, 150) << "..." << std::endl;
        }

        if (!ws.SendText(req.str())) {
            ws.Close();
            if (audio_callback_) audio_callback_(std::vector<int16_t>(8000, 0));
            return false;
        }

        std::vector<int16_t> all_pcm;
        while (true) {
            std::string resp;
            if (!ws.RecvText(resp)) break;

            if (debug_mode_) {
                std::cout << kTag << " [调试] TTS 响应: " << resp.substr(0, 250) << std::endl;
            }

            /* 检查错误（新版错误在 header.code） */
            int header_code = ExtractJsonInt(resp, "code", 0);
            if (header_code != 0) {
                std::string err_msg = ExtractJsonStr(resp, "message");
                std::cerr << kTag << " API 错误: code=" << header_code
                          << " msg=" << err_msg << std::endl;
                break;
            }

            /* 新版格式：audio 在 payload.audio.audio，status 在 payload.audio.status */
            std::string audio_b64 = ExtractJsonStr(resp, "audio");
            int status = ExtractJsonInt(resp, "status", 0);

            if (!audio_b64.empty()) {
                auto raw = Base64Decode(audio_b64);
                size_t samples = raw.size() / 2;
                std::vector<int16_t> pcm(samples);
                for (size_t i = 0; i < samples && i * 2 + 1 < raw.size(); i++) {
                    pcm[i] = static_cast<int16_t>(
                        static_cast<uint16_t>(raw[i*2]) |
                        (static_cast<uint16_t>(raw[i*2+1]) << 8));
                }
                all_pcm.insert(all_pcm.end(), pcm.begin(), pcm.end());
            }
            if (status == 2) break;
        }
        ws.Close();

        if (all_pcm.empty()) {
            if (audio_callback_) audio_callback_(std::vector<int16_t>(8000, 0));
            return false;
        }

        /* 单声道转立体声（喇叭接右声道，单声道只会出左声道无声） */
        if (!all_pcm.empty()) {
            std::vector<int16_t> stereo;
            stereo.reserve(all_pcm.size() * 2);
            for (size_t i = 0; i < all_pcm.size(); i++) {
                stereo.push_back(all_pcm[i]);  /* 左声道 */
                stereo.push_back(all_pcm[i]);  /* 右声道（喇叭所在） */
            }
            if (debug_mode_) {
                std::cout << kTag << " [调试] 单声道→立体声: "
                          << all_pcm.size() << " → " << stereo.size() << " samples" << std::endl;
            }
            all_pcm.swap(stereo);
        }

        /* 诊断 PCM 数据是否有效 */
        if (!all_pcm.empty()) {
            int16_t pcm_min = all_pcm[0], pcm_max = all_pcm[0];
            double pcm_rms = 0;
            for (size_t i = 0; i < all_pcm.size(); i++) {
                if (all_pcm[i] < pcm_min) pcm_min = all_pcm[i];
                if (all_pcm[i] > pcm_max) pcm_max = all_pcm[i];
                pcm_rms += static_cast<double>(all_pcm[i]) * all_pcm[i];
            }
            pcm_rms = std::sqrt(pcm_rms / all_pcm.size());
            std::cout << kTag << " 合成完成: " << all_pcm.size()
                      << " samples (" << (all_pcm.size() / 16000.0) << "s)"
                      << " min=" << pcm_min << " max=" << pcm_max
                      << " rms=" << static_cast<int>(pcm_rms) << std::endl;
        }

        if (audio_callback_) audio_callback_(all_pcm);
        return true;
    }

    void SetDebugMode(bool enable) override { debug_mode_ = enable; }

private:
    std::string app_id_, api_key_, api_secret_, tts_url_, voice_, auth_token_;
    bool debug_mode_ = false;
};

std::unique_ptr<TTSClient> CreateTTSClient() {
    return std::make_unique<TTSClientWS>();
}

#endif /* ENABLE_SPARKCHAIN_SDK */
