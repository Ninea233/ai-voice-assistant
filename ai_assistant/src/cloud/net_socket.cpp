/*
 * net_socket.cpp
 * TCP/TLS 套接字实现
 */

#include "assistant/cloud/net_socket.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

/* ========== TcpSocket ========== */

TcpSocket::TcpSocket() {}

TcpSocket::~TcpSocket() { Close(); }

bool TcpSocket::Connect(const std::string& host, uint16_t port) {
    struct addrinfo hints, *res = nullptr;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    int ret = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (ret != 0 || !res) {
        std::cerr << "[NetSocket] DNS 解析失败: " << host
                  << " (" << gai_strerror(ret) << ")" << std::endl;
        return false;
    }

    for (struct addrinfo* p = res; p; p = p->ai_next) {
        fd_ = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd_ < 0) continue;

        if (::connect(fd_, p->ai_addr, p->ai_addrlen) == 0) {
            /* 成功连接 */
            freeaddrinfo(res);

            /* 禁用 Nagle 算法 */
            int flag = 1;
            setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            /* 接收超时 60 秒，防止 Recv 永久阻塞（ASR/HTTP 可能处理较慢） */
            struct timeval tv;
            tv.tv_sec = 60;
            tv.tv_usec = 0;
            setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return true;
        }

        ::close(fd_);
        fd_ = -1;
    }

    freeaddrinfo(res);
    std::cerr << "[NetSocket] 连接失败: " << host << ":" << port << std::endl;
    return false;
}

bool TcpSocket::Send(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    while (len > 0) {
        ssize_t n = ::write(fd_, data, len);
        if (n <= 0) return false;
        data += n;
        len -= n;
    }
    return true;
}

bool TcpSocket::Send(const std::string& data) {
    return Send(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

int TcpSocket::Recv(uint8_t* buf, size_t max_len) {
    if (fd_ < 0) return -1;
    ssize_t n = ::read(fd_, buf, max_len);
    return static_cast<int>(n);
}

std::string TcpSocket::RecvAll() {
    std::string result;
    uint8_t buf[4096];
    while (true) {
        int n = Recv(buf, sizeof(buf));
        if (n <= 0) break;
        result.append(reinterpret_cast<char*>(buf), n);
    }
    return result;
}

void TcpSocket::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

/* ========== TlsSocket ========== */

/* OpenSSL 库全局初始化（线程安全，只做一次） */
static bool SslLibraryInit() {
    static bool initialized = false;
    if (!initialized) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        initialized = true;
    }
    return true;
}

TlsSocket::TlsSocket() {}

TlsSocket::~TlsSocket() { Close(); }

bool TlsSocket::Connect(const std::string& host, uint16_t port) {
    SslLibraryInit();

    /* TCP 连接 */
    if (!TcpSocket::Connect(host, port)) {
        return false;
    }

    /* 创建 SSL_CTX */
    ctx_ = SSL_CTX_new(SSLv23_client_method());
    if (!ctx_) {
        std::cerr << "[TlsSocket] SSL_CTX 创建失败" << std::endl;
        TcpSocket::Close();
        return false;
    }

    /* 创建 SSL */
    ssl_ = SSL_new(static_cast<SSL_CTX*>(ctx_));
    if (!ssl_) {
        std::cerr << "[TlsSocket] SSL 创建失败" << std::endl;
        Close();
        return false;
    }

    /* 绑定套接字 */
    SSL_set_fd(static_cast<SSL*>(ssl_), fd_);

    /* SNI（多域名 TLS 必需） */
    SSL_set_tlsext_host_name(static_cast<SSL*>(ssl_), host.c_str());

    /* TLS 握手 */
    int ret = SSL_connect(static_cast<SSL*>(ssl_));
    if (ret != 1) {
        char err_buf[256];
        ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
        std::cerr << "[TlsSocket] TLS 握手失败: " << err_buf << std::endl;
        Close();
        return false;
    }

    return true;
}

bool TlsSocket::Send(const uint8_t* data, size_t len) {
    if (!ssl_) return false;
    while (len > 0) {
        int n = SSL_write(static_cast<SSL*>(ssl_), data, len);
        if (n <= 0) return false;
        data += n;
        len -= n;
    }
    return true;
}

bool TlsSocket::Send(const std::string& data) {
    return Send(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

int TlsSocket::Recv(uint8_t* buf, size_t max_len) {
    if (!ssl_) return -1;
    int n = SSL_read(static_cast<SSL*>(ssl_), buf, max_len);
    return n;
}

void TlsSocket::Close() {
    if (ssl_) {
        SSL_shutdown(static_cast<SSL*>(ssl_));
        SSL_free(static_cast<SSL*>(ssl_));
        ssl_ = nullptr;
    }
    if (ctx_) {
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
        ctx_ = nullptr;
    }
    TcpSocket::Close();
}
