/*
 * http_client.cpp
 * 简易 HTTP/1.1 客户端实现
 */

#include "assistant/cloud/http_client.h"
#include "assistant/cloud/net_socket.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>

#include <openssl/ssl.h>

static const char* kTag = "[HttpClient]";

HttpClient::HttpClient() {}
HttpClient::~HttpClient() {}

bool HttpClient::ParseUrl(const std::string& url, std::string& scheme,
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
        port = (scheme == "https") ? 443 : 80;
    }
    return true;
}

/* 发送 HTTP 请求并接收响应，通过 sock 参数传入已连接的 socket */
static bool SendHttpRequest(TcpSocket& sock, const std::string& method,
                             const std::string& host, const std::string& path,
                             const std::string& body,
                             const std::map<std::string, std::string>& extra_headers,
                             std::string& raw_response) {
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";

    for (auto& h : extra_headers) {
        req << h.first << ": " << h.second << "\r\n";
    }

    if (!body.empty()) {
        req << "Content-Length: " << body.size() << "\r\n";
    }

    req << "Connection: close\r\n";
    req << "\r\n";
    req << body;

    if (!sock.Send(req.str())) {
        std::cerr << kTag << " 发送请求失败" << std::endl;
        return false;
    }

    uint8_t buf[4096];
    while (true) {
        int n = sock.Recv(buf, sizeof(buf));
        if (n <= 0) break;
        raw_response.append(reinterpret_cast<char*>(buf), n);
    }

    return !raw_response.empty();
}

/* ========== 流式响应（SSE） ========== */

namespace {

/* 带缓冲的套接字读取器：在缓冲区内提供「读一行 / 读 N 字节 / 增量填充」 */
class SocketReader {
public:
    explicit SocketReader(TcpSocket& sock) : sock_(sock) {}

    /* 填充缓冲；连接关闭或出错返回 false */
    bool Fill() {
        uint8_t buf[4096];
        int n = sock_.Recv(buf, sizeof(buf));
        if (n <= 0) return false;
        buffer_.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
        return true;
    }

    /* 读取一行（含 '\n'）；无更多数据时返回 false */
    bool ReadLine(std::string& line) {
        size_t pos;
        while ((pos = buffer_.find('\n')) == std::string::npos) {
            if (!Fill()) {
                if (buffer_.empty()) return false;
                line.swap(buffer_);
                return true;
            }
        }
        line = buffer_.substr(0, pos + 1);
        buffer_.erase(0, pos + 1);
        return true;
    }

    /* 读取恰好 n 字节；数据不足返回 false */
    bool ReadN(size_t n, std::string& out) {
        while (buffer_.size() < n) {
            if (!Fill()) return false;
        }
        out = buffer_.substr(0, n);
        buffer_.erase(0, n);
        return true;
    }

    /* 取出并清空当前缓冲的全部数据 */
    std::string TakeBuffer() {
        std::string out;
        out.swap(buffer_);
        return out;
    }

private:
    TcpSocket& sock_;
    std::string buffer_;
};

/* 去掉首尾空白 */
std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/* 头部值是否包含指定子串（忽略大小写） */
bool HeaderContains(const std::string& value, const std::string& token) {
    std::string v = value, t = token;
    for (size_t i = 0; i < v.size(); i++) v[i] = static_cast<char>(::tolower(v[i]));
    for (size_t i = 0; i < t.size(); i++) t[i] = static_cast<char>(::tolower(t[i]));
    return v.find(t) != std::string::npos;
}

} /* namespace */

