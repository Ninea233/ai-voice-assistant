/*
 * net_socket.h
 * TCP/TLS 套接字封装
 *
 * 提供 TCP 连接 + OpenSSL TLS 加密的统一接口。
 * 用于 HTTP 客户端（LLM）和 WebSocket 客户端（ASR/TTS）的底层传输。
 */

#ifndef AI_ASSISTANT_NET_SOCKET_H
#define AI_ASSISTANT_NET_SOCKET_H

#include <cstdint>
#include <string>
#include <vector>

/* TCP 套接字（非加密） */
class TcpSocket {
public:
    TcpSocket();
    virtual ~TcpSocket();

    /* 连接主机:端口，成功返回 true */
    bool Connect(const std::string& host, uint16_t port);

    /* 发送数据（虚函数，派生类 TlsSocket 重写以支持 SSL 加密） */
    virtual bool Send(const uint8_t* data, size_t len);
    virtual bool Send(const std::string& data);

    /* 接收数据（阻塞，返回接收字节数，0=连接关闭，-1=错误） */
    virtual int Recv(uint8_t* buf, size_t max_len);

    /* 接收全部数据直到连接关闭 */
    std::string RecvAll();

    /* 关闭连接 */
    virtual void Close();

    /* 是否已连接 */
    bool IsConnected() const { return fd_ >= 0; }

protected:
    int fd_ = -1;
};

/* TLS 套接字（OpenSSL 加密） */
class TlsSocket : public TcpSocket {
public:
    TlsSocket();
    ~TlsSocket() override;

    /* 建立 TLS 连接（含 TCP 握手 + TLS 握手） */
    bool Connect(const std::string& host, uint16_t port);

    /* TLS 加密发送 */
    bool Send(const uint8_t* data, size_t len) override;
    bool Send(const std::string& data) override;

    /* TLS 解密接收 */
    int Recv(uint8_t* buf, size_t max_len) override;

    /* 关闭 TLS 连接 */
    void Close() override;

private:
    void* ssl_ = nullptr;    /* SSL* */
    void* ctx_ = nullptr;    /* SSL_CTX* */
};

#endif /* AI_ASSISTANT_NET_SOCKET_H */
