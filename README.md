# Agent Voice Assistant v2.3.1

> 🎙️ An offline/online hybrid Agent voice assistant system built on i.MX6ULL (ARM Cortex-A7)
>
> Local wake word + cloud brain + Agent architecture, giving embedded devices a voice

[![Platform](https://img.shields.io/badge/platform-i.MX6ULL%20ARM-blue)](https://www.nxp.com/products/processors-and-microcontrollers/arm-processors/i-mx-applications-processors/i-mx-6-processors/i-mx-6ull-single-core-processor-with-arm-cortex-a7:i.MX6ULL)
[![Language](https://img.shields.io/badge/language-C%2B%2B14-orange)](https://en.cppreference.com/w/cpp/14)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

---

## 📖 Table of Contents

- [Features](#features)
- [System Architecture](#system-architecture)
- [Technical Highlights](#technical-highlights)
- [Quick Start](#quick-start)
- [Configuration](#configuration)
- [Skill System](#skill-system)
- [Project Structure](#project-structure)
- [Build & Deployment](#build--deployment)
- [Tech Stack](#tech-stack)
- [FAQ](#faq)
- [License](#license)

---

## Features

### 🎯 Core Interaction

| Feature | Description |
|------|------|
| **Voice Wake-up** | Local TFLite KWS model detects the wake word "小九小九" (Xiaojiu), threshold 0.97, confirmed over 4 consecutive frames |
| **Wake-up Feedback** | Plays the `wakeup.wav` beep immediately after detection |
| **Continuous Conversation** | Keeps listening after a single wake-up; multi-turn dialog without re-waking |
| **Auto Sleep** | Auto-sleeps after 10 seconds of silence; say "休眠" (sleep) to sleep manually |

### ☁️ Cloud Intelligence

| Feature | Engine | Description |
|------|------|------|
| **Speech Recognition (ASR)** | iFlytek Chinese recognition large model | Multi-engine voting (primary + dialect engine), confidence-weighted |
| **Dialog Generation (LLM)** | iFlytek Spark-X2 | OpenAI-compatible API + native Function Calling, carries full conversation context |
| **Speech Synthesis (TTS)** | iFlytek hyper-realistic synthesis | Streaming pipeline: synthesis and playback run in parallel, eliminating first-sentence latency |

### 🤖 Agent

| Feature | Description |
|------|------|
| **Action** | Triggerable independently by keyword or LLM, executed directly without injecting back (lights/AC/volume/sleep/preferences) |
| **Skill** | LLM-triggered only, two-stage SKILL.md loading, always injected back (daily briefing) |
| **MCP** | LLM-triggered only, JSON-RPC 2.0 standard, fully visible, always injected back |
| **Short-term Memory** | Keeps the full conversation context within a wake cycle (`agent_core_->history_`) |
| **Long-term Memory** | Preferences persisted in the editable region of `agent_prompt.md` |

### 🏠 Smart Home Control

Control home devices through voice commands:

- 💡 **Lights**: turn on / turn off
- 🌡️ **AC**: on/off / temperature adjustment
- 🔊 **Volume**: turn up / turn down

### 📡 Real-time Data

- 🌤️ **Weather Forecast**: wttr.in API for current weather + 3-day forecast (temperature, humidity, wind, conditions)
- 🕐 **Time Query**: system local time + POSIX timezone mapping (supports 15+ timezones including `Asia/Shanghai`, `America/New_York`)
- 📋 **Daily Briefing**: LLM automatically synthesizes weather and time into a daily briefing

---

## System Architecture

### Overall Architecture

```
┌──────────────────────────────────────────────────────┐
│                   Hardware Layer                      │
│  i.MX6ULL (Cortex-A7) + WM8960 + Mic + Speaker       │
└──────────────────┬───────────────────────────────────┘
                   │
┌──────────────────▼───────────────────────────────────┐
│                 Audio Capture Layer                   │
│  ALSA AudioCapture (16kHz, mono, S16_LE)             │
│  ├─ Ring Buffer                                      │
│  └─ VAD (Voice Activity Detection)                   │
└──────────────────┬───────────────────────────────────┘
                   │
┌──────────────────▼───────────────────────────────────┐
│                Wake-word Detection Layer              │
│  KWS TFLite Engine                                   │
│  ├─ MFCC: pre-emphasis→framing→windowing→FFT→        │
│  │        Mel filtering→log→DCT                      │
│  └─ TFLite inference (10 frames × 40 dims →          │
│       softmax → wake word)                           │
└──────────────────┬───────────────────────────────────┘
                   │
┌──────────────────▼───────────────────────────────────┐
│                  Cloud Service Layer                  │
│  ┌─────────┐  ┌─────────┐  ┌───────────────────┐     │
│  │   ASR   │  │   LLM   │  │        TTS         │     │
│  │ WSS      │  │ OpenAI  │  │ streaming pipeline │     │
│  │ streaming│  │ Agent   │  │ (producer-consumer)│     │
│  │ multi-   │  │         │  │ aplay cmd playback │     │
│  │ engine   │  │         │  │                    │     │
│  │ voting   │  │         │  │                    │     │
│  └────┬────┘  └────┬────┘  └─────────┬─────────┘     │
│       │            │                 │                 │
│  ┌────▼────────────▼─────────────────▼──────────┐     │
│  │                  Network Layer                 │     │
│  │  TCP + TLS (OpenSSL) + WebSocket (RFC 6455)   │     │
│  │  + iFlytek HMAC-SHA256 signature auth         │     │
│  └──────────────────────────────────────────────┘     │
└──────────────────┬───────────────────────────────────┘
                   │
┌──────────────────▼───────────────────────────────────┐
│                  Agent Decision Layer                 │
│  ┌─────────────┐  ┌──────────────┐  ┌─────────────┐  │
│  │ Action match │  │  LLM Agent   │  │  Executor    │  │
│  │ keyword or   │  │ ≤3 tool calls│  │ action, no   │  │
│  │ LLM, either  │  │              │  │ injection    │  │
│  │ triggers it  │  │              │  │ back         │  │
│  └─────────────┘  └──────────────┘  └─────────────┘  │
│                          │                             │
│              ┌───────────▼──────────┐                  │
│              │   State Machine      │                  │
│              │ SLEEP→WAKEUP→LISTEN  │                  │
│              │ →PROCESS→SPEAK→loop  │                  │
│              └──────────────────────┘                  │
└──────────────────────────────────────────────────────┘
```

### State Machine

```
     ┌──────────────────────────────────┐
     │                                  │
     ▼                                  │
  ┌──────┐   wake word   ┌────────┐     │
  │ SLEEP │ ────────────→ │ WAKEUP │    │
  │ (low  │              │ (plays │     │
  │ power)│ ←─────────── │ beep)  │     │
  └──────┘  sleep/       └───┬────┘     │
     ↑      10s silence      │          │
     │                       ▼          │
     │                ┌──────────┐      │
     │                │ LISTENING│      │
     │                │ (VAD)    │      │
     │                └────┬─────┘      │
     │                     │ speech     │
     │                     │ ended      │
     │                     ▼            │
     │                ┌──────────┐      │
     │                │PROCESSING│      │
     │                │ ASR→Agent│      │
     │                │ →TTS     │      │
     │                └────┬─────┘      │
     │                     │            │
     │                     ▼            │
     │                ┌──────────┐      │
     └─────────────── │ SPEAKING │──────┘
        playback done │ (stream  │  continuous
                      │  TTS)    │  (back to
                      └──────────┘  LISTENING)
```

---

## Technical Highlights

### 1. Fully Offline Wake-up

No cloud dependency; the local KWS engine responds in milliseconds:

- **Complete MFCC Pipeline**: pre-emphasis → framing → Hamming windowing → kissfft FFT → Mel filter bank → log → DCT
- **Float16 Quantized Model**: 120KB, 99.8% accuracy on training validation
- **Python Training Toolchain**: `train_kws.py` — record → train → quantize → deploy in one go
- **4 Consecutive Frames Confirmation**: effectively prevents false wake-ups

### 2. Agent Architecture: Three-Layer Separation (action > skill > MCP)

```
User says "How's the weather?"
      │
      ▼
┌─────────────┐      ┌──────────┐      ┌─────────┐
│   action    │  >   │  skill   │  >   │   MCP   │
│ keyword/LLM │      │ LLM only │      │ LLM only│
│ either      │      │ always   │      │ always  │
│ triggers it │      │ injected │      │ injected│
│ no inject   │      │ back     │      │ back    │
│ hardware ops│      │ smart    │      │ data    │
│             │      │ tasks    │      │ tools   │
└──────┬──────┘      └────┬─────┘      └────┬────┘
       │ ✗                │ ✗              │
   (no action match) (no skill match)  ┌────▼──────┐
                                  │ MCP returns │
                                  │ 25°C,        │
                                  │ humidity 60% │
                                  └──────┬──────┘
                                         │
                                    ┌────▼─────┐
                                    │ LLM      │
                                    │ polish:  │
                                    │ "It's    │
                                    │ 25°C in  │
                                    │ Hangzhou" │
                                    └──────────┘
```

| Layer | Trigger | Injected Back into LLM | Config | Typical Use |
|------|----------|----------|------|----------|
| **action** | keyword or LLM, either independently | No | `actions.json` | Lights, AC, volume, sleep |
| **skill** | LLM only | Yes | `skills/*/SKILL.md` | Daily briefing (internally calls MCP) |
| **MCP** | LLM only | Yes | `mcp_tools.json` | Weather, time, news, preferences |

**Design advantages**:
- **LLM sees all tools**: action fully / skill only summary / MCP fully
- **Skill aligned with Agent**: folder + SKILL.md (YAML frontmatter), Phase 1 summary → Phase 2 full load
- **MCP follows JSON-RPC 2.0**: standard inputSchema format; HTTP type is pure configuration
- **Configuration-driven**: adding/removing actions or MCPs requires no C++ changes; adding a skill is just adding a folder

### 3. Streaming TTS Pipeline

Producer-consumer pattern makes synthesis and playback truly parallel:

```
Main thread (producer)              Background thread (consumer)
     │                            │
     ├─ synth sentence 1 → PCM enqueue → notified → aplay plays
     │   (notify)                 │
     ├─ synth sentence 2 → PCM enqueue    ├─ dequeue → aplay plays
     │   (parallel with playback!)        │   (while sentence 1 plays,
     └─ ...                               │    sentence 2 is already ready)
                                         └─ ...
```

Perceived first-sentence latency = synthesis time of the first sentence (not the whole text); subsequent sentences wait zero time.

### 4. Adaptive Multi-Engine ASR Voting

The primary engine (Mandarin) and a dialect engine recognize in parallel, weighted by confidence:

- Results agree → confidences add up
- Dialect engine has higher confidence → adopted first
- Primary engine runs independently → a dialect error never breaks core functionality

### 5. Embedded-Friendly

- **Single Binary**: 5.3MB ARM ELF, no runtime dependencies
- **No Database**: memory system is based on Markdown files
- **No Python/Node.js**: pure C++14, no GC pauses

---

## Quick Start

### Prerequisites

- ALIENTEK i.MX6ULL development board (Alpha/EMC version) + WM8960 audio
- Microphone + speaker/headphones
- Network connection (WiFi/Ethernet)
- iFlytek Open Platform account (ASR/LLM/TTS API credentials)

### 1. Build

```bash
cd ai_assistant

# First-time environment setup (only once)
bash scripts/build_tflite_arm.sh         # TFLite ARM library
bash scripts/build_openssl_arm.sh        # OpenSSL ARM static library
bash scripts/download_kissfft.sh         # kissfft lightweight FFT library

# Build
bash scripts/cross_compile.sh release tflite
```

> ⚠️ **Important**: The board uses EGLIBC 2.19, so the `tflite` flag is required to compile with Linaro GCC 4.9.4.

### 2. Configure

Edit `config/assistant.conf` and fill in your iFlytek credentials:

```ini
[cloud]
app_id = YOUR_APPID
api_key = YOUR_APIKey
api_secret = YOUR_APISecret

[llm]
api_key = YOUR_SPARK_KEY

[tts]
auth = YOUR_TTS_AUTH_TOKEN
```

### 3. Deploy

```bash
bash scripts/deploy.sh <BOARD_IP>
```

### 4. Run

```bash
# SSH into the board
cd /ai_assistant
sh mic_in_config.sh                       # Configure the sound card (once per reboot)
./ai_assistant -c config/assistant.conf   # Start the assistant
```

Once you hear the wake-up beep, say "小九小九" (Xiaojiu) to start talking!

---

## Configuration

All configuration lives in `config/assistant.conf`, organized in sections:

| Section | Description |
|------|------|
| `[audio]` | Record/playback devices, sample rate, VAD timeout |
| `[kws]` | Model path, detection threshold, wake word |
| `[cloud]` | iFlytek API credentials (shared by ASR/LLM/TTS) |
| `[asr]` | ASR WebSocket address, dialect engine, voting strategy |
| `[llm]` | iFlytek Spark API address, key, model name |
| `[tts]` | TTS WebSocket address, authentication method, voice |
| `[memory]` | Memory directory, MEMORY.md index |
| `[agent]` | Prompt path, skill config, max tool-call rounds |
| `[system]` | Debug mode, network fallback |

See the comments inside the config file for detailed parameter descriptions.

---

## Skill System

The project is configuration-driven with a three-layer architecture:

### Action — Direct Operations

| Action | Trigger Words | Description |
|--------|--------|------|
| 💡 `action.light_on/off` | 打开灯 / 开灯 / 关灯 | IR-controlled lighting |
| 🌡️ `action.ac_*` | 开空调 / 温度调高 / 温度调低 | IR-controlled AC |
| 🔊 `action.vol_up/down` | 大声点 / 小声点 | System volume control |
| 😴 `action.sleep` | 休眠 / 待机 / 睡觉 / 休息 | Enter low-power sleep |
| ⚙️ `action.set_preference` | 记住 / 设置 / 偏好 | Persisted to the editable region of agent_prompt.md |

### Skill — Smart Tasks

| Skill | MCP Dependencies | Description |
|-------|----------|------|
| 📋 `skill.daily_briefing` | `get_weather`, `get_time` | LLM synthesizes today's briefing |

### MCP — Data Tools

| MCP | Data Source | Description |
|-----|--------|------|
| 🌤️ `get_weather` | wttr.in HTTP API (JSON) | Live weather + 3-day forecast (temperature, humidity, wind, conditions) |
| 🕐 `get_time` | system `date` command | Local time, supports 15+ timezones (POSIX TZ mapping) |
| 📰 `get_news` | configurable API endpoint | Today's trending news |

### Custom Extensions

**Add an Action** (hardware operation; keyword or LLM can trigger it independently):
Edit `config/actions.json` and add a new entry:

```json
{
    "name": "Example action",
    "description": "This is an example action",
    "triggers": ["trigger word 1", "trigger word 2"],
    "action": "action.example",
    "category": "control"
}
```

**Add a Skill** (LLM-triggered only, injected back):
Create `skills/new-skill/SKILL.md`:

```markdown
---
name: new-skill
description: Skill description
tools:
  - mcp.get_weather
---
# Skill body
```

**Add an MCP tool** (LLM-triggered only, JSON-RPC 2.0):
Edit `config/mcp_tools.json` and add the tool definition.

---

## Project Structure

```
ai_assistant/
├── CMakeLists.txt                  # CMake build system
├── README.zh.md                    # User manual (Chinese)
├── README.md                       # User manual (English, this file)
├── config/                         # Runtime configuration
│   ├── assistant.conf              # Main config
│   ├── agent_prompt.md             # Global Agent prompt
│   ├── actions.json                # Action definitions (keyword or LLM, either triggers)
│   ├── mcp_tools.json              # MCP tool definitions (JSON-RPC 2.0)
│   └── wakeup.wav                  # Wake-up beep
├── skills/                         # Skill directory (SKILL.md)
│   └── daily_briefing/SKILL.md     # Daily briefing
├── memory/                         # MEMORY.md index (preferences live in the editable region of agent_prompt.md)
├── include/assistant/              # Headers
│   ├── core/                       # Core framework
│   ├── audio/                      # Audio capture/playback/VAD
│   ├── kws/                        # KWS wake-word engine
│   ├── agent/                      # Agent architecture
│   ├── cloud/                      # Cloud services
│   ├── command/                    # IR commands
│   └── sensitive/                  # Sensitive-word filtering
├── src/                            # Source files (mirrors include/)
├── scripts/                        # Helper scripts
│   ├── cross_compile.sh            # Main build script
│   ├── build_tflite_arm.sh         # TFLite ARM build
│   ├── build_openssl_arm.sh        # OpenSSL ARM build
│   ├── deploy.sh                   # Deploy script
│   ├── train_kws.py                # KWS model training
│   └── mic_in_config.sh            # Sound card config
├── models/                         # KWS model files
├── third_party/                    # Third-party libraries
│   ├── tflite/                     # TensorFlow Lite ARM
│   ├── openssl/                    # OpenSSL ARM
│   └── kissfft/                    # Lightweight FFT
└── build/                          # Build artifacts
```

---

## Build & Deployment

### Build Modes

| Mode | Command | Compiler | GLIBC | Size | Use |
|------|------|--------|-------|------|------|
| **TFLite** ✅ | `cross_compile.sh release tflite` | Linaro 4.9.4 | ≤ 2.17 | ~5.3MB | Runs on the board |
| Plain ❌ | `cross_compile.sh release` | arm-linux-gnueabihf-g++ | ≥ 2.34 | ~1.7MB | PC syntax check only |

### Full Flow

```bash
# Step 1: Toolchain (Linaro GCC 4.9.4 must be preinstalled)
export PATH="/opt/arm-linux-gnueabihf/bin:$PATH"

# Step 2: Build dependency libraries (first run only, can skip later)
bash scripts/build_tflite_arm.sh         # → third_party/tflite/lib/libtensorflow-lite.a
bash scripts/build_openssl_arm.sh        # → third_party/openssl/lib/libssl.a, libcrypto.a
bash scripts/download_kissfft.sh         # → third_party/kissfft/

# Step 3: Build the app
bash scripts/cross_compile.sh release tflite   # → build/ai_assistant

# Step 4: Verify GLIBC compatibility
arm-linux-gnueabihf-readelf -a build/ai_assistant | grep GLIBC_ | sort -Vu
# Pass: GLIBC_2.4, 2.6, 2.16, 2.17
# Fail: 2.25/2.33/2.34 present → rebuild with the tflite flag

# Step 5: Deploy
Deploy manually to the board directory, e.g. via NFS mount
```

### Board Deployment Layout

```
/ai_assistant/
├── ai_assistant              # Main program (5.3MB ARM ELF)
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

### KWS Model Training (Optional)

```bash
pip install tensorflow librosa sounddevice scikit-learn
python scripts/train_kws.py --record --count 100    # Record data
python scripts/train_kws.py --train --quantize --test  # Train + quantize + validate
# → models/kws_model.tflite (Float16, ~120KB)
```

---

## Tech Stack

| Layer | Technology |
|------|------|
| **Language** | C++14 (GCC 4.9.4) |
| **Build** | CMake 3.16+, cross-compilation |
| **AI Inference** | TensorFlow Lite 2.14.0 |
| **FFT** | kissfft (header-only) |
| **Crypto** | OpenSSL 1.1.1d (statically linked) |
| **Networking** | Self-implemented TCP/TLS/HTTP/WebSocket (RFC 6455) |
| **ASR** | iFlytek streaming speech-to-text WebAPI |
| **LLM** | iFlytek Spark-X2 (OpenAI-compatible) |
| **TTS** | iFlytek hyper-realistic synthesis / SparkChain SDK |
| **Weather** | wttr.in free HTTP API |
| **Playback** | ALSA aplay (WM8960 DMA-compatible solution) |

---

## FAQ

### Wake-up is not sensitive enough

Lower the `[kws] threshold` in `config/assistant.conf` (default 0.97). Be aware that going too low increases false wake-ups.

### How do I add a new voice command

Edit `config/actions.json` to add an Action (keyword-triggered + LLM-called), or create a new `skills/*/SKILL.md` to add a Skill (LLM-triggered only).

### Debug mode

Set `[system] debug_mode = true`, restart, and it will print all API requests/responses and KWS inference scores, and save `asr_debug_input.pcm` and `tts_debug.pcm`.

---

## License

MIT License — for learning and research purposes only.

---

<p align="center">
  <sub>Made with ❤️</sub>
</p>
