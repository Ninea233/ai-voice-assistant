/*
 * http_client.h
 * 简易 HTTP/1.1 客户端（支持 HTTPS）
 *
 * 用于 LLM API 调用（星火 Spark-X2 HTTP 接口）。
 */

#ifndef AI_ASSISTANT_HTTP_CLIENT_H
#define AI_ASSISTANT_HTTP_CLIENT_H

#include <cstdint>
#include <map>
#include <string>

class TlsSocket;

/* HTTP 响应 */
struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    /* POST 请求（完整 URL，如 https://api.example.com/v1/chat） */
    bool Post(const std::string& url,
              const std::string& body,
              const std::map<std::string, std::string>& extra_headers,
              HttpResponse& response);

    /* GET 请求 */
    bool Get(const std::string& url,
             const std::map<std::string, std::string>& extra_headers,
             HttpResponse& response);

private:
    bool ParseUrl(const std::string& url, std::string& scheme,
                  std::string& host, uint16_t& port, std::string& path);

    bool Request(const std::string& method, const std::string& url,
                 const std::string& body,
                 const std::map<std::string, std::string>& extra_headers,
                 HttpResponse& response);

    int ParseResponse(const std::string& raw, HttpResponse& response);
};

#endif /* AI_ASSISTANT_HTTP_CLIENT_H */
