/*
 * http_client.h
 * 简易 HTTP/1.1 客户端（支持 HTTPS）
 *
 * 用于 LLM API 调用（星火 Spark-X2 HTTP 接口）。
 */

#ifndef AI_ASSISTANT_HTTP_CLIENT_H
#define AI_ASSISTANT_HTTP_CLIENT_H

#include <cstdint>
#include <functional>
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
    /* 流式响应体回调：每收到一段响应体调用一次，返回 false 表示中止接收 */
    using BodyChunkCallback = std::function<bool(const std::string& chunk)>;

    HttpClient();
    ~HttpClient();

    /* POST 请求（完整 URL，如 https://api.example.com/v1/chat） */
    bool Post(const std::string& url,
              const std::string& body,
              const std::map<std::string, std::string>& extra_headers,
              HttpResponse& response);

    /* 流式 POST：边收边回调，用于 SSE（text/event-stream）。
     * 支持 Transfer-Encoding: chunked 与普通响应体两种形式。
     * 回调返回 false 或收到 chunked 结束块时停止接收。 */
    bool PostStream(const std::string& url,
                    const std::string& body,
                    const std::map<std::string, std::string>& extra_headers,
                    const BodyChunkCallback& on_chunk,
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
