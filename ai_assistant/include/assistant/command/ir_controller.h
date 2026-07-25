/*
 * ir_controller.h
 * 红外控制器（模拟）
 *
 * 当前为模拟实现，所有发送操作仅打印日志。
 * 后续扩展为真实红外发射时，可使用 LIRC 或 GPIO bit-banging。
 *
 * commands.json 中 control 类型的动作值会映射为 IRCommand，
 * 例如 "light_on" → { device: "light", action: "on" }
 */

#ifndef AI_ASSISTANT_IR_CONTROLLER_H
#define AI_ASSISTANT_IR_CONTROLLER_H

#include <functional>
#include <string>

/* 红外命令结构体 */
struct IRCommand {
    std::string device;     /* 设备类型: light, ac, tv ... */
    std::string action;     /* 动作: on, off, temp_up, temp_down ... */
    std::string parameter;  /* 可选参数: 温度值等 */
};

class IRController {
public:
    IRController();
    ~IRController() = default;

    /* 初始化，simulated=true 为模拟模式 */
    bool Initialize(bool simulated = true);

    /* 根据动作名发送命令（如 "light_on"） */
    bool Send(const std::string& action);

    /* 直接发送 IRCommand 结构体 */
    bool SendCommand(const IRCommand& cmd);

    /* 学习红外码（模拟） */
    bool Learn(const std::string& device, const std::string& action);

    /* 发送回调（供外部监听模拟发送事件） */
    using IRSendCallback = std::function<void(const IRCommand&)>;
    void OnIRSend(IRSendCallback cb) { send_callback_ = std::move(cb); }

private:
    /* 将动作名解析为 IRCommand */
    IRCommand ParseAction(const std::string& action) const;

    bool simulated_ = true;
    IRSendCallback send_callback_;
};

#endif /* AI_ASSISTANT_IR_CONTROLLER_H */
