/*
 * ws_client.cpp
 * WebSocket (RFC 6455) 客户端实现
 */

#include "assistant/cloud/ws_client.h"
#include "assistant/cloud/net_socket.h"

#include <cstring>
#include <iostream>
#include <sstream>
#include <random>
#include <vector>

#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

static const char* kTag = "[WebSocket]";

/* WebSocket GUID (RFC 6455) */
static const char* kWSGUID = "258EAFA5-E914-47DA-95CA-5AB5A32670DA";

/* Base64 编码（OpenSSL BIO 方式） */
static std::string Base64Encode(const uint8_t* data, size_t len) {
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

WebSocketClient::WebSocketClient() {}

WebSocketClient::~WebSocketClient() { Close(); }

void WebSocketClient::SetHeader(const std::string& name, const std::string& value) {
    custom_headers_[name] = value;
}

bool WebSocketClient::ParseUrl(const std::string& url, std::string& scheme,
                                std::string& host, uint16_t& port,
                                std::string& path) {
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;

    scheme = url.substr(0, scheme_end);
    size_t host_start = scheme_end + 3;
    size_t path_start = url.find('/', host_start);

    std::string host_port;
    if (path_start == std::string::npos) {
        host_port = url.substr(host_start);
        path = "/";
    } else {
        host_port = url.substr(host_start, path_start - host_start);
        path = url.substr(path_start);
    }

    size_t colon = host_port.find(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        port = static_cast<uint16_t>(std::stoi(host_port.substr(colon + 1)));
    } else {
        host = host_port;
        port = (scheme == "wss") ? 443 : 80;
    }
    return true;
}

std::string WebSocketClient::GenerateKey() {
    uint8_t random[16];
    RAND_bytes(random, 16);
    return Base64Encode(random, 16);
}

std::string WebSocketClient::ComputeAccept(const std::string& key) {
    std::string concat = key + kWSGUID;
    uint8_t sha1_hash[20];  /* SHA-1 输出 20 字节 */
    unsigned int hash_len = 0;

    /* 使用 EVP 接口代替直接 SHA1()，避免 ARM 平台上 SHA1 汇编实现兼容性问题 */
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx) {
        EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
        EVP_DigestUpdate(ctx, concat.data(), concat.size());
        EVP_DigestFinal_ex(ctx, sha1_hash, &hash_len);
        EVP_MD_CTX_free(ctx);
    }

    return Base64Encode(sha1_hash, hash_len > 0 ? hash_len : sizeof(sha1_hash));
}

bool WebSocketClient::Connect(const std::string& url) {
    std::string scheme, host, path;
    uint16_t port = 0;
    if (!ParseUrl(url, scheme, host, port, path)) {
        std::cerr << kTag << " URL 解析失败: " << url << std::endl;
        return false;
    }

    /* 连接 TCP/TLS */
    sock_ = new TlsSocket();
    if (!sock_->Connect(host, port)) {
        std::cerr << kTag << " 连接失败: " << host << ":" << port << std::endl;
        delete sock_;
        sock_ = nullptr;
        return false;
    }

    /* 生成 WebSocket 握手 key */
    std::string ws_key = GenerateKey();

    /* 发送 HTTP Upgrade 请求 */
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n";
    req << "Host: " << host;
    if (port != 443 && port != 80) req << ":" << port;
    req << "\r\n";
    req << "Upgrade: websocket\r\n";
    req << "Connection: Upgrade\r\n";
    req << "Sec-WebSocket-Key: " << ws_key << "\r\n";
    req << "Sec-WebSocket-Version: 13\r\n";

    /* 自定义请求头（如 x-api-key / Date） */
    for (const auto& h : custom_headers_) {
        req << h.first << ": " << h.second << "\r\n";
    }

    req << "\r\n";

    std::string req_str = req.str();
    /* 调试输出完整握手请求（始终输出，不依赖回调） */
    if (debug_mode_) {
        std::string msg = "[WebSocket] >> 握手请求:\n" + req_str;
        if (debug_cb_) debug_cb_(msg);
        else std::cout << kTag << " >> 握手请求" << std::endl;
    }

    if (!sock_->Send(req_str)) {
        std::cerr << kTag << " 发送握手失败" << std::endl;
        Close();
        return false;
    }

    /* 读取响应（最多 4096 字节） */
    uint8_t buf[4096];
    int n = sock_->Recv(buf, sizeof(buf) - 1);
    if (n <= 0) {
        std::cerr << kTag << " 握手无响应" << std::endl;
        Close();
        return false;
    }
    buf[n] = '\0';
    std::string resp(reinterpret_cast<char*>(buf));

    if (debug_mode_) {
        std::string msg = "[WebSocket] << 握手响应:\n" + resp;
        if (debug_cb_) debug_cb_(msg);
        else std::cout << kTag << " << 握手响应 (" << n << "B)" << std::endl;
    }

    /* 验证响应 */
    if (resp.find("HTTP/1.1 101") == std::string::npos &&
        resp.find("HTTP/1.0 101") == std::string::npos &&
        resp.find("101") == std::string::npos) {
        std::cerr << kTag << " 握手失败(" << n << "B): "
                  << resp.substr(0, 200) << std::endl;
        Close();
        return false;
    }



    connected_ = true;
    if (debug_mode_) {
        std::string msg = "[WebSocket] 连接成功: " + url;
        if (debug_cb_) debug_cb_(msg);
        else std::cout << kTag << " 连接成功: " << url << std::endl;
    }
    return true;
}

void WebSocketClient::ApplyMask(uint8_t* data, size_t len, const uint8_t mask[4]) {
    for (size_t i = 0; i < len; i++) {
        data[i] ^= mask[i % 4];
    }
}

