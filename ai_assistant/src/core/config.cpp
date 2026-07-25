/*
 * config.cpp
 * 配置管理器实现
 */

#include "assistant/core/config.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>

static std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r");
    size_t end   = s.find_last_not_of(" \t\r");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

bool Config::Load(const std::string& path) {
    file_path_ = path;
    values_.clear();

    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string line, section;
    while (std::getline(file, line)) {
        line = Trim(line);

        /* 跳过空行和注释 */
        if (line.empty() || line[0] == '#') continue;

        /* 节标题 [section] */
        if (line[0] == '[') {
            size_t close = line.find(']');
            if (close != std::string::npos) {
                section = Trim(line.substr(1, close - 1));
            }
            continue;
        }

        /* key=value */
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;

        values_[MakeKey(section, key)] = val;
    }
    return true;
}

std::string Config::MakeKey(const std::string& section,
                            const std::string& key) const {
    return section + "." + key;
}

std::string Config::Get(const std::string& section,
                        const std::string& key,
                        const std::string& default_val) const {
    auto it = values_.find(MakeKey(section, key));
    return (it != values_.end()) ? it->second : default_val;
}

int Config::GetInt(const std::string& section,
                   const std::string& key,
                   int default_val) const {
    std::string val = Get(section, key, "");
    if (val.empty()) return default_val;
    return std::atoi(val.c_str());
}

float Config::GetFloat(const std::string& section,
                       const std::string& key,
                       float default_val) const {
    std::string val = Get(section, key, "");
    if (val.empty()) return default_val;
    return std::atof(val.c_str());
}

bool Config::GetBool(const std::string& section,
                     const std::string& key,
                     bool default_val) const {
    std::string val = Get(section, key, "");
    if (val.empty()) return default_val;
    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
    return (val == "true" || val == "yes" || val == "1");
}

bool Config::Reload() {
    if (file_path_.empty()) return false;
    return Load(file_path_);
}
