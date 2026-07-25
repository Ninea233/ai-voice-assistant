#!/usr/bin/env python3
#===========================================
# train_kws.py
# KWS 唤醒词模型训练 Pipeline
#
# 功能:
#   1. 用 Speech Commands 数据集训练基础 KWS 模型
#   2. 用自定义录音微调「小九小九」
#   3. 模型转换 TFLite
#   4. 模型量化（int8 可选）
#
# 使用方式:
#   pip install tensorflow librosa sounddevice numpy
#   python scripts/train_kws.py                  # 完整训练
#   python scripts/train_kws.py --finetune        # 微调模式
#   python scripts/train_kws.py --record          # 仅录音
#
# 依赖:
#   tensorflow >= 2.8
#   librosa
#   numpy
#   sounddevice (录音用)
#===========================================

import argparse
import json
import os
import sys
import wave
from pathlib import Path

import numpy as np

# ============================================
# 配置
# ============================================
SAMPLE_RATE = 16000          # 采样率 (Hz)
FRAME_LEN_MS = 30            # 帧长 (ms)
FRAME_SHIFT_MS = 10          # 帧移 (ms)
NUM_MFCC = 40                # MFCC 维度
NUM_MEL_BINS = 40            # Mel 滤波器数量
FFT_SIZE = 512               # FFT 大小
NUM_FFT_BINS = FFT_SIZE // 2 + 1  # 257
NUM_CONTEXT_FRAMES = 10      # 上下文帧数（用于分类）

WAKE_WORD = "正样本"       # 唤醒词（目录名）
NEGATIVE_LABEL = "负样本"  # 负样本标签（目录名）

# 数据路径
BASE_DIR = Path(__file__).resolve().parent.parent
DATA_DIR = BASE_DIR / "data"         # 录音数据
MODELS_DIR = BASE_DIR / "models"     # 模型输出
SCRIPTS_DIR = BASE_DIR / "scripts"

# ============================================
# MFCC 特征提取（手动实现，与 C++ KWSTFLite 完全一致）
# ============================================
def _hz_to_mel(hz: float) -> float:
    """频率 → Mel 尺度"""
    return 2595.0 * np.log10(1.0 + hz / 700.0)


def _mel_to_hz(mel: float) -> float:
    """Mel 尺度 → 频率"""
    return 700.0 * (10.0 ** (mel / 2595.0) - 1.0)


def _precompute_mel_filterbank() -> np.ndarray:
    """预计算 Mel 滤波器组（与 C++ PreComputeFilterbank 一致）"""
    low_mel = _hz_to_mel(0.0)
    high_mel = _hz_to_mel(SAMPLE_RATE / 2.0)
    mel_points = np.linspace(low_mel, high_mel, NUM_MEL_BINS + 2)
    hz_points = _mel_to_hz(mel_points)
    bin_indices = np.round(hz_points * FFT_SIZE / SAMPLE_RATE).astype(int)
    bin_indices = np.clip(bin_indices, 0, NUM_FFT_BINS - 1)

    filterbank = np.zeros((NUM_MEL_BINS, NUM_FFT_BINS), dtype=np.float64)
    for m in range(NUM_MEL_BINS):
        left, center, right = bin_indices[m], bin_indices[m + 1], bin_indices[m + 2]
        for k in range(left, center):
            if center != left:
                filterbank[m, k] = (k - left) / (center - left)
        for k in range(center, right + 1):
            if right != center:
                filterbank[m, k] = (right - k) / (right - center)
    return filterbank


def _precompute_dct() -> np.ndarray:
    """预计算 DCT-II 矩阵（与 C++ PreComputeDCT 一致）"""
    dct = np.zeros((NUM_MFCC, NUM_MEL_BINS), dtype=np.float64)
    for i in range(NUM_MFCC):
        scale = np.sqrt(1.0 / NUM_MEL_BINS) if i == 0 else np.sqrt(2.0 / NUM_MEL_BINS)
        for j in range(NUM_MEL_BINS):
            dct[i, j] = scale * np.cos(np.pi * i * (j + 0.5) / NUM_MEL_BINS)
    return dct


