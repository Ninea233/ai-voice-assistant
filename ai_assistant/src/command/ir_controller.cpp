/*
 * ir_controller.cpp
 * 红外控制器（模拟）实现
 */

#include "assistant/command/ir_controller.h"
#include <iostream>
#include <map>

static const char* kTag = "[IRController]";

/* 动作名 → IRCommand 映射表 */
static const std::map<std::string, IRCommand> kActionMap = {
    {"light_on",   {"light", "on",   ""}},
    {"light_off",  {"light", "off",  ""}},
    {"ac_on",      {"ac",    "on",   ""}},
    {"ac_off",     {"ac",    "off",  ""}},
    {"ac_toggle",  {"ac",    "toggle", ""}},
    {"ac_temp_up", {"ac",    "temp_up", "1"}},
    {"ac_temp_down",{"ac",   "temp_down", "1"}},
};

IRController::IRController() = default;

bool IRController::Initialize(bool simulated) {
    simulated_ = simulated;
    std::cout << kTag << " 已初始化 ("
              << (simulated_ ? "模拟模式" : "硬件模式") << ")"
              << std::endl;
    return true;
}

bool IRController::Send(const std::string& action) {
    IRCommand cmd = ParseAction(action);
    return SendCommand(cmd);
}

bool IRController::SendCommand(const IRCommand& cmd) {
    std::cout << kTag
              << " [模拟发送] 设备=" << cmd.device
              << " 动作=" << cmd.action
              << " 参数=" << (cmd.parameter.empty() ? "无" : cmd.parameter)
              << std::endl;

    if (send_callback_) {
        send_callback_(cmd);
    }
    return true;
}

bool IRController::Learn(const std::string& device, const std::string& action) {
    std::cout << kTag << " [模拟学习] 设备=" << device
              << " 动作=" << action << std::endl;
    return true;
}

IRCommand IRController::ParseAction(const std::string& action) const {
    auto it = kActionMap.find(action);
    if (it != kActionMap.end()) {
        return it->second;
    }

    /* 未注册的动作，尝试从名字解析 */
    size_t underscore = action.find('_');
    if (underscore != std::string::npos) {
        return {action.substr(0, underscore),
                action.substr(underscore + 1), ""};
    }
    return {action, "toggle", ""};
}
