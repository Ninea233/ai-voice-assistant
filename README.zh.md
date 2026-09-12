# Agent 语音智能助手 v2.3.1

> 🎙️ 基于 i.MX6ULL (ARM Cortex-A7) 的离线/在线混合 Agent 语音助手系统
>
> 本地唤醒 + 云端大脑 + Agent 架构，让嵌入式设备开口说话

[![Platform](https://img.shields.io/badge/platform-i.MX6ULL%20ARM-blue)](https://www.nxp.com/products/processors-and-microcontrollers/arm-processors/i-mx-applications-processors/i-mx-6-processors/i-mx-6ull-single-core-processor-with-arm-cortex-a7:i.MX6ULL)
[![Language](https://img.shields.io/badge/language-C%2B%2B14-orange)](https://en.cppreference.com/w/cpp/14)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

---

## 📖 目录

- [功能特性](#功能特性)
- [系统架构](#系统架构)
- [技术亮点](#技术亮点)
- [快速开始](#快速开始)
- [配置说明](#配置说明)
- [技能系统](#技能系统)
- [项目结构](#项目结构)
- [编译与部署](#编译与部署)
- [技术栈](#技术栈)
- [常见问题](#常见问题)
- [许可证](#许可证)

---

## 功能特性

### 🎯 核心交互

| 功能 | 说明 |
|------|------|
| **语音唤醒** | 本地 TFLite KWS 模型检测唤醒词"小九小九"，阈值 0.97，连续 4 帧确认 |
| **唤醒反馈** | 检测后立即播放 `wakeup.wav` 提示音 |
| **连续对话** | 一次唤醒后持续聆听，多轮对话无需反复唤醒 |
| **自动休眠** | 10 秒无语音自动休眠；说"休眠"手动休眠 |

### ☁️ 云端智能

| 功能 | 引擎 | 说明 |
|------|------|------|
| **语音识别 (ASR)** | 讯飞中文识别大模型 | 多引擎表决框架（置信度加权）；**当前仅接入主引擎**，方言引擎未接线 |
| **对话生成 (LLM)** | 星火 Spark-X2 | OpenAI 兼容 API + 原生 Function Calling，携带完整对话上下文 |
| **语音合成 (TTS)** | 讯飞超拟人合成 | 并行管线：多句并发合成、按句序播放，合成与 LLM 生成重叠，消除逐句合成延迟 |

### 🤖 Agent 智能体

| 功能 | 说明 |
|------|------|
| **Action** | keyword或LLM均可独立触发，直接执行不回注（灯光/空调/音量/休眠/偏好）——偏好两个 action 带 `key`/`value` 参数 |
| **Skill** | 仅 LLM 触发，SKILL.md 两阶段加载，必回注（今日简报） |
| **MCP** | 仅 LLM 触发，工具定义参考 MCP 的 `inputSchema` 字段格式（**未实现 JSON-RPC 2.0 协议**），全量可见，必回注 |
| **短期记忆** | 唤醒周期内保留完整对话上下文（`agent_core_->history_`） |
| **长期记忆** | 偏好持久化存储在 `agent_prompt.md` 可改动区 |

### 🏠 智能家居控制

通过语音指令控制家居设备：

- 💡 **灯光**：开灯 / 关灯
- 🌡️ **空调**：开关 / 温度调节
- 🔊 **音量**：调大 / 调小

### 📡 实时数据

- 🌤️ **天气预报**：wttr.in API 获取当前天气 + 未来 3 天预报（温度、湿度、风力、天气状况）
- 🕐 **时间查询**：系统本地时间 + POSIX 时区映射（支持 `Asia/Shanghai`、`America/New_York` 等 15+ 时区）
- 📋 **今日简报**：LLM 自动综合天气、时间生成每日简报

---

## 系统架构

### 整体架构

```
┌──────────────────────────────────────────────────────┐
│                     硬件层                             │
│  i.MX6ULL (Cortex-A7) + WM8960 + 麦克风 + 喇叭        │
└──────────────────┬───────────────────────────────────┘
                   │
┌──────────────────▼───────────────────────────────────┐
│                    音频采集层                           │
│  ALSA AudioCapture (16kHz, 单声道, S16_LE)             │
│  ├─ 环形缓冲区 (Ring Buffer)                           │
│  └─ VAD 语音活动检测                                   │
└──────────────────┬───────────────────────────────────┘
                   │
┌──────────────────▼───────────────────────────────────┐
│                   唤醒检测层                            │
│  KWS TFLite 引擎                                       │
│  ├─ MFCC: 预加重→分帧→加窗→FFT→Mel滤波→log→DCT       │
│  └─ TFLite 推理 (10帧×40维 → softmax → 唤醒词)        │
└──────────────────┬───────────────────────────────────┘
                   │
┌──────────────────▼───────────────────────────────────┐
│                   云端服务层                            │
│  ┌─────────┐  ┌─────────┐  ┌───────────────────┐     │
│  │   ASR   │  │   LLM   │  │        TTS         │     │
│  │ WSS流式 │  │ SSE流式 │  │ 并行合成+按序播放   │     │
│  │ 多引擎表决│  │ Agent  │  │ ALSA DMA 直写播放   │     │
│  └────┬────┘  └────┬────┘  └─────────┬─────────┘     │
│       │            │                 │                 │
│  ┌────▼────────────▼─────────────────▼──────────┐     │
│  │              网络层                            │     │
│  │  TCP + TLS (OpenSSL) + WebSocket (RFC 6455)   │     │
│  │  + 讯飞 HMAC-SHA256 签名认证                   │     │
│  └──────────────────────────────────────────────┘     │
└──────────────────┬───────────────────────────────────┘
                   │
┌──────────────────▼───────────────────────────────────┐
│                    Agent 决策层                        │
│  ┌─────────────┐  ┌──────────────┐  ┌─────────────┐  │
│  │ Action匹配   │  │ LLM Agent   │  │  执行器      │  │
│  │ keyword或LLM均可独立触发 │  │ ≤3轮调用     │  │ action无回注 │  │
│  └─────────────┘  └──────────────┘  └─────────────┘  │
│                          │                             │
│              ┌───────────▼──────────┐                  │
│              │     状态机调度        │                  │
│              │ SLEEP→WAKEUP→LISTEN  │                  │
│              │ →PROCESS→SPEAK→循环  │                  │
│              └──────────────────────┘                  │
└──────────────────────────────────────────────────────┘
```

### 状态机

```
     ┌──────────────────────────────────┐
     │                                  │
     ▼                                  │
  ┌──────┐   唤醒词    ┌────────┐      │
  │ SLEEP │ ────────→ │ WAKEUP │      │
  │ (低功 │           │ (播放   │      │
  │  耗)  │ ←──────── │ 提示音) │      │
  └──────┘   休眠/    └───┬────┘      │
     ↑       10s静默      │            │
     │                    ▼            │
     │              ┌──────────┐       │
     │              │ LISTENING│       │
     │              │ (VAD检测)│       │
     │              └────┬─────┘       │
     │                   │ 语音结束     │
     │                   ▼              │
     │              ┌──────────┐       │
     │              │PROCESSING│       │
     │              │ ASR→Agent│       │
     │              │  →TTS    │       │
     │              └────┬─────┘       │
     │                   │              │
     │                   ▼              │
     │              ┌──────────┐       │
     └──────────────│ SPEAKING │───────┘
         播放完成    │ (流式TTS)│  连续对话
                    └──────────┘  (回LISTENING)
```

---

## 技术亮点

### 1. 完整离线唤醒

不依赖云端，本地 KWS 引擎毫秒级响应：

- **完整 MFCC Pipeline**：预加重 → 分帧 → Hamming 加窗 → kissfft FFT → Mel 滤波器组 → log → DCT
- **Float16 量化模型**：120KB，训练验证准确率 99.8%
- **Python 训练工具链**：`train_kws.py` — 录音→训练→量化→部署一站式
- **连续 4 帧确认**：有效防止误唤醒

### 2. Agent 架构：三层分离（action > skill > MCP）

```
用户说"天气怎么样"
      │
      ▼
┌─────────────┐      ┌──────────┐      ┌─────────┐
│   action    │  >   │  skill   │  >   │   MCP   │
│ keyword/LLM独立触发 │      │ 仅 LLM   │      │ 仅 LLM  │
│  不回注     │      │ 必回注   │      │ 必回注  │
│ 硬件操作    │      │ 智能任务 │      │ 数据工具 │
└──────┬──────┘      └────┬─────┘      └────┬────┘
       │ ✗                │ ✗              │
   (无匹配action)    (无匹配skill)    ┌──────▼──────┐
                                  │ MCP 返回数据  │
                                  │ 25°C, 湿度60% │
                                  └──────┬──────┘
                                         │
                                    ┌────▼─────┐
                                    │ LLM 润色  │
                                    │ "杭州今天  │
                                    │  25°C..."  │
                                    └──────────┘
```

| 层级 | 触发方式 | 回注 LLM | 配置 | 典型场景 |
|------|----------|----------|------|----------|
| **action** | keyword或LLM均可独立触发 | 不回注 | `actions.json` | 开灯、空调、音量、休眠 |
| **skill** | 仅 LLM | 必回注 | `skills/*/SKILL.md` | 今日简报（内部调 MCP） |
| **MCP** | 仅 LLM | 必回注 | `mcp_tools.json` | 天气、时间、新闻 |

**设计优势**：
- **LLM 可见所有工具**：action 全量 / skill 仅摘要 / MCP 全量
- **skill 跟 Agent 对齐**：文件夹 + SKILL.md（YAML frontmatter），Phase 1 摘要 → Phase 2 完整加载
- **工具定义参考 MCP 风格**：`inputSchema` 采用 MCP 的字段格式；**未实现 JSON-RPC 2.0 协议**（无 initialize/tools/list 握手），执行结果以纯文本回注，`_implementation` 只决定执行方式、不发 LLM；HTTP 类型纯配置
- **配置驱动**：增删 action/MCP 无需改 C++，增删 skill 只需加文件夹

### 3. 并行 TTS 合成与顺序播放

LLM 流式输出 → 增量切句 → N 个合成线程并行合成（乱序完成）→ Reorder Buffer 按序号重排 → 播放调度线程严格按句序 DMA 播放：

```
LLM 流式增量 ──→ 切句 ──→ 任务队列 ──→ 合成线程池(×3)  ──┐
                                       句1✓ 句2… 句3✓   │ 乱序完成
                                                         ▼
播放调度线程 ←── 只取连续序号 ←── Reorder Buffer(按序号暂存)
     │
     └─ 句1播放时，句2/句3早已合成就绪 → 后续句子零等待
```

收益：整段回复的合成延迟从「Σ 每句合成」压缩到「最慢 1 句」；合成与 LLM 生成重叠，首句播放延迟 ≈ 首句合成时间。

### 4. ASR 多引擎表决

主引擎（普通话）始终运行；多引擎表决算法已实现（置信度加权）：

- 单引擎结果直接采用
- 多引擎时先取**置信度最高者**为基准，再把「文本与基准完全相同」的引擎置信度**累加**（相同文本互相印证）→ `confidence = min(累加, 1.0)`

> ⚠️ 现状：**只接入了主引擎**。方言引擎的框架与配置项（`[asr] dialect_enabled`）都在，但该配置在 C++ 中未被读取，因此方言引擎当前不会运行。

### 5. 嵌入式友好

- **单二进制**：5.3MB ARM ELF，无运行时依赖
- **无数据库**：记忆系统基于 Markdown 文件
- **无 Python/Node.js**：纯 C++14，无 GC 停顿
- **EGLIBC 2.19 兼容**：Linaro GCC 4.9.4 + 多层符号存根，全静态链接

---

## 快速开始

### 前置条件

- 正点原子 i.MX6ULL 开发板（阿尔法/EMC 版本）+ WM8960 音频
- 麦克风 + 喇叭/耳机
- 网络连接（WiFi/以太网）
- 讯飞开放平台账号（ASR/LLM/TTS API 凭据）

### 1. 编译

```bash
cd ai_assistant

# 首次环境准备（仅需一次）
bash scripts/build_tflite_arm.sh         # TFLite ARM 库
bash scripts/build_openssl_arm.sh        # OpenSSL ARM 静态库
bash scripts/download_kissfft.sh         # kissfft 轻量 FFT 库

# 编译（TFLite 模式！）
bash scripts/cross_compile.sh release tflite
```

> ⚠️ **重要**：开发板使用 EGLIBC 2.19，必须加 `tflite` 参数使用 Linaro GCC 4.9.4 编译。

### 2. 配置

编辑 `config/assistant.conf`，填入讯飞凭据：

```ini
[cloud]
app_id = 你的APPID
api_key = 你的APIKey
api_secret = 你的APISecret

[llm]
api_key = 你的星火Key

[tts]
auth = 你的TTS Auth Token
```

### 3. 部署

```bash
bash scripts/deploy.sh <开发板IP>
```

### 4. 运行

```bash
# SSH 登录开发板
cd /ai_assistant
sh mic_in_config.sh                       # 配置声卡（每次重启一次）
./ai_assistant -c config/assistant.conf   # 启动助手
```

听到唤醒提示音后，说"小九小九"开始对话！

---

## 配置说明

所有配置在 `config/assistant.conf`，分节如下：

| 节 | 说明 |
|------|------|
| `[audio]` | 录音/播放设备、采样率、VAD 超时 |
| `[kws]` | 模型路径、检测阈值、唤醒词 |
| `[cloud]` | 讯飞 API 凭据（ASR/LLM/TTS 共用） |
| `[asr]` | ASR WebSocket 地址、`res_id`（注：`dialect_enabled` 当前未接线，见 §4） |
| `[llm]` | 星火 API 地址、Key、模型名 |
| `[tts]` | TTS WebSocket 地址、认证方式、音色 |
| `[memory]` | 记忆目录、MEMORY.md 索引 |
| `[agent]` | Prompt 路径、技能配置、最大工具轮数 |
| `[system]` | 调试模式、网络回退 |

详细参数说明见配置文件内注释。

---

## 技能系统

项目采用三层架构配置驱动：

### Action 操作类

| Action | 触发词 | 说明 |
|--------|--------|------|
| 💡 `action.light_on/off` | 打开灯、开灯 / 关灯 | 红外控制灯光（**模拟实现**，见下） |
| 🌡️ `action.ac_*` | 开空调、温度调高/低 | 红外控制空调（**模拟实现**） |
| 🔊 `action.vol_up/down` | 大声点、小声点 | 系统音量调节 |
| 😴 `action.sleep` | 休眠、待机、睡觉、休息 | 进入低功耗休眠 |
| ⚙️ `action.set_preference` | 记住、设置、偏好 | 持久化到 agent_prompt.md 可改动区 |

### Skill 智能任务类

| Skill | 依赖 MCP | 说明 |
|-------|----------|------|
| 📋 `skill.daily_briefing` | `get_weather`, `get_time` | LLM 综合生成今日简报 |

### MCP 数据工具类

| MCP | 数据源 | 说明 |
|-----|--------|------|
| 🌤️ `get_weather` | wttr.in HTTP API (JSON) | 实时天气 + 未来 3 天预报（温度、湿度、风力、天气状况） |
| 🕐 `get_time` | 系统 `date` 命令 | 本地时间，支持 15+ 时区（POSIX TZ 映射） |
| 📰 `get_news` | 需配置 API 地址 | 今日热点新闻 |

### 自定义扩展

**添加 Action**（硬件操作，keyword或LLM均可独立触发）：
编辑 `config/actions.json`，添加新条目：

```json
{
    "name": "示例操作",
    "description": "这是一个示例操作",
    "triggers": ["触发词1", "触发词2"],
    "action": "action.example",
    "category": "control"
}
```

**添加 Skill**（仅 LLM 触发，需回注）：
创建 `skills/新技能/SKILL.md`：

```markdown
---
name: new_skill                # 技能标识（英文，action 生成为 skill.new_skill）
description: 技能描述           # 供 LLM 阅读的摘要
category: utility              # 分类
priority: 5                    # 优先级（数字越大越优先）
tools:                         # 依赖的 MCP 工具列表
  - mcp.get_weather
---
# 技能正文
```

**添加 MCP 工具**（仅 LLM 触发）：
编辑 `config/mcp_tools.json`，添加工具定义。

---

## 项目结构

```
ai_assistant/
├── CMakeLists.txt                  # CMake 构建系统
├── README.md                       # 用户手册（本文件）
├── CLAUDE.md                       # AI 辅助开发文档
├── DEVELOPMENT_LOG.md              # 问题与解决方案记录
├── config/                         # 运行时配置
│   ├── assistant.conf              # 主配置
│   ├── agent_prompt.md             # Agent 全局 Prompt
│   ├── actions.json                # Action 定义（关键词或LLM均可独立触发）
│   ├── mcp_tools.json              # MCP 工具定义（参考 MCP inputSchema 格式）
│   └── wakeup.wav                  # 唤醒提示音
├── skills/                         # Skill 目录（SKILL.md）
│   └── daily_briefing/SKILL.md     # 今日简报
├── memory/                         # MEMORY.md 索引（偏好本体在 agent_prompt.md 可改动区）
├── include/assistant/              # 头文件
│   ├── core/                       # 核心框架
│   ├── audio/                      # 音频采集/播放/VAD
│   ├── kws/                        # KWS 唤醒引擎
│   ├── agent/                      # Agent 架构
│   ├── cloud/                      # 云端服务
│   ├── command/                    # 红外命令
│   └── sensitive/                  # 敏感词过滤
├── src/                            # 源文件（与 include 一一对应）
├── scripts/                        # 辅助脚本
│   ├── cross_compile.sh            # 主编译脚本
│   ├── build_tflite_arm.sh         # TFLite ARM 编译
│   ├── build_openssl_arm.sh        # OpenSSL ARM 编译
│   ├── deploy.sh                   # 部署脚本
│   ├── train_kws.py                # KWS 模型训练
│   └── mic_in_config.sh            # 声卡配置
├── models/                         # KWS 模型文件
├── third_party/                    # 第三方库
│   ├── tflite/                     # TensorFlow Lite ARM
│   ├── openssl/                    # OpenSSL ARM
│   └── kissfft/                    # 轻量 FFT
└── build/                          # 编译产物
```

---

## 编译与部署

### 编译模式

| 模式 | 命令 | 编译器 | GLIBC | 体积 | 用途 |
|------|------|--------|-------|------|------|
| **TFLite** ✅ | `cross_compile.sh release tflite` | Linaro 4.9.4 | ≤ 2.17 | ~5.3MB | 开发板运行 |
| 普通 ❌ | `cross_compile.sh release` | arm-linux-gnueabihf-g++ | ≥ 2.34 | ~1.7MB | 仅 PC 语法检查 |

### 完整流程

```bash
# Step 1: 工具链（需预先安装 Linaro GCC 4.9.4）
export PATH="/opt/arm-linux-gnueabihf/bin:$PATH"

# Step 2: 编译依赖库（首次执行，后续可跳过）
bash scripts/build_tflite_arm.sh         # → third_party/tflite/lib/libtensorflow-lite.a
bash scripts/build_openssl_arm.sh        # → third_party/openssl/lib/libssl.a, libcrypto.a
bash scripts/download_kissfft.sh         # → third_party/kissfft/

# Step 3: 编译应用
bash scripts/cross_compile.sh release tflite   # → build/ai_assistant

# Step 4: 验证 GLIBC 兼容
arm-linux-gnueabihf-readelf -a build/ai_assistant | grep GLIBC_ | sort -Vu
# 合格: GLIBC_2.4, 2.6, 2.16, 2.17
# 不合格: 出现 2.25/2.33/2.34 → 重编译加 tflite

# Step 5: 部署
bash scripts/deploy.sh 192.168.1.100
```

### 开发板部署目录

```
/ai_assistant/
├── ai_assistant              # 主程序 (5.3MB ARM ELF)
├── config/
│   ├── assistant.conf
│   ├── agent_prompt.md
│   ├── actions.json
│   ├── mcp_tools.json
│   └── wakeup.wav
├── skills/
│   └── daily_briefing/SKILL.md
├── models/
│   └── kws_model.tflite
├── lib/
│   ├── libstdc++.so.6
│   └── libgcc_s.so.1
└── mic_in_config.sh
```

### KWS 模型训练（可选）

```bash
pip install tensorflow librosa sounddevice scikit-learn
python scripts/train_kws.py --record --count 100    # 录制数据
python scripts/train_kws.py --train --quantize --test  # 训练+量化+验证
# → models/kws_model.tflite (Float16, ~120KB)
```

---

## 技术栈

| 层级 | 技术 |
|------|------|
| **语言** | C++14（GCC 4.9.4） |
| **构建** | CMake 3.16+，交叉编译 |
| **AI 推理** | TensorFlow Lite 2.14.0 |
| **FFT** | kissfft（仅头文件） |
| **加密** | OpenSSL 1.1.1d（静态链接） |
| **网络** | 自实现 TCP/TLS/HTTP/WebSocket (RFC 6455) |
| **ASR** | 讯飞语音听写流式版 WebAPI |
| **LLM** | 讯飞星火 Spark-X2（OpenAI 兼容） |
| **TTS** | 讯飞超拟人合成 / SparkChain SDK |
| **天气** | wttr.in 免费 HTTP API |
| **播放** | ALSA `snd_pcm_writei` DMA 直写（无中间 WAV / 子进程） |

---

## 常见问题

### 编译：找不到 `arm-linux-gnueabihf-g++`

安装 Linaro GCC 4.9.4 工具链：

```bash
wget https://releases.linaro.org/components/toolchain/binaries/4.9-2017.01/arm-linux-gnueabihf/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf.tar.xz
sudo tar -xf gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf.tar.xz -C /opt/
```

### 运行：`GLIBC_2.34 not found`

编译时没加 `tflite` 参数。重新用 `bash scripts/cross_compile.sh release tflite` 编译。

### 唤醒不灵敏

调低 `config/assistant.conf` 中 `[kws] threshold`（默认 0.97），注意过低会增加误唤醒。

### 如何添加新语音指令

编辑 `config/actions.json` 添加 Action（关键词触发 + LLM 调用），或新建 `skills/*/SKILL.md` 添加 Skill（仅 LLM 触发）。

### 调试模式

设置 `[system] debug_mode = true`，重启后打印所有 API 请求/响应、KWS 推理分数，保存 `asr_debug_input.pcm` 和 `tts_debug.pcm`。

---

## 许可证

MIT License — 仅供学习和研究使用。

---

<p align="center">
  <sub>Made with ❤️</sub>
</p>
