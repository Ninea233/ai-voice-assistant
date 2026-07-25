#!/bin/sh
#===========================================
# mic_config.sh
# WM8960 麦克风 ALSA 配置脚本
# 参考 rootfs/music/mic_in_config.sh
#===========================================

# 设置麦克风输入增益
amixer cset name='Capture Volume' 63,63
amixer cset name='Capture Switch' on

# 设置 ADC 输入源为 Mic
amixer cset name='Left Input Mixer LINPUT2 Switch' on
amixer cset name='Right Input Mixer RINPUT1 Switch' on
amixer cset name='Right Input Mixer RINPUT2 Switch' on

# 设置 ADC 增益
amixer cset name='ADC PCM Volume' 255

# 设置麦克风增强
amixer cset name='Mono Input Mixer Mic Boost' on

# 关闭喇叭输出（避免录制时的回响）
amixer cset name='Speaker Playback Volume' 0

# 保存 ALSA 状态
alsactl store

echo "麦克风配置完成!"
