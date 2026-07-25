/*
 * http_client.cpp
 * 简易 HTTP/1.1 客户端实现
 */

#include "assistant/cloud/http_client.h"
#include "assistant/cloud/net_socket.h"

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
