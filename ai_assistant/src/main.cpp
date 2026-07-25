/*
 * main.cpp
 * AI 语音助手入口
 *
 * 使用方式:
 *   ./ai_assistant -c /path/to/assistant.conf
 *
 * 编译（交叉编译）:
 *   arm-linux-gnueabihf-g++ -std=c++14 ... -lasound -lpthread
 */

#include <csignal>
#include <cstdlib>
#include <iostream>

#include "assistant/core/assistant.h"

static Assistant* g_assistant = nullptr;

/* 信号处理：优雅退出 */
void SignalHandler(int sig) {
    std::cout << std::endl << "[Main] 收到信号 " << sig << "，正在退出..." << std::endl;
    if (g_assistant) {
        g_assistant->Stop();
    }
    exit(0);
}

void PrintUsage(const char* prog) {
    std::cout << "用法: " << prog << " -c <配置文件>" << std::endl;
    std::cout << "示例: " << prog << " -c config/assistant.conf" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string config_path = "config/assistant.conf";

    /* 解析命令行参数 */
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
    }

    std::cout << "========================================" << std::endl;
    std::cout << "   AI 语音智能助手 v1.0" << std::endl;
    std::cout << "   平台: i.MX6ULL (Cortex-A7)" << std::endl;
    std::cout << "   配置: " << config_path << std::endl;
    std::cout << "========================================" << std::endl;

    /* 设置时区为北京时间（CST, UTC+8） */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* 启动前初始化：声卡配置 + 时间同步 */
    std::cout << "[Main] 初始化声卡..." << std::endl;
    int ret = system("./mic_in_config.sh");
    if (ret != 0) {
        std::cerr << "[Main] mic_in_config.sh 执行失败，Audio 可能不可用" << std::endl;
    }

    std::cout << "[Main] 同步系统时间..." << std::endl;
    ret = system("sh ./time_sync.sh");
    if (ret != 0) {
        std::cout << "[Main] 时间同步失败（不影响运行，但 ASR 认证可能因时间偏差出错）" << std::endl;
    }

    /* 注册信号处理 */
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGPIPE, SIG_IGN); /* 忽略 SIGPIPE，防止写断开连接时进程静默退出 */

    /* 创建并启动助手 */
    Assistant assistant;
    g_assistant = &assistant;

    if (!assistant.Initialize(config_path)) {
        std::cerr << "[Main] 初始化失败，退出" << std::endl;
        return 1;
    }

    assistant.Start();
    return 0;
}
