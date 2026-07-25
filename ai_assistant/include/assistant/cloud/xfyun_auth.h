/*
 * xfyun_auth.h
 * 讯飞平台认证辅助函数
 *
 * 提供：
 * 1. 标准 WebSocket 认证 URL 生成（HMAC-SHA1 + Base64）
 * 2. TTS 令牌认证
 */

#ifndef AI_ASSISTANT_XFYUN_AUTH_H
#define AI_ASSISTANT_XFYUN_AUTH_H

#include <string>

namespace xfyun {

/* 生成 WebSocket 认证 URL（标准 HMAC-SHA256 方式）
 * 用于 ASR (iat-api.xfyun.cn) 等 WebSocket 接口
 * 参数：
 *   api_key    - 讯飞 API Key
 *   api_secret - 讯飞 API Secret
 *   ws_url     - 原始 WebSocket URL，如 "wss://iat-api.xfyun.cn/v2/iat"
 *   out_date   - [可选] 输出参数，返回签名用的 RFC 1123 日期字符串，
 *                调用者需将其设为 WebSocket 握手请求的 Date 头
 * 返回：
 *   带认证参数的完整 URL
 */
std::string BuildAuthUrl(const std::string& api_key,
                          const std::string& api_secret,
                          const std::string& ws_url,
                          std::string* out_date = nullptr);

/* Base64 URL Safe 编码（替换 +/= 为 -/_/） */
std::string Base64UrlEncode(const std::string& input);

}  // namespace xfyun

#endif /* AI_ASSISTANT_XFYUN_AUTH_H */
