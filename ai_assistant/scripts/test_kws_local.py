#!/usr/bin/env python3
"""
test_kws_local.py
本地测试 KWS 唤醒词模型（PC 麦克风实时测试）

使用方式:
  python3 scripts/test_kws_local.py                # 实时录音测试
  python3 scripts/test_kws_local.py --file 1.wav   # 测试已有 wav 文件
"""

import argparse
import sys
import time
from pathlib import Path

import numpy as np
import tensorflow as tf

# MFCC 参数（与 C++ KWSTFLite 保持一致）
SAMPLE_RATE = 16000
FRAME_LEN = 480       # 30ms
FRAME_SHIFT = 160     # 10ms
FFT_SIZE = 512
NUM_MFCC = 40
NUM_MEL_BINS = 40
NUM_FFT_BINS = FFT_SIZE // 2 + 1
CONTEXT_FRAMES = 10
INPUT_DIM = CONTEXT_FRAMES * NUM_MFCC

MODEL_PATH = Path(__file__).resolve().parent.parent / "models" / "kws_model.tflite"


def load_model(path: str):
    """加载 TFLite 模型"""
    interpreter = tf.lite.Interpreter(model_path=str(path))
    interpreter.allocate_tensors()
    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()
    print(f"  输入: {input_details[0]['shape']}  {input_details[0]['dtype']}")
    print(f"  输出: {output_details[0]['shape']}  {output_details[0]['dtype']}")
    return interpreter, input_details, output_details


def hz_to_mel(hz):
    return 2595.0 * np.log10(1.0 + hz / 700.0)


def mel_to_hz(mel):
    return 700.0 * (10.0 ** (mel / 2595.0) - 1.0)


def precompute_mel_filterbank():
    """预计算 Mel 滤波器组（与 C++ 端一致）"""
    low_mel = hz_to_mel(0.0)
    high_mel = hz_to_mel(SAMPLE_RATE / 2.0)
    mel_points = np.linspace(low_mel, high_mel, NUM_MEL_BINS + 2)
    hz_points = mel_to_hz(mel_points)
    bin_indices = np.round(hz_points * FFT_SIZE / SAMPLE_RATE).astype(int)
    bin_indices = np.clip(bin_indices, 0, NUM_FFT_BINS - 1)

    filterbank = np.zeros((NUM_MEL_BINS, NUM_FFT_BINS), dtype=np.float32)
    for m in range(NUM_MEL_BINS):
        left, center, right = bin_indices[m], bin_indices[m + 1], bin_indices[m + 2]
        # 上升沿
        if center > left:
            filterbank[m, left:center] = np.arange(center - left) / (center - left)
        # 下降沿
        if right > center:
            filterbank[m, center:right + 1] = 1.0 - np.arange(right - center + 1) / (right - center)
    return filterbank


def precompute_dct():
    """预计算 DCT-II 矩阵"""
    dct = np.zeros((NUM_MFCC, NUM_MEL_BINS), dtype=np.float32)
    for i in range(NUM_MFCC):
        scale = np.sqrt(1.0 / NUM_MEL_BINS) if i == 0 else np.sqrt(2.0 / NUM_MEL_BINS)
        dct[i] = scale * np.cos(np.pi * i * (np.arange(NUM_MEL_BINS) + 0.5) / NUM_MEL_BINS)
    return dct


def compute_mfcc(audio: np.ndarray, filterbank: np.ndarray, dct: np.ndarray, hamming: np.ndarray):
    """
    完整 MFCC 提取 pipeline（与 C++ KWSTFLite::ComputeOneMFCC 一致）
    返回 list of MFCC 向量，每帧 40 维
    """
    # 预加重
    preemphasized = np.zeros_like(audio, dtype=np.float32)
    preemphasized[0] = audio[0]
    for i in range(1, len(audio)):
        preemphasized[i] = audio[i] - 0.97 * audio[i - 1]

    # 分帧 + MFCC
    mfcc_frames = []
    pos = 0
    while pos + FRAME_LEN <= len(preemphasized):
        frame = preemphasized[pos:pos + FRAME_LEN]

        # Hamming 窗
        windowed = frame * hamming

        # FFT + 功率谱
        spectrum = np.fft.rfft(windowed, n=FFT_SIZE)
        power = (np.real(spectrum) ** 2 + np.imag(spectrum) ** 2)[:NUM_FFT_BINS]

        # Mel 滤波器组
        mel_energies = filterbank @ power
        mel_energies = np.maximum(mel_energies, 1e-10)

        # Log
        log_energies = np.log(mel_energies)

        # DCT → MFCC
        mfcc = dct @ log_energies
        mfcc_frames.append(mfcc)

        pos += FRAME_SHIFT

    return mfcc_frames


