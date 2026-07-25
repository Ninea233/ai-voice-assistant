#!/bin/sh
# mic_in_config.sh
# WM8960 声卡配置（正点原子 i.MX6ULL）
#
# 录音通路: 麦克风 → LINPUT1 → Boost Mixer → ADC → Capture Volume
# 播放通路: DAC → Speaker/Headphone
#
# 注意: 单声道录音默认走左声道（LINPUT1），
#       之前左声道关闭导致录音增益极低。

# ============================================
# 捕获（录音）主音量
# ============================================
amixer cset name='Capture Volume' 255,255
amixer sset 'ADC PCM' 255

# ============================================
# 左声道输入（麦克风主通路 — 重点修复）
# ============================================
# 启用左声道输入 Boost Mixer
amixer sset 'Left Input Mixer Boost' on

# LINPUT1 连接到左 Boost Mixer（板载麦克风）
amixer sset 'Left Boost Mixer LINPUT1' on
# LINPUT1 输入增益（0~127，127 最大增益 = +22.5dB）
amixer sset 'Left Input Boost Mixer LINPUT1' 127

# LINPUT2/LINPUT3 关闭
amixer sset 'Left Boost Mixer LINPUT2' off
amixer sset 'Left Input Boost Mixer LINPUT2' 0
amixer sset 'Left Boost Mixer LINPUT3' off
amixer sset 'Left Input Boost Mixer LINPUT3' 0

# ============================================
# 右声道输入（备用）
# ============================================
amixer sset 'Right Input Mixer Boost' on
amixer sset 'Right Boost Mixer RINPUT2' on
amixer sset 'Right Input Boost Mixer RINPUT2' 127
amixer sset 'Right Boost Mixer RINPUT1' off
amixer sset 'Right Input Boost Mixer RINPUT1' 0
amixer sset 'Right Boost Mixer RINPUT3' off
amixer sset 'Right Input Boost Mixer RINPUT3' 0

# ============================================
# PCM 数字播放
# ============================================
amixer sset 'PCM Playback' on
amixer sset 'Playback' 255          # DAC 数字音量 0-255，255 最大
amixer sset 'Right Output Mixer PCM' on
amixer sset 'Left Output Mixer PCM' on

# ============================================
# 喇叭（扬声器）
# ============================================
amixer sset 'Speaker Playback ZC' on
amixer sset Speaker 120,120         # 喇叭模拟音量 0-127
amixer sset 'Speaker AC' 5          # 喇叭升压 A 类
amixer sset 'Speaker DC' 5          # 喇叭升压 D 类
# ============================================
# 耳机
# ============================================
amixer sset 'Headphone Playback ZC' on
amixer sset Headphone 120,120       # 耳机音量 0-127

echo "mic_in_config: 配置完成"
echo "  录音: LINPUT1 启用 增益最大"
echo "  播放: Speaker 125/127"
