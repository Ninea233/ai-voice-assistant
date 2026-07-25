/*
 * xfyun_auth.cpp
 * 讯飞平台认证实现
 */

#include "assistant/cloud/xfyun_auth.h"

#include <cstring>
#include <iostream>
#include <sstream>

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>

static const char* kTag = "[XfyunAuth]";

/* RFC 1123 日期格式 */
static std::string GetRfc1123Date() {
    time_t now = time(nullptr);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char buf[64];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
    return std::string(buf);
}

/* 标准 Base64 编码 */
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

/* HMAC-SHA256 签名（讯飞 API 要求的标准算法） */
static std::string HmacSha256(const std::string& key, const std::string& data) {
    uint8_t result[SHA256_DIGEST_LENGTH];
    unsigned int result_len = SHA256_DIGEST_LENGTH;

    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const uint8_t*>(data.data()), data.size(),
         result, &result_len);

    return std::string(reinterpret_cast<char*>(result), result_len);
}

namespace xfyun {

std::string Base64UrlEncode(const std::string& input) {
    std::string encoded = Base64Encode(
        reinterpret_cast<const uint8_t*>(input.data()), input.size());

    /* URL safe: + → -, / → _, 去掉末尾 = */
    for (auto& c : encoded) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }

    /* 去掉末尾的 = */
    size_t pos = encoded.find('=');
    if (pos != std::string::npos) {
        encoded = encoded.substr(0, pos);
    }

    return encoded;
}

std::string BuildAuthUrl(const std::string& api_key,
                          const std::string& api_secret,
                          const std::string& ws_url,
                          std::string* out_date) {
    /* 解析 URL */
    std::string url = ws_url;
    std::string scheme_prefix = "wss://";
    size_t proto = url.find("://");
    if (proto == std::string::npos) {
        std::cerr << kTag << " URL 格式错误: " << url << std::endl;
        return url;
    }

    std::string rest = url.substr(proto + 3);
    size_t path_start = rest.find('/');
    std::string host = (path_start == std::string::npos) ? rest : rest.substr(0, path_start);
    std::string path = (path_start == std::string::npos) ? "/" : rest.substr(path_start);

    /* 1. 生成 RFC 1123 日期 */
    std::string date = GetRfc1123Date();
    if (out_date) {
        *out_date = date;
    }

    /* 2. 构建签名原始字符串 */
    std::string signature_origin = "host: " + host + "\n";
    signature_origin += "date: " + date + "\n";
    signature_origin += "GET " + path + " HTTP/1.1";

    /* 3. HMAC-SHA256 签名 */
    std::string signature_raw = HmacSha256(api_secret, signature_origin);

    /* 4. Base64 编码签名 */
    std::string signature = Base64Encode(
        reinterpret_cast<const uint8_t*>(signature_raw.data()), signature_raw.size());

    /* 5. 构建 authorization */
    std::string authorization = "api_key=\"" + api_key + "\", ";
    authorization += "algorithm=\"hmac-sha256\", ";
    authorization += "headers=\"host date request-line\", ";
    authorization += "signature=\"" + signature + "\"";

    /* 6. 标准 base64 编码 authorization（文档要求标准 base64，非 URL-safe） */
    std::string auth_b64 = Base64Encode(
        reinterpret_cast<const uint8_t*>(authorization.data()), authorization.size());

    /* 7. URL Encode 日期 */
    std::string encoded_date;
    for (char c : date) {
        if (c == ' ') encoded_date += "%20";
        else if (c == ',') encoded_date += "%2C";
        else if (c == ':') encoded_date += "%3A";
        else encoded_date += c;
    }

    /* 8. URL Encode authorization（标准 base64 可能含有 +/= 等 URL 特殊字符） */
    std::string encoded_auth;
    for (char c : auth_b64) {
        if (c == '+') encoded_auth += "%2B";
        else if (c == '/') encoded_auth += "%2F";
        else if (c == '=') encoded_auth += "%3D";
        else encoded_auth += c;
    }

    /* 9. 组装最终 URL */
    std::string auth_url = scheme_prefix + host + path;
    auth_url += "?authorization=" + encoded_auth;
    auth_url += "&date=" + encoded_date;
    auth_url += "&host=" + host;

    if (false) {  /* debug 不打印凭据 */
        std::cout << kTag << " 认证 URL 已生成" << std::endl;
    }

    return auth_url;
}

}  // namespace xfyun