# 预计算（全局缓存，避免重复计算）
_MEL_FILTERBANK = _precompute_mel_filterbank()
_DCT_MATRIX = _precompute_dct()
_HAMMING_WINDOW = np.hamming(FRAME_LEN_MS * SAMPLE_RATE // 1000).astype(np.float64)


def extract_mfcc(audio: np.ndarray, sr: int = SAMPLE_RATE) -> np.ndarray:
    """
    提取 MFCC 特征，与 C++ KWSTFLite::ComputeOneMFCC 完全一致。
    Pipeline: 预加重 → 分帧 → Hamming 窗 → FFT → 功率谱
             → Mel 滤波器组 → log → DCT → MFCC 系数
    """
    k_pre_emphasis = 0.97
    k_frame_len = int(sr * FRAME_LEN_MS / 1000)     # 480
    k_frame_shift = int(sr * FRAME_SHIFT_MS / 1000)  # 160
    k_fft_size = 512
    k_num_fft_bins = k_fft_size // 2 + 1

    # 预加重: y[n] = x[n] - α * x[n-1]
    audio_f = audio.astype(np.float64)
    preemphasized = np.zeros_like(audio_f)
    preemphasized[0] = audio_f[0]
    preemphasized[1:] = audio_f[1:] - k_pre_emphasis * audio_f[:-1]

    # 分帧并计算每帧 MFCC
    mfcc_list = []
    pos = 0
    while pos + k_frame_len <= len(preemphasized):
        frame = preemphasized[pos:pos + k_frame_len]

        # Hamming 窗
        windowed = frame * _HAMMING_WINDOW

        # FFT + 功率谱（|X[k]|^2）
        fft_in = np.zeros(k_fft_size, dtype=np.float64)
        fft_in[:k_frame_len] = windowed
        spectrum = np.fft.rfft(fft_in)
        power = spectrum.real ** 2 + spectrum.imag ** 2

        # Mel 滤波器组（double 精度累加，与 C++ 一致）
        mel_energies = _MEL_FILTERBANK @ power

        # Log（添加极小值避免 log(0)）
        log_energies = np.log(mel_energies + 1e-10)

        # DCT → MFCC
        mfcc = _DCT_MATRIX @ log_energies
        mfcc_list.append(mfcc.astype(np.float32))

        pos += k_frame_shift

    return np.array(mfcc_list)  # (num_frames, NUM_MFCC)


def extract_context(audio: np.ndarray, num_context: int = NUM_CONTEXT_FRAMES):
    """
    提取带上下文的特征。
    将连续 num_context 帧的 MFCC 拼接为一维向量。
    """
    mfcc = extract_mfcc(audio)
    if mfcc.shape[0] < num_context:
        # 音频太短，填充
        pad = num_context - mfcc.shape[0]
        mfcc = np.pad(mfcc, ((pad, 0), (0, 0)), mode='constant')

    features = []
    for i in range(mfcc.shape[0] - num_context + 1):
        ctx = mfcc[i:i + num_context].flatten()  # (num_context * NUM_MFCC,)
        features.append(ctx)

    return np.array(features)


# ============================================
# 录音工具
# ============================================
def record_audio(duration: float = 2.0, sample_rate: int = SAMPLE_RATE) -> np.ndarray:
    """录制音频"""
    try:
        import sounddevice as sd
    except ImportError:
        print("错误: 请安装 sounddevice: pip install sounddevice")
        sys.exit(1)

    print(f"  🎙️ 录制 {duration} 秒... 请说话")
    recording = sd.rec(int(duration * sample_rate), samplerate=sample_rate,
                        channels=1, dtype='int16')
    sd.wait()
    print("  ✅ 录制完成")
    return recording.flatten()


def save_wav(path: Path, audio: np.ndarray, sample_rate: int = SAMPLE_RATE):
    """保存 WAV 文件"""
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)  # 16-bit
        wf.setframerate(sample_rate)
        wf.writeframes(audio.tobytes())
    print(f"  💾 保存: {path}")


