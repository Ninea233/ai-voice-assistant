/*
 * command_manager.cpp
 * 键值对指令集管理器实现
 */

#include "assistant/command/command_manager.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static const char* kTag = "[CommandManager]";

/* 简单的 JSON key-value 解析器（不依赖第三方库）。
 * 支持格式: { "key": "value", "key2": "value2" }
 * 规范 JSON 时才有效，需注意引号转义。 */
static bool ParseSimpleJSON(const std::string& content, CommandMap& out) {
    out.clear();
    std::string::size_type pos = 0;

    /* 跳过 { */
    pos = content.find('{');
    if (pos == std::string::npos) return false;
    pos++;

    while (true) {
        /* 查找 key 的开始引号 */
        pos = content.find('"', pos);
        if (pos == std::string::npos) break;
        pos++;

        /* 读取 key */
        std::string::size_type end = content.find('"', pos);
        if (end == std::string::npos) break;
        std::string key = content.substr(pos, end - pos);
        pos = end + 1;

        /* 查找 : */
        pos = content.find(':', pos);
        if (pos == std::string::npos) break;
        pos++;

        /* 跳过空白 */
        pos = content.find_first_not_of(" \t\r\n", pos);
        if (pos == std::string::npos) break;

        /* 读取 value */
        if (content[pos] == '"') {
            pos++;
            end = content.find('"', pos);
            if (end == std::string::npos) break;
            std::string val = content.substr(pos, end - pos);
            out[key] = val;
            pos = end + 1;
        } else {
            /* 数字或布尔值，读到逗号或 } */
            end = content.find_first_of(",}\n", pos);
            if (end == std::string::npos) break;
            std::string val = content.substr(pos, end - pos);
            /* 去掉首尾空白 */
            val.erase(0, val.find_first_not_of(" \t\r"));
            val.erase(val.find_last_not_of(" \t\r") + 1);
            out[key] = val;
            pos = end;
        }
    }
    return !out.empty();
}

bool CommandManager::LoadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << kTag << " 无法打开指令文件: " << path << std::endl;
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    if (!ParseSimpleJSON(content, commands_)) {
        std::cerr << kTag << " 解析指令文件失败: " << path << std::endl;
        return false;
    }

    std::cout << kTag << " 已加载 " << commands_.size() << " 条指令: "
              << path << std::endl;
    return true;
}

std::string CommandManager::Match(const std::string& text) const {
    std::string best_match;
    size_t best_len = 0;

    for (auto it = commands_.begin(); it != commands_.end(); ++it) {
        const std::string& key = it->first;
        const std::string& value = it->second;
        if (text.find(key) != std::string::npos) {
            /* 最长 key 优先匹配 */
            if (key.length() > best_len) {
                best_match = value;
                best_len = key.length();
            }
        }
    }

    return best_match;
}
