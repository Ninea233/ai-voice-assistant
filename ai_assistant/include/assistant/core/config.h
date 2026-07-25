/*
 * config.h
 * 配置管理器：解析 assistant.conf 的 ini 风格配置
 *
 * 支持 [section]、key=value、# 注释
 * 不支持多行值
 */

#ifndef AI_ASSISTANT_CONFIG_H
#define AI_ASSISTANT_CONFIG_H

#include <map>
#include <string>

class Config {
public:
    Config() = default;
    ~Config() = default;

    /* 从文件加载配置 */
    bool Load(const std::string& path);

    /* 获取字符串值，若不存在返回 default_val */
    std::string Get(const std::string& section,
                    const std::string& key,
                    const std::string& default_val = "") const;

    /* 获取整数值 */
    int GetInt(const std::string& section,
               const std::string& key,
               int default_val = 0) const;

    /* 获取浮点值 */
    float GetFloat(const std::string& section,
                   const std::string& key,
                   float default_val = 0.0f) const;

    /* 获取布尔值 */
    bool GetBool(const std::string& section,
                 const std::string& key,
                 bool default_val = false) const;

    /* 重新加载 */
    bool Reload();

private:
    std::string file_path_;
    /* 存储方式: section_key → value */
    std::map<std::string, std::string> values_;

    std::string MakeKey(const std::string& section,
                        const std::string& key) const;
};

#endif /* AI_ASSISTANT_CONFIG_H */