def record_wake_word(count: int = 100, auto: bool = False):
    """录制唤醒词样本"""
    pos_dir = DATA_DIR / "wake_word" / WAKE_WORD
    pos_dir.mkdir(parents=True, exist_ok=True)

    # 检查已有样本数
    existing = list(pos_dir.glob("*.wav"))
    print(f"\n📂 已有 {len(existing)} 个样本在 {pos_dir}")

    to_record = count - len(existing)
    if to_record <= 0:
        print(f"✅ 已有足够样本 ({count}个)，无需再录")
        return

    print(f"\n🎤 需要录制 {to_record} 条「{WAKE_WORD}」样本")
    print("  每次录制 1.5 秒，请清晰说出「小九小九」")
    if auto:
        print("  自动模式：自动连续录音，准备好后开始\n")
        import time
        for i in range(to_record):
            idx = len(existing) + i + 1
            print(f"  [{idx}/{count}] 录制第 {idx} 条（{i+1}/{to_record}）...")
            audio = record_audio(1.5)
            if np.max(np.abs(audio)) < 500:
                print("  ⚠️ 检测不到声音，跳过，重试...")
                # 自动重试一次
                time.sleep(0.3)
                audio = record_audio(1.5)
            if np.max(np.abs(audio)) < 500:
                print("  ⚠️ 仍无声，继续下一组")
                continue
            save_wav(pos_dir / f"sample_{idx:04d}.wav", audio)
            time.sleep(0.5)  # 间隔半秒
    else:
        print("  按 Enter 开始录制，输入 q 退出\n")
        for i in range(to_record):
            idx = len(existing) + i + 1
            inp = input(f"  [{idx}/{count}] 按 Enter 录制第 {idx} 条...")
            if inp.strip().lower() == 'q':
                break
            audio = record_audio(1.5)
            if np.max(np.abs(audio)) < 500:
                print("  ⚠️ 检测不到声音，跳过")
                continue
            save_wav(pos_dir / f"sample_{idx:04d}.wav", audio)
            print()


def record_negative(count: int = 100, auto: bool = False):
    """录制负样本（非唤醒词语音）"""
    neg_dir = DATA_DIR / "wake_word" / NEGATIVE_LABEL
    neg_dir.mkdir(parents=True, exist_ok=True)

    # 检查已有样本数
    existing = list(neg_dir.glob("*.wav"))
    print(f"\n📂 已有 {len(existing)} 个负样本在 {neg_dir}")

    to_record = count - len(existing)
    if to_record <= 0:
        print(f"✅ 已有足够负样本 ({count}个)，无需再录")
        return

    print(f"\n🎤 需要录制 {to_record} 条负样本")
    print("  每次录制 2 秒，请随意说话（不要包含「小九小九」）")
    if auto:
        print("  自动模式：准备好后开始\n")
        import time
        for i in range(to_record):
            idx = len(existing) + i + 1
            print(f"  [{idx}/{count}] 录制第 {idx} 条负样本...")
            audio = record_audio(2.0)
            if np.max(np.abs(audio)) < 500:
                print("  ⚠️ 检测不到声音，跳过，重试...")
                time.sleep(0.3)
                audio = record_audio(2.0)
            if np.max(np.abs(audio)) < 500:
                continue
            save_wav(neg_dir / f"negative_{idx:04d}.wav", audio)
            time.sleep(0.5)
    else:
        print("  按 Enter 开始录制，输入 q 退出\n")
        for i in range(to_record):
            idx = len(existing) + i + 1
            inp = input(f"  [{idx}/{count}] 按 Enter 录制第 {idx} 条...")
            if inp.strip().lower() == 'q':
                break
            audio = record_audio(2.0)
            if np.max(np.abs(audio)) < 500:
                print("  ⚠️ 检测不到声音，跳过")
                continue
            save_wav(neg_dir / f"negative_{idx:04d}.wav", audio)
            print()


# ============================================
# 数据加载与预处理
# ============================================
def load_wav(path: Path, target_sr: int = SAMPLE_RATE) -> np.ndarray:
    """加载 WAV 文件"""
    try:
        import librosa
    except ImportError:
        print("错误: 请安装 librosa: pip install librosa")
        sys.exit(1)

    audio, sr = librosa.load(str(path), sr=target_sr, mono=True)
    return audio


def load_custom_data() -> tuple:
    """
    加载自定义录音数据（按文件拆分，每文件采样有限帧避免过拟合）。

    返回:
        X: 特征数组 (n_samples, NUM_CONTEXT_FRAMES * NUM_MFCC)
        y: 标签数组 (n_samples,)
        file_ids: 文件 ID 数组（用于文件级拆分）
    """
    import random

    X, y = [], []

    def _load_file_features(wav_path: Path, label: int):
        """加载一个文件的特征，随机采样最多 30 帧避免相关帧过多"""
        audio = load_wav(wav_path)
        features = extract_context(audio)
        if len(features) == 0:
            return None
        # 随机采样最多 30 帧（避免相邻帧高度相关）
        max_per_file = 30
        if len(features) > max_per_file:
            indices = sorted(random.sample(range(len(features)), max_per_file))
            features = features[indices]
        return features

    # 加载正样本
    pos_dir = DATA_DIR / "wake_word" / WAKE_WORD
    file_ids = []
    file_counter = 0
    if pos_dir.exists():
        for wav_path in sorted(pos_dir.glob("*.wav")):
            feats = _load_file_features(wav_path, 1)
            if feats is not None:
                X.append(feats)
                y.append(np.ones(len(feats), dtype=np.int32))
                file_ids.extend([file_counter] * len(feats))
                file_counter += 1

    # 加载负样本
    neg_dir = DATA_DIR / "wake_word" / NEGATIVE_LABEL
    if neg_dir.exists():
        for wav_path in sorted(neg_dir.glob("*.wav")):
            feats = _load_file_features(wav_path, 0)
            if feats is not None:
                X.append(feats)
                y.append(np.zeros(len(feats), dtype=np.int32))
                file_ids.extend([file_counter] * len(feats))
                file_counter += 1

    if len(X) == 0:
        return np.array([]), np.array([]), np.array([])

    X = np.concatenate(X, axis=0)
    y = np.concatenate(y, axis=0)

    return X, y, np.array(file_ids, dtype=np.int32)


