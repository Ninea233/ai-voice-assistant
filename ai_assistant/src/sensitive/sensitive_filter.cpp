/*
 * sensitive_filter.cpp
 * 敏感词过滤器实现（Aho-Corasick 自动机）
 */

#include "assistant/sensitive/sensitive_filter.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <unordered_map>

static const char* kTag = "[SensitiveFilter]";

/* Aho-Corasick Trie 节点 */
struct ACTrieNode {
    std::unordered_map<char, ACTrieNode*> children;
    ACTrieNode* fail = nullptr;      /* 失败指针 */
    std::string keyword;             /* 如果该节点是关键词结尾，记录关键词 */
    SensitiveAction action = SensitiveAction::REPLACE;
    bool is_end = false;
};

SensitiveFilter::SensitiveFilter()
    : root_(std::make_unique<ACTrieNode>()) {}

SensitiveFilter::~SensitiveFilter() = default;

bool SensitiveFilter::LoadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << kTag << " 无法打开敏感词文件: " << path << std::endl;
        return false;
    }

    std::string line;
    int count = 0;
    while (std::getline(file, line)) {
        /* 跳过空行和注释 */
        if (line.empty() || line[0] == '#') continue;

        /* 格式: "关键词:动作" 或 "关键词" */
        SensitiveAction action = default_action_;
        size_t colon = line.find(':');
        std::string word;
        if (colon != std::string::npos) {
            word = line.substr(0, colon);
            std::string action_str = line.substr(colon + 1);
            /* 去除首尾空格 */
            action_str.erase(0, action_str.find_first_not_of(" \t\r"));
            action_str.erase(action_str.find_last_not_of(" \t\r") + 1);

            if (action_str == "silence") action = SensitiveAction::SILENCE;
            else if (action_str == "alert") action = SensitiveAction::ALERT;
            else action = SensitiveAction::REPLACE;
        } else {
            word = line;
        }

        AddWord(word, action);
        count++;
    }

    Build();
    std::cout << kTag << " 已加载 " << count << " 个敏感词: "
              << path << std::endl;
    return true;
}

void SensitiveFilter::AddWord(const std::string& word, SensitiveAction action) {
    ACTrieNode* node = root_.get();
    for (char c : word) {
        if (node->children.find(c) == node->children.end()) {
            node->children[c] = new ACTrieNode();
        }
        node = node->children[c];
    }
    node->is_end = true;
    node->keyword = word;
    node->action = action;
    built_ = false;
}

void SensitiveFilter::Build() {
    std::queue<ACTrieNode*> q;

    /* 第一层节点的失败指针指向根 */
    for (auto it = root_->children.begin(); it != root_->children.end(); ++it) {
        ACTrieNode* child = it->second;
        child->fail = root_.get();
        q.push(child);
    }

    /* BFS 构建失败指针 */
    while (!q.empty()) {
        ACTrieNode* node = q.front();
        q.pop();

        for (auto it = node->children.begin(); it != node->children.end(); ++it) {
            char c = it->first;
            ACTrieNode* child = it->second;
            ACTrieNode* fail = node->fail;
            while (fail && fail->children.find(c) == fail->children.end()) {
                fail = fail->fail;
            }
            child->fail = fail ? fail->children[c] : root_.get();
            q.push(child);
        }
    }
    built_ = true;
}

std::vector<SensitiveMatch> SensitiveFilter::Detect(const std::string& text) const {
    std::vector<SensitiveMatch> matches;
    if (!built_) const_cast<SensitiveFilter*>(this)->Build();

    ACTrieNode* node = root_.get();
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];

        while (node && node->children.find(c) == node->children.end()) {
            node = node->fail;
        }
        node = node ? node->children[c] : root_.get();

        /* 检查匹配 */
        ACTrieNode* temp = node;
        while (temp && temp != root_.get()) {
            if (temp->is_end) {
                matches.push_back({temp->keyword, temp->action, i - temp->keyword.size() + 1});
            }
            temp = temp->fail;
        }
    }

    /* 去重（同一个词可能匹配多次，只保留第一个） */
    // 简单去重：只保留每个关键词的第一次出现
    std::vector<SensitiveMatch> deduped;
    for (const auto& m : matches) {
        bool found = false;
        for (const auto& d : deduped) {
            if (d.keyword == m.keyword) { found = true; break; }
        }
        if (!found) deduped.push_back(m);
    }

    return deduped;
}

std::string SensitiveFilter::Filter(const std::string& text,
                                     const std::string& replacement) const {
    auto matches = Detect(text);
    if (matches.empty()) return text;

    /* 按位置排序（从后往前替换，保持索引不变） */
    std::sort(matches.begin(), matches.end(),
              [](const SensitiveMatch& a, const SensitiveMatch& b) {
                  return a.position < b.position;
              });

    std::string result = text;
    /* 从后往前替换 */
    for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
        if (it->action == SensitiveAction::SILENCE) {
            /* SILENCE 动作直接返回空字符串 */
            return "";
        }
        if (it->action == SensitiveAction::REPLACE) {
            result.replace(it->position, it->keyword.size(), replacement);
        }
        /* ALERT 动作也替换，但上层可以检查 Detects 返回值 */
    }

    return result;
}

bool SensitiveFilter::ContainsSensitive(const std::string& text) const {
    return !Detect(text).empty();
}