bool WebSocketClient::SendFrame(WsOpcode opcode, const uint8_t* data, size_t len) {
    if (!sock_ || !connected_) return false;

    std::vector<uint8_t> frame;
    /* FIN + opcode */
    frame.push_back(static_cast<uint8_t>(0x80 | static_cast<uint8_t>(opcode)));

    /* Mask 位 = 1，长度编码 */
    uint8_t mask_key[4];
    RAND_bytes(mask_key, 4);

    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(0x80 | len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<uint8_t>(0x80 | 126));
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(static_cast<uint8_t>(0x80 | 127));
        for (int i = 7; i >= 0; i--) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }

    /* Mask 密钥 */
    for (int i = 0; i < 4; i++) {
        frame.push_back(mask_key[i]);
    }

    /* 掩码处理 payload */
    size_t payload_offset = frame.size();
    frame.resize(frame.size() + len);
    if (len > 0) {
        std::memcpy(frame.data() + payload_offset, data, len);
        ApplyMask(frame.data() + payload_offset, len, mask_key);
    }

    return sock_->Send(frame.data(), frame.size());
}

bool WebSocketClient::SendText(const std::string& text) {
    if (debug_mode_) {
        std::string msg = "[WebSocket] >> 文本(" + std::to_string(text.size()) + "B): " +
                          text.substr(0, 100);
        if (debug_cb_) debug_cb_(msg);
    }
    return SendFrame(WsOpcode::TEXT,
                     reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool WebSocketClient::SendBinary(const uint8_t* data, size_t len) {
    if (debug_mode_) {
        std::string msg = "[WebSocket] >> 二进制(" + std::to_string(len) + "B)";
        if (debug_cb_) debug_cb_(msg);
    }
    return SendFrame(WsOpcode::BINARY, data, len);
}

bool WebSocketClient::RecvFrame(WsOpcode& opcode, std::vector<uint8_t>& data) {
    if (!sock_ || !connected_) return false;

    data.clear();

    /* 读取前 2 字节（基本帧头） */
    uint8_t header[2];
    int recv_ret = sock_->Recv(header, 2);
    if (recv_ret != 2) {
        if (debug_mode_) {
            std::string em = "[WebSocket] 读取帧头失败: ret=" + std::to_string(recv_ret);
            if (debug_cb_) debug_cb_(em);
            else std::cout << kTag << " " << em << std::endl;
        }
        connected_ = false;
        return false;
    }

    bool fin = (header[0] & 0x80) != 0;
    (void)fin;  /* 用于 continuation 帧拼接 */
    opcode = static_cast<WsOpcode>(header[0] & 0x0F);
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    /* 扩展长度 */
    if (payload_len == 126) {
        uint8_t ext[2];
        if (sock_->Recv(ext, 2) != 2) { connected_ = false; return false; }
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (sock_->Recv(ext, 8) != 8) { connected_ = false; return false; }
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | ext[i];
        }
    }

    /* Mask 密钥（服务器→客户端通常不掩码，但标准允许） */
    uint8_t mask_key[4] = {0};
    if (masked) {
        if (sock_->Recv(mask_key, 4) != 4) { connected_ = false; return false; }
    }

    /* 读取 payload */
    data.resize(static_cast<size_t>(payload_len));
    if (payload_len > 0) {
        size_t remaining = static_cast<size_t>(payload_len);
        uint8_t* ptr = data.data();
        while (remaining > 0) {
            int n = sock_->Recv(ptr, remaining);
            if (n <= 0) { connected_ = false; return false; }
            ptr += n;
            remaining -= n;
        }
        if (masked) {
            ApplyMask(data.data(), data.size(), mask_key);
        }
    }

    /* 处理 PING/PONG/CLOSE 控制帧 */
    if (opcode == WsOpcode::PING) {
        if (debug_mode_) {
            if (debug_cb_) debug_cb_("[WebSocket] << PING");
        }
        /* 回复 PONG */
        SendFrame(WsOpcode::PONG, data.data(), data.size());
        /* 递归读取下一帧 */
        return RecvFrame(opcode, data);
    }

    if (opcode == WsOpcode::CLOSE) {
        if (debug_mode_) {
            if (debug_cb_) debug_cb_("[WebSocket] << CLOSE");
        }
        connected_ = false;
        return false;
    }

    return true;
}

bool WebSocketClient::RecvText(std::string& text) {
    text.clear();

    WsOpcode opcode;
    std::vector<uint8_t> data;

    while (true) {
        if (!RecvFrame(opcode, data)) {
            return false;
        }

        if (opcode == WsOpcode::TEXT || opcode == WsOpcode::CONTINUATION) {
            text.append(reinterpret_cast<const char*>(data.data()), data.size());
        }

        /* FIN=1 表示消息结束（无后续 continuation 帧） */
        /* 我们通过 opcode 判断：TEXT 帧的 FIN=1 就是完整消息 */
        break;
    }

    return true;
}

std::string WebSocketClient::RecvAllText() {
    std::string result;
    while (true) {
        std::string text;
        if (!RecvText(text)) break;
        result += text;
    }
    return result;
}

bool WebSocketClient::SendPong() {
    return SendFrame(WsOpcode::PONG, nullptr, 0);
}

bool WebSocketClient::SendClose() {
    connected_ = false;
    return SendFrame(WsOpcode::CLOSE, nullptr, 0);
}

void WebSocketClient::Close() {
    if (connected_) {
        SendClose();
    }
    connected_ = false;
    if (sock_) {
        sock_->Close();
        delete sock_;
        sock_ = nullptr;
    }
}

bool WebSocketClient::IsConnected() const {
    return connected_ && sock_ && sock_->IsConnected();
}