def download_speech_commands(data_dir: str = None):
    """
    下载 Speech Commands v2 数据集（用于基础训练）。
    """
    if data_dir is None:
        data_dir = os.path.join(str(DATA_DIR), "speech_commands")

    os.makedirs(data_dir, exist_ok=True)

    # 使用 TensorFlow 的数据集 API
    print("📥 下载 Speech Commands 数据集...")
    try:
        import tensorflow_datasets as tfds
    except ImportError:
        print("错误: 请安装 tensorflow-datasets: pip install tensorflow-datasets")
        sys.exit(1)

    # 使用 tfds 加载
    print("  这是首次下载，可能需要几分钟...")
    ds = tfds.load(
        'speech_commands',
        split='train',
        data_dir=data_dir,
        as_supervised=True,
        shuffle_files=False,
    )

    return ds


# ============================================
# 模型定义
# ============================================
def build_kws_model(input_dim: int, num_classes: int = 2) -> 'tf.keras.Model':
    """
    构建轻量级 KWS 模型。

    架构: DNN 2层隐藏层，减少参数量避免小数据集过拟合
    参数: ~70K（适合 i.MX6ULL Cortex-A7 实时推理）
    """
    import tensorflow as tf

    model = tf.keras.Sequential([
        # 输入: 拼接的上下文 MFCC 特征向量
        tf.keras.layers.Input(shape=(input_dim,), name="mfcc_input"),

        # 全连接层 1（减少神经元，增加正则化）
        tf.keras.layers.Dense(64, activation='relu',
                              kernel_regularizer=tf.keras.regularizers.l2(0.001),
                              name="fc1"),
        tf.keras.layers.Dropout(0.5, name="dropout1"),
        tf.keras.layers.BatchNormalization(),

        # 全连接层 2
        tf.keras.layers.Dense(32, activation='relu',
                              kernel_regularizer=tf.keras.regularizers.l2(0.001),
                              name="fc2"),
        tf.keras.layers.Dropout(0.5, name="dropout2"),
        tf.keras.layers.BatchNormalization(),

        # 输出层
        tf.keras.layers.Dense(num_classes, activation='softmax', name="output"),
    ])

    return model


# ============================================
# 训练流程
# ============================================
def train_base_model():
    """
    使用 Speech Commands 数据集训练基础 KWS 模型。
    """
    import tensorflow as tf

    print("\n" + "=" * 50)
    print(" 训练基础 KWS 模型 (Speech Commands)")
    print("=" * 50)

    # 下载数据
    ds = download_speech_commands()

    # TODO: 实现完整的数据加载、MFCC 特征提取、训练循环
    # 这需要处理 Speech Commands 数据集的标签映射
    # 选择 "yes", "no", "marvin" 等作为正样本候选
    # 选择其他词作为负样本

    print("\n⚠️  Speech Commands 数据集很大（~1GB），首次下载较慢")
    print(" 可以先跳过基础训练，直接使用自定义数据微调")
    print(" 或者使用后续的 --finetune 模式\n")


