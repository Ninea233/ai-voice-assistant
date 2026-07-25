#!/bin/bash
#===========================================
# deploy.sh
# 部署 Agent 语音助手到目标板
#
# 使用方式:
#   ./scripts/deploy.sh <目标IP>
#   例: ./scripts/deploy.sh 192.168.1.100
#
# 依赖: 目标板运行 sshd, 用户 root
#===========================================

set -e

if [ $# -lt 1 ]; then
    echo "用法: $0 <目标板IP>"
    echo "示例: $0 192.168.1.100"
    exit 1
fi

TARGET_IP="$1"
TARGET_USER="${TARGET_USER:-root}"
TARGET_DIR="${TARGET_DIR:-/home/root/ai_assistant}"

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

echo "=========================================="
echo " 部署到目标板: ${TARGET_USER}@${TARGET_IP}"
echo " 目标路径: ${TARGET_DIR}"
echo "=========================================="

# 确保二进制已编译
if [ ! -f "${BUILD_DIR}/ai_assistant" ]; then
    echo "错误: 未找到编译产物，请先运行 cross_compile.sh"
    exit 1
fi

# 创建目标目录
ssh "${TARGET_USER}@${TARGET_IP}" "mkdir -p ${TARGET_DIR}/{config,memory,skills,models}"

# 复制二进制
echo "1/5 复制二进制..."
scp "${BUILD_DIR}/ai_assistant" "${TARGET_USER}@${TARGET_IP}:${TARGET_DIR}/"

# 复制配置
echo "2/5 复制配置..."
scp "${PROJECT_DIR}/config/assistant.conf" \
    "${PROJECT_DIR}/config/agent_prompt.md" \
    "${PROJECT_DIR}/config/actions.json" \
    "${PROJECT_DIR}/config/mcp_tools.json" \
    "${PROJECT_DIR}/config/wakeup.wav" \
    "${TARGET_USER}@${TARGET_IP}:${TARGET_DIR}/config/"

# 复制记忆
echo "3/5 复制记忆..."
scp -r "${PROJECT_DIR}/memory/"* "${TARGET_USER}@${TARGET_IP}:${TARGET_DIR}/memory/"

# 复制 Skill 目录
echo "4/5 复制 Skill..."
scp -r "${PROJECT_DIR}/skills/"* "${TARGET_USER}@${TARGET_IP}:${TARGET_DIR}/skills/"

# 复制模型
if [ -d "${PROJECT_DIR}/models" ] && [ "$(ls -A "${PROJECT_DIR}/models" 2>/dev/null)" ]; then
    scp -r "${PROJECT_DIR}/models/"* "${TARGET_USER}@${TARGET_IP}:${TARGET_DIR}/models/"
fi

# 复制辅助脚本
echo "5/5 复制辅助脚本..."
scp "${PROJECT_DIR}/scripts/mic_in_config.sh" \
    "${PROJECT_DIR}/scripts/time_sync.sh" \
    "${TARGET_USER}@${TARGET_IP}:${TARGET_DIR}/" 2>/dev/null || true

echo ""
echo "=========================================="
echo " 部署完成!"
echo " 在目标板上运行:"
echo "   ssh ${TARGET_USER}@${TARGET_IP}"
echo "   cd ${TARGET_DIR}"
echo "   sh mic_in_config.sh"
echo "   ./ai_assistant -c config/assistant.conf"
echo "=========================================="