bool HttpClient::PostStream(const std::string& url,
                            const std::string& body,
                            const std::map<std::string, std::string>& extra_headers,
                            const BodyChunkCallback& on_chunk,
                            HttpResponse& response) {
    std::string scheme, host, path;
    uint16_t port = 0;
    if (!ParseUrl(url, scheme, host, port, path)) {
        std::cerr << kTag << " URL 解析失败: " << url << std::endl;
        return false;
    }

    std::unique_ptr<TcpSocket> sock;
    if (scheme == "https") {
        std::unique_ptr<TlsSocket> tls(new TlsSocket());
        if (!tls->Connect(host, port)) {
            std::cerr << kTag << " TLS 连接失败: " << host << ":" << port << std::endl;
            return false;
        }
        sock.reset(tls.release());
    } else {
        std::unique_ptr<TcpSocket> tcp(new TcpSocket());
        if (!tcp->Connect(host, port)) {
            std::cerr << kTag << " TCP 连接失败: " << host << ":" << port << std::endl;
            return false;
        }
        sock.reset(tcp.release());
    }

    /* 组装请求（流式接口通常要求 Accept: text/event-stream） */
    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";
    bool has_accept = false;
    for (std::map<std::string, std::string>::const_iterator it = extra_headers.begin();
         it != extra_headers.end(); ++it) {
        req << it->first << ": " << it->second << "\r\n";
        if (HeaderContains(it->first, "accept")) has_accept = true;
    }
    if (!has_accept) req << "Accept: text/event-stream\r\n";
    if (!body.empty()) req << "Content-Length: " << body.size() << "\r\n";
    req << "Connection: close\r\n\r\n";
    req << body;

    if (!sock->Send(req.str())) {
        std::cerr << kTag << " 发送流式请求失败" << std::endl;
        return false;
    }

    SocketReader reader(*sock);

    /* 状态行 */
    std::string status_line;
    if (!reader.ReadLine(status_line)) {
        std::cerr << kTag << " 流式响应无状态行" << std::endl;
        return false;
    }
    size_t code_start = status_line.find(' ');
    size_t code_end = (code_start == std::string::npos)
                          ? std::string::npos
                          : status_line.find(' ', code_start + 1);
    if (code_start != std::string::npos && code_end != std::string::npos) {
        response.status_code = std::atoi(
            status_line.substr(code_start + 1, code_end - code_start - 1).c_str());
    }

    /* 响应头 */
    while (true) {
        std::string line;
        if (!reader.ReadLine(line)) break;
        std::string trimmed = Trim(line);
        if (trimmed.empty()) break; /* 空行 = 头部结束 */
        size_t colon = trimmed.find(':');
        if (colon != std::string::npos) {
            response.headers[trimmed.substr(0, colon)] = Trim(trimmed.substr(colon + 1));
        }
    }

    /* 响应体：非 200 直接读全（供调用方提取错误信息） */
    if (response.status_code != 200) {
        while (true) {
            std::string chunk = reader.TakeBuffer();
            if (!chunk.empty()) response.body += chunk;
            if (!reader.Fill()) break;
        }
        sock->Close();
        return true;
    }

    std::string transfer_encoding;
    std::map<std::string, std::string>::const_iterator te = response.headers.find("Transfer-Encoding");
    if (te != response.headers.end()) transfer_encoding = te->second;

    if (HeaderContains(transfer_encoding, "chunked")) {
        while (true) {
            std::string size_line;
            if (!reader.ReadLine(size_line)) break;
            /* chunk 头可带扩展（"1a;ext"），取分号前的十六进制长度 */
            size_t semi = size_line.find(';');
            std::string hex = Trim(semi == std::string::npos ? size_line
                                                             : size_line.substr(0, semi));
            if (hex.empty()) continue;
            size_t chunk_size = static_cast<size_t>(std::strtoul(hex.c_str(), nullptr, 16));
            if (chunk_size == 0) break; /* 结束块 */

            std::string chunk;
            if (!reader.ReadN(chunk_size, chunk)) break;
            std::string crlf;
            reader.ReadLine(crlf); /* 消费 chunk 尾部的 CRLF */

            response.body += chunk;
            if (on_chunk && !on_chunk(chunk)) break;
        }
    } else {
        while (true) {
            std::string chunk = reader.TakeBuffer();
            if (!chunk.empty()) {
                response.body += chunk;
                if (on_chunk && !on_chunk(chunk)) break;
            }
            if (!reader.Fill()) break;
        }
    }

    sock->Close();
    return true;
}

bool HttpClient::Get(const std::string& url,
                     const std::map<std::string, std::string>& extra_headers,
                     HttpResponse& response) {
    return Request("GET", url, "", extra_headers, response);
}

bool HttpClient::Post(const std::string& url,
                      const std::string& body,
                      const std::map<std::string, std::string>& extra_headers,
                      HttpResponse& response) {
    return Request("POST", url, body, extra_headers, response);
}

bool HttpClient::Request(const std::string& method, const std::string& url,
                          const std::string& body,
                          const std::map<std::string, std::string>& extra_headers,
                          HttpResponse& response) {
    std::string scheme, host, path;
    uint16_t port = 0;
    if (!ParseUrl(url, scheme, host, port, path)) {
        std::cerr << kTag << " URL 解析失败: " << url << std::endl;
        return false;
    }

    bool use_tls = (scheme == "https");
    std::string raw_response;

    if (use_tls) {
        TlsSocket sock;
        if (!sock.Connect(host, port)) {
            std::cerr << kTag << " TLS 连接失败: " << host << ":" << port << std::endl;
            return false;
        }
        if (!SendHttpRequest(sock, method, host, path, body, extra_headers, raw_response)) {
            return false;
        }
    } else {
        TcpSocket sock;
        if (!sock.Connect(host, port)) {
            std::cerr << kTag << " TCP 连接失败: " << host << ":" << port << std::endl;
            return false;
        }
        if (!SendHttpRequest(sock, method, host, path, body, extra_headers, raw_response)) {
            return false;
        }
    }

    if (raw_response.empty()) {
        std::cerr << kTag << " 无响应" << std::endl;
        return false;
    }

    /* 解析响应 */
    size_t line_end = raw_response.find("\r\n");
    if (line_end == std::string::npos) {
        std::cerr << kTag << " 响应格式错误" << std::endl;
        return false;
    }

    std::string status_line = raw_response.substr(0, line_end);
    size_t code_start = status_line.find(' ');
    size_t code_end = status_line.find(' ', code_start + 1);
    if (code_start != std::string::npos && code_end != std::string::npos) {
        response.status_code = std::stoi(status_line.substr(code_start + 1, code_end - code_start - 1));
    }

    /* 解析头部 */
    size_t pos = line_end + 2;
    while (true) {
        size_t header_end = raw_response.find("\r\n", pos);
        if (header_end == std::string::npos) break;
        if (header_end == pos) {
            pos = header_end + 2;
            break;
        }
        std::string header_line = raw_response.substr(pos, header_end - pos);
        size_t colon = header_line.find(':');
        if (colon != std::string::npos) {
            std::string key = header_line.substr(0, colon);
            std::string val = header_line.substr(colon + 2);
            response.headers[key] = val;
        }
        pos = header_end + 2;
    }

    response.body = raw_response.substr(pos);
    return true;
}