def train_custom():
    """
    使用自定义录制数据训练/微调 KWS 模型。
    """
    import tensorflow as tf

    print("\n" + "=" * 50)
    print(f" 训练「{WAKE_WORD}」KWS 模型")
    print("=" * 50)

    # 1. 加载数据
    print("[1/5] 加载录音数据...")
    X, y, file_ids = load_custom_data()
    if len(X) == 0:
        print("❌ 未找到录音数据！请先运行 --record 录制")
        print(f"  录音目录: {DATA_DIR}/wake_word/")
        return None

    print(f"  加载了 {len(X)} 个样本, 特征维度: {X.shape[1]}")
    print(f"  正样本: {np.sum(y == 1)}, 负样本: {np.sum(y == 0)}")
    print(f"  文件数: {len(np.unique(file_ids))}")

    # 2. 按文件分割数据集（避免同一文件特征同时出现在训练和验证集）
    print("[2/5] 按文件分割训练/验证集...")
    from sklearn.model_selection import train_test_split
    unique_files = np.unique(file_ids)
    # 按文件比例分割
    train_files, val_files = train_test_split(
        unique_files, test_size=0.2, random_state=42
    )
    train_mask = np.isin(file_ids, train_files)
    val_mask = np.isin(file_ids, val_files)
    X_train, X_val = X[train_mask], X[val_mask]
    y_train, y_val = y[train_mask], y[val_mask]
    print(f"  训练: {len(X_train)} (来自 {len(train_files)} 个文件)")
    print(f"  验证: {len(X_val)} (来自 {len(val_files)} 个文件)")

    # 3. 构建模型
    print("[3/5] 构建 KWS 模型...")
    input_dim = X.shape[1]
    model = build_kws_model(input_dim)
    model.summary()

    # 4. 训练
    print("[4/5] 训练模型...")
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=0.001),
        loss='sparse_categorical_crossentropy',
        metrics=['accuracy'],
    )

    # 早停和模型保存回调
    callbacks = [
        tf.keras.callbacks.EarlyStopping(
            monitor='val_loss', patience=10, restore_best_weights=True
        ),
        tf.keras.callbacks.ReduceLROnPlateau(
            monitor='val_loss', factor=0.5, patience=5, min_lr=1e-6
        ),
    ]

    history = model.fit(
        X_train, y_train,
        validation_data=(X_val, y_val),
        epochs=100,
        batch_size=32,
        callbacks=callbacks,
        verbose=1,
    )

    # 5. 评估
    print("[5/5] 评估模型...")
    val_loss, val_acc = model.evaluate(X_val, y_val, verbose=0)
    print(f"\n  ✅ 验证集准确率: {val_acc:.4f} ({val_acc*100:.1f}%)")
    print(f"     验证集损失:   {val_loss:.4f}")

    return model


def convert_to_tflite(model, quantize: bool = False) -> bytes:
    """
    将 Keras 模型转换为 TFLite 格式。

    Args:
        model: Keras 模型
        quantize: 是否进行 Int8 量化（减小体积，加速推理）

    Returns:
        TFLite 模型二进制数据
    """
    import tensorflow as tf

    print("\n📦 转换 TFLite 模型...")

    converter = tf.lite.TFLiteConverter.from_keras_model(model)

    if quantize:
        # Int8 量化（推荐用于 ARM 部署）
        converter.optimizations = [tf.lite.Optimize.DEFAULT]
        converter.target_spec.supported_types = [tf.float16]
        print("  量化: Float16（平衡精度与速度）")
    else:
        print("  量化: Float32（最高精度）")

    tflite_model = converter.convert()

    # 保存模型
    model_name = f"kws_model_{WAKE_WORD}.tflite"
    model_path = MODELS_DIR / model_name
    model_path.parent.mkdir(parents=True, exist_ok=True)
    with open(model_path, 'wb') as f:
        f.write(tflite_model)

    print(f"  ✅ TFLite 模型已保存: {model_path}")
    print(f"     大小: {len(tflite_model) / 1024:.1f} KB")

    # 同时保存一份为 kws_model.tflite（方便程序加载）
    default_path = MODELS_DIR / "kws_model.tflite"
    with open(default_path, 'wb') as f:
        f.write(tflite_model)
    print(f"  ✅ 已复制到: {default_path}")

    return tflite_model


def test_tflite_inference(tflite_model: bytes):
    """
    在 PC 上测试 TFLite 模型推理。
    """
    import tensorflow as tf

    print("\n🧪 测试 TFLite 推理...")

    interpreter = tf.lite.Interpreter(model_content=tflite_model)
    interpreter.allocate_tensors()

    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()

    print(f"  输入: {input_details}")
    print(f"  输出: {output_details}")

    # 创建随机输入测试
    input_shape = input_details[0]['shape']
    input_data = np.random.randn(*input_shape).astype(np.float32)

    interpreter.set_tensor(input_details[0]['index'], input_data)
    interpreter.invoke()

    output = interpreter.get_tensor(output_details[0]['index'])
    print(f"  推理结果: {output}")
    print(f"  非唤醒词: {output[0][0]:.4f}, 唤醒词: {output[0][1]:.4f}")
    print(f"  ✅ TFLite 推理正常")


