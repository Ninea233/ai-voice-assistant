#!/bin/sh
# time_sync.sh - 时间同步脚本
# 在 AI 助手启动前执行，同步开发板系统时间并设置时区

# 设置系统时区为北京时间（CST, UTC+8）
export TZ='CST-8'
# 写入系统配置（shell 启动时自动设置时区）
echo "export TZ='CST-8'" >> /etc/profile 2>/dev/null  # 登录 shell
echo "export TZ='CST-8'" >> /etc/ashrc 2>/dev/null     # 非登录 shell（BusyBox ash 读 $ENV）
echo 'CST-8' > /etc/TZ 2>/dev/null                     # BusyBox 通用

# 等待网络就绪（延迟1秒）
sleep 1

# 同步一次时间（NTP 方式，-q 为一次性同步）
busybox ntpd -q -p pool.ntp.org

# 手动设置 TZ 确保 date 显示正确（/etc/profile 可能未被 shell 加载）
export TZ='CST-8'

# 显示同步后的时间
echo "时间同步完成"