def run_inference(interpreter, input_details, output_details, audio: np.ndarray):
    """处理音频并运行 TFLite 推理"""
    filterbank = precompute_mel_filterbank()
    dct = precompute_dct()
    hamming = np.hamming(FRAME_LEN).astype(np.float32)

    # 提取 MFCC
    mfcc_frames = compute_mfcc(audio, filterbank, dct, hamming)

    if len(mfcc_frames) < CONTEXT_FRAMES:
        print(f"  ⚠️ 音频太短（{len(mfcc_frames)} 帧，需要至少 {CONTEXT_FRAMES} 帧）")
        return []

    scores = []
    for i in range(len(mfcc_frames) - CONTEXT_FRAMES + 1):
        context = np.concatenate(mfcc_frames[i:i + CONTEXT_FRAMES])
        input_data = context.astype(np.float32).reshape(1, INPUT_DIM)

        interpreter.set_tensor(input_details[0]['index'], input_data)
        interpreter.invoke()
        output = interpreter.get_tensor(output_details[0]['index'])
        wakeword_score = output[0][1]
        scores.append(wakeword_score)

    return scores


def test_file(file_path: str):
    """测试已有 WAV 文件"""
    try:
        import soundfile as sf
        audio, sr = sf.read(file_path, dtype='int16')
    except ImportError:
        import librosa
        audio, sr = librosa.load(file_path, sr=SAMPLE_RATE, mono=True)
        audio = (audio * 32767).astype(np.int16)

    if sr != SAMPLE_RATE:
        print(f"  ⚠️ 采样率 {sr}Hz，需重采样到 {SAMPLE_RATE}Hz")
        import librosa
        audio = librosa.resample(audio.astype(float), orig_sr=sr, target_sr=SAMPLE_RATE)
        audio = (audio * 32767).astype(np.int16)

    print(f"  音频时长: {len(audio) / SAMPLE_RATE:.1f}s, 采样率: {SAMPLE_RATE}Hz")

    interpreter, input_details, output_details = load_model(MODEL_PATH)
    scores = run_inference(interpreter, input_details, output_details, audio)

    if scores:
        max_score = max(scores)
        mean_score = np.mean(scores)
        print(f"\n  最大唤醒词分数: {max_score:.4f}")
        print(f"  平均唤醒词分数: {mean_score:.4f}")
        if max_score > 0.5:
            print(f"  {'🔊 检测到唤醒词!' if max_score > 0.8 else '🔉 可能检测到唤醒词'}")
        else:
            print(f"  🔇 未检测到唤醒词")
    print()


def test_mic(duration: float = 3.0):
    """实时录音测试"""
    try:
        import sounddevice as sd
    except ImportError:
        print("错误: 请安装 sounddevice: pip install sounddevice")
        sys.exit(1)

    interpreter, input_details, output_details = load_model(MODEL_PATH)

    print(f"\n🎤 录音 {duration} 秒... 请说话")
    print("  提示: 说「小九小九」来测试唤醒词检测")
    for i in range(3, 0, -1):
        print(f"  {i}...")
        time.sleep(0.5)

    recording = sd.rec(int(duration * SAMPLE_RATE), samplerate=SAMPLE_RATE,
                       channels=1, dtype='int16')
    sd.wait()
    audio = recording.flatten()
    print(f"  ✅ 录音完成 ({len(audio)} 采样点)")

    # 处理
    scores = run_inference(interpreter, input_details, output_details, audio)

    if scores:
        max_score = max(scores)
        # 找出唤醒词分数最高的时间段
        peak_idx = np.argmax(scores)
        peak_time = peak_idx * FRAME_SHIFT / SAMPLE_RATE

        print(f"\n  📊 检测结果:")
        print(f"     最大唤醒词分数: {max_score:.4f} (在 {peak_time:.1f}s 处)")
        print(f"     平均唤醒词分数: {np.mean(scores):.4f}")
        print(f"     触发帧数: {sum(1 for s in scores if s > 0.5)}/{len(scores)}")

        if max_score > 0.8:
            print(f"  {'=' * 40}")
            print(f"  🎉🔊 唤醒词「小九小九」检测成功!")
            print(f"  {'=' * 40}")
        elif max_score > 0.5:
            print(f"  ⚠️  模糊检测 (score={max_score:.2f})，可尝试提高音量")
        else:
            print(f"  🔇 未检测到唤醒词")
    else:
        print("  ❌ 音频太短或无有效音频")


def main():
    parser = argparse.ArgumentParser(description="本地测试 KWS 唤醒词模型")
    parser.add_argument('--file', '-f', type=str, help='测试 WAV 文件')
    parser.add_argument('--duration', '-d', type=float, default=3.0,
                        help='录音时长（秒，默认 3.0）')
    args = parser.parse_args()

    print(f"{'=' * 50}")
    print(f"  KWS 唤醒词本地测试")
    print(f"  模型: {MODEL_PATH}")
    print(f"{'=' * 50}")

    if not MODEL_PATH.exists():
        print(f"❌ 模型文件不存在: {MODEL_PATH}")
        print(f"   请先训练模型: python3 scripts/train_kws.py --train --quantize")
        sys.exit(1)

    if args.file:
        test_file(args.file)
    else:
        test_mic(args.duration)


if __name__ == '__main__':
    main()