# ============================================
# 验证 C++ 兼容性
# ============================================
def verify_compatibility(tflite_model: bytes):
    """
    验证 TFLite 模型与 C++ KWSTFLite 的兼容性。
    """
    import tensorflow as tf

    interpreter = tf.lite.Interpreter(model_content=tflite_model)
    interpreter.allocate_tensors()

    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()

    print("\n🔍 C++ 兼容性检查:")
    print(f"  - 输入张量数量: {len(input_details)} (期望: 1)")
    print(f"  - 输出张量数量: {len(output_details)} (期望: 1)")

    input_shape = input_details[0]['shape']
    input_dtype = input_details[0]['dtype']
    output_shape = output_details[0]['shape']
    output_dtype = output_details[0]['dtype']

    print(f"  - 输入 shape: {input_shape}")
    print(f"  - 输入 dtype: {input_dtype} (期望: float32)")
    print(f"  - 输出 shape: {output_shape} (期望: [1, 2])")
    print(f"  - 输出 dtype: {output_dtype} (期望: float32)")

    # 检查 C++ 代码兼容性
    issues = []
    if len(input_details) != 1:
        issues.append("输入张量数量不为 1，需要修改 C++ 代码")
    if len(output_details) != 1:
        issues.append("输出张量数量不为 1，需要修改 C++ 代码")
    if input_dtype != np.float32:
        issues.append(f"输入类型为 {input_dtype}，需要修改 C++ 代码")
    if input_shape[0] != 1:
        issues.append(f"输入 batch size 不为 1 (shape={input_shape})")

    if issues:
        print("  ⚠️ 兼容性问题:")
        for issue in issues:
            print(f"    - {issue}")
    else:
        print("  ✅ 完全兼容！")

    return len(issues) == 0


# ============================================
# 命令行入口
# ============================================
def main():
    parser = argparse.ArgumentParser(
        description="KWS 唤醒词模型训练 Pipeline",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 仅录音（先收集数据）
  python scripts/train_kws.py --record --count 50

  # 训练模型
  python scripts/train_kws.py --train

  # 训练并量化
  python scripts/train_kws.py --train --quantize

  # 完整流程：录音 → 训练 → 测试
  python scripts/train_kws.py --record --train --test
        """,
    )

    parser.add_argument('--record', action='store_true',
                        help='录制唤醒词样本')
    parser.add_argument('--auto-record', action='store_true',
                        help='自动连续录音（无需按 Enter）')
    parser.add_argument('--count', type=int, default=50,
                        help='录制样本数量 (默认: 50)')
    parser.add_argument('--train', action='store_true',
                        help='训练 KWS 模型')
    parser.add_argument('--finetune', action='store_true',
                        help='微调模式（从已有模型继续训练）')
    parser.add_argument('--quantize', action='store_true',
                        help='量化模型（Float16）')
    parser.add_argument('--test', action='store_true',
                        help='测试 TFLite 推理')
    parser.add_argument('--all', action='store_true',
                        help='完整流程（不录音，直接用已有数据）')

    args = parser.parse_args()

    # 如果没有参数，显示帮助
    if len(sys.argv) == 1:
        parser.print_help()
        print("\n💡 建议流程:")
        print("  步骤 1: python scripts/train_kws.py --record --count 100")
        print("  步骤 2: python scripts/train_kws.py --train --test")
        print("  步骤 3: python scripts/train_kws.py --train --quantize --test")
        return

    # 录音模式
    if args.record or args.auto_record:
        auto = args.auto_record
        print("\n🎤 === 语音数据录制 ===")
        record_wake_word(args.count, auto=auto)

        record_negative(max(50, args.count), auto=auto)

    # 训练模式
    if args.train or args.all:
        model = train_custom()
        if model is None:
            return

        # 转换 TFLite
        tflite_model = convert_to_tflite(model, quantize=args.quantize)

        # 验证兼容性
        verify_compatibility(tflite_model)

        # 测试
        if args.test:
            test_tflite_inference(tflite_model)

    # 微调模式
    if args.finetune:
        print("\n⚠️  微调模式尚未实现")
        print("  当前先使用完整训练模式: --train")

    print("\n✅ 完成!")


if __name__ == '__main__':
    main()
