#!/bin/sh
#===========================================
# record_samples.sh
# 在开发板上录制唤醒词训练样本
#
# 使用 ALSA arecord 录制 16kHz/16bit/mono WAV 文件。
# 录制文件直接保存到 NFS 目录，PC 端即刻可用。
#
# 用法:
#   sh record_samples.sh --wakeword    录制唤醒词样本
#   sh record_samples.sh --negative    录制负样本
#   sh record_samples.sh --auto        自动连续录制
#===========================================

set -e

SAMPLE_RATE=16000
BITS=16
CHANNELS=1

DATA_DIR="/ai_assistant/data/wake_word"
POS_DIR="${DATA_DIR}/正样本"
NEG_DIR="${DATA_DIR}/负样本"

# 确保录音目录存在
mkdir -p "${POS_DIR}" "${NEG_DIR}"

# ============================================
# 录音功能
# ============================================
record_one() {
    local label="$1"      # 文件名前缀: sample 或 negative
    local idx="$2"        # 序号
    local duration="$3"   # 录音秒数（整数）
    local output_dir="$4" # 输出目录
    local prompt="$5"     # 提示语

    local output="${output_dir}/${label}_$(date +%Y%m%d_%H%M%S)_${idx}.wav"

    echo ""
    echo "  🎤 [${idx}] ${prompt}"
    echo "  录音 ${duration} 秒..."
    echo "  请准备..."

    # 1 秒倒计时
    sleep 1

    # 录音（注意：EGLIBC 2.19 的 arecord 不支持小数秒）
    arecord -q -r ${SAMPLE_RATE} -f S16_LE -c ${CHANNELS} \
            -d ${duration} "${output}" 2>&1
    local rc=$?
    if [ ${rc} -ne 0 ]; then
        echo "  ❌ arecord 失败 (rc=${rc})，请检查声卡配置"
        return 1
    fi

    # 检查文件是否为空或太小
    local filesize=$(ls -l "${output}" 2>/dev/null | awk '{print $5}')
    if [ -z "${filesize}" ] || [ "${filesize}" -lt 1000 ]; then
        echo "  ⚠️  录音文件过小（${filesize} bytes），已删除，请重试"
        rm -f "${output}"
        return 1
    fi

    echo "  ✅ 保存: $(basename ${output}) (${filesize} bytes)"
    return 0
}

# ============================================
# 录唤醒词（正样本）
# ============================================
record_wakeword() {
    local count=${1:-50}
    local existing=$(ls ${POS_DIR}/*.wav 2>/dev/null | wc -l)
    local to_record=$((count - existing))

    if [ ${to_record} -le 0 ]; then
        echo "✅ 已有 ${existing} 个唤醒词样本，无需录制"
        return 0
    fi

    echo ""
    echo "=========================================="
    echo " 🎤 录制唤醒词样本 (${to_record} 条)"
    echo "=========================================="
    echo " 共需要 ${count} 条，已有 ${existing} 条"
    echo " 每次录 2 秒，间隔 1 秒"
    echo " 请清晰说出「小九小九」"
    echo "=========================================="
    echo ""

    local recorded=0
    while [ ${recorded} -lt ${to_record} ]; do
        local idx=$((existing + recorded + 1))

        echo "--- 第 ${idx}/${count} 条 ---"
        if record_one "sample" "${idx}" "2" "${POS_DIR}" "请说「小九小九」"; then
            recorded=$((recorded + 1))
        fi

        if [ ${recorded} -lt ${to_record} ]; then
            sleep 1
        fi
    done

    echo ""
    echo "✅ 唤醒词录制完成! (共 ${count} 条)"
}

# ============================================
# 录负样本（非唤醒词）
# ============================================
record_negative() {
    local count=${1:-100}
    local existing=$(ls ${NEG_DIR}/*.wav 2>/dev/null | wc -l)
    local to_record=$((count - existing))

    if [ ${to_record} -le 0 ]; then
        echo "✅ 已有 ${existing} 个负样本，无需录制"
        return 0
    fi

    echo ""
    echo "=========================================="
    echo " 🎤 录制负样本 (${to_record} 条)"
    echo "=========================================="
    echo " 共需要 ${count} 条，已有 ${existing} 条"
    echo " 请说其他话（不要包含「小九小九」）"
    echo " 也可以静音录制背景噪音"
    echo "=========================================="
    echo ""

    local recorded=0
    while [ ${recorded} -lt ${to_record} ]; do
        local idx=$((existing + recorded + 1))

        echo "--- 第 ${idx}/${count} 条 ---"
        if record_one "negative" "${idx}" "2" "${NEG_DIR}" "请不要说「小九小九」"; then
            recorded=$((recorded + 1))
        fi

        if [ ${recorded} -lt ${to_record} ]; then
            sleep 1
        fi
    done

    echo ""
    echo "✅ 负样本录制完成! (共 ${count} 条)"
}

# ============================================
# 自动模式（连续录制）
# ============================================
record_auto() {
    echo ""
    echo "=========================================="
    echo " 🎤 自动连续录音模式"
    echo "=========================================="
    echo " 请准备好后按 Enter 开始..."
    echo " 先录唤醒词，再录负样本"
    echo "=========================================="
    read dummy

    record_wakeword 50
    echo ""
    echo "=== 唤醒词完成，休息 3 秒 ==="
    sleep 3
    record_negative 100

    echo ""
    echo "=========================================="
    echo " ✅ 全部录制完成!"
    echo " 正样本: $(ls ${POS_DIR}/*.wav 2>/dev/null | wc -l) 条"
    echo " 负样本: $(ls ${NEG_DIR}/*.wav 2>/dev/null | wc -l) 条"
    echo ""
    echo " 下一步: 在 PC 端训练模型"
    echo "=========================================="
}

# ============================================
# 查看统计
# ============================================
show_status() {
    local pos_count=$(ls ${POS_DIR}/*.wav 2>/dev/null | wc -l)
    local neg_count=$(ls ${NEG_DIR}/*.wav 2>/dev/null | wc -l)

    echo ""
    echo "=========================================="
    echo " 📊 录音数据统计"
    echo "=========================================="
    echo " 唤醒词（小九小九）: ${pos_count} 条"
    echo " 负样本（未知）:     ${neg_count} 条"
    echo " 目录: ${DATA_DIR}"
    echo ""
    echo " 建议: 正样本至少 50 条，负样本至少 100 条"
    echo "=========================================="
}

# ============================================
# 主入口
# ============================================
case "${1:-}" in
    --wakeword|--pos)
        record_wakeword "${2:-50}"
        ;;
    --negative|--neg)
        record_negative "${2:-100}"
        ;;
    --auto|--all)
        record_auto
        ;;
    --status|--stat)
        show_status
        ;;
    *)
        echo "用法: sh record_samples.sh [选项]"
        echo ""
        echo "选项:"
        echo "  --wakeword [N]   录制 N 条唤醒词（默认 50）"
        echo "  --negative [N]   录制 N 条负样本（默认 100）"
        echo "  --auto           自动连续录制（正+负）"
        echo "  --status         查看当前数据统计"
        echo ""
        echo "示例:"
        echo "  sh record_samples.sh --wakeword 50"
        echo "  sh record_samples.sh --negative 100"
        echo "  sh record_samples.sh --auto"
        echo "  sh record_samples.sh --status"
        echo ""
        show_status
        ;;
esac
