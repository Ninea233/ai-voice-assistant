/*
 * ws_client.h
 * WebSocket (RFC 6455) 客户端
 *
 * 支持 WSS（TLS 加密），用于讯飞 ASR/TTS 的 WebSocket 接口。
 * 提供帧级别读写和字符串消息收发。
 */

#ifndef AI_ASSISTANT_WS_CLIENT_H
#define AI_ASSISTANT_WS_CLIENT_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

class TlsSocket;

/* WebSocket 帧 opcode */
enum class WsOpcode : uint8_t {
    CONTINUATION = 0x0,
    TEXT        = 0x1,
    BINARY      = 0x2,
    CLOSE       = 0x8,
    PING        = 0x9,
    PONG        = 0xA,
};

class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    /* 连接 WebSocket 服务器（wss:// 或 ws://） */
    bool Connect(const std::string& url);

    /* 发送文本帧 */
    bool SendText(const std::string& text);

    /* 发送二进制帧 */
    bool SendBinary(const uint8_t* data, size_t len);

    /* 接收一帧（阻塞），返回 opcode，payload 写入 data */
    /* 返回 false 表示连接关闭或出错 */
    bool RecvFrame(WsOpcode& opcode, std::vector<uint8_t>& data);

    /* 接收一个完整文本消息（自动拼接 continuation 帧） */
    bool RecvText(std::string& text);

    /* 发送 Pong 响应 */
    bool SendPong();

    /* 发送 Close 帧 */
    bool SendClose();

    /* 关闭连接 */
    void Close();

    /* 是否已连接 */
    bool IsConnected() const;

    /* 设置自定义请求头（在 WebSocket 握手时发送） */
    void SetHeader(const std::string& name, const std::string& value);

    /* 设置日志回调（debug 模式） */
    void SetDebugCallback(std::function<void(const std::string&)> cb) {
        debug_cb_ = std::move(cb);
    }

    void SetDebugMode(bool on) { debug_mode_ = on; }

    /* 读取所有响应直到结束（返回拼接得到的数据，用于短连接场景） */
    std::string RecvAllText();

private:
    /* 发送原始帧（已掩码） */
    bool SendFrame(WsOpcode opcode, const uint8_t* data, size_t len);

    /* 生成 WebSocket 握手 key */
    std::string GenerateKey();

    /* 计算 accept key */
    std::string ComputeAccept(const std::string& key);

    /* 写入掩码 */
    void ApplyMask(uint8_t* data, size_t len, const uint8_t mask[4]);

    /* 为 URL 生成握手 Host + Path */
    bool ParseUrl(const std::string& url, std::string& scheme,
                  std::string& host, uint16_t& port, std::string& path);

    TlsSocket* sock_ = nullptr;
    bool connected_ = false;
    bool debug_mode_ = false;
    std::map<std::string, std::string> custom_headers_;
    std::function<void(const std::string&)> debug_cb_;
};

#endif /* AI_ASSISTANT_WS_CLIENT_H */
