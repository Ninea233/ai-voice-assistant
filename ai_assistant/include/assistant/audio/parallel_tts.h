/*
 * parallel_tts.h
 * 并行 TTS：多句并行合成 + 按句序顺序播放
 *
 * 动机：串行流水线「合成一句→播一句」会把 TTS 合成延迟逐句累加。
 * 合成是网络 I/O 密集型（WebSocket 请求讯飞），可多线程并发：
 *
 *   LLM 流式文本 → 增量切句 → 任务队列 → N 个合成线程（乱序完成）
 *                                              ↓
 *                        播放调度线程 ← Reorder Buffer（按序号暂存）
 *                        （只播连续序号，绝不越序）
 *
 * 线程池**常驻**：首次 BeginRound 时创建，之后每轮只复位轮次状态，
 * 直到 Shutdown() 才回收——避免每轮 4 条线程的创建/销毁开销。
 *
 * 常驻带来的两个必要机制：
 *   1. **轮次（epoch）**：取消/换轮后在途合成结果会被丢弃，绝不污染下一轮
 *      （每轮创建线程时靠 join 兜底，常驻后必须显式判代）；
 *   2. **单句延迟提交**：切出的句子先暂存一句，确认本轮不是工具调用后才入队，
 *      使「工具调用轮不发起任何 TTS 请求」。
 */

#ifndef AI_ASSISTANT_PARALLEL_TTS_H
#define AI_ASSISTANT_PARALLEL_TTS_H

#include "assistant/audio/audio_playback.h"
#include "assistant/cloud/tts_client.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* 增量句子切分器：边接收 LLM 增量文本边切出完整句子。
 * UTF-8 安全（中文标点占 3 字节），并把过短句与相邻句合并，
 * 超长无标点文本按字数强制切分。 */
class SentenceSplitter {
public:
    using EmitCallback = std::function<void(const std::string&)>;

    /* 追加一段增量文本，输出其中已完整的句子 */
    void Feed(const std::string& delta, const EmitCallback& emit);

    /* 文本结束：输出残留内容 */
    void Finish(const EmitCallback& emit);

    /* 清空内部状态 */
    void Reset();

    /* 统计 UTF-8 字符数（连续字节计 1，空白 ASCII 不计） */
    static size_t CharCount(const std::string& utf8);

private:
    /* 输出一句（含过短句合并逻辑） */
    void EmitSentence(const std::string& sentence, const EmitCallback& emit);

    /* 无标点超长时按字数强制切分 */
    void ForceSplit(const EmitCallback& emit);

    std::string buffer_;       /* 尚未成句的累积文本 */
    std::string pending_;      /* 过短、待与下一句合并的句子 */
    size_t max_chars_ = 100;   /* 单句字数上限，防止合成超时 */
};

/* 并行 TTS 调度器：对上层只暴露「一轮」的生命周期方法 */
class ParallelTTS {
public:
    /* 为每个合成线程创建独立 TTSClient（回调为实例成员，不可共享） */
    using TTSFactory = std::function<std::unique_ptr<TTSClient>()>;

    ParallelTTS();
    ~ParallelTTS();

    void SetPlayback(AudioPlayback* playback) { playback_ = playback; }
    void SetTTSFactory(TTSFactory factory) { tts_factory_ = std::move(factory); }
    void SetThreadCount(size_t count) { thread_count_ = (count > 0 ? count : 1); }

    /* 开始一轮：复位轮次状态并唤醒常驻线程（首次调用时创建线程池，播放门控关闭） */
    void BeginRound();

    /* 增量喂入本轮文本（内部切句，并按「单句延迟提交」策略入队） */
    void FeedText(const std::string& delta);

    /* 本轮不需要播报（工具调用轮 / 无内容）：丢弃待合成与迟到结果并停止播放，
     * 线程保持常驻，立即返回（不等待在途合成）。 */
    void CancelRound();

    /* 本轮结束：提交残句、放行播放并等待全部播完（线程保持常驻） */
    void FinishRound();

    /* 回收线程池（进程退出前调用） */
    void Shutdown();

private:
    struct Task {
        size_t epoch;          /* 所属轮次，用于丢弃迟到结果 */
        size_t index;          /* 句序号 */
        std::string text;
    };

    struct Result {
        std::vector<int16_t> pcm;
        bool success;
    };

    /* 懒创建常驻线程池（合成线程 + 播放调度线程） */
    void EnsureStarted();

    /* 合成线程主体：抢任务 → 用本线程私有 TTSClient 合成 → 回传结果 */
    void SynthesizeLoop();

    /* 播放调度线程主体：跨轮存活，每轮只消费连续序号并按序 DMA 播放 */
    void PlaybackLoop();

    /* 提交一个句子任务（序号由 submit_index_ 递增分配） */
    void SubmitNext(const std::string& text);

    /* 切出一句：暂存当前句、提交上一句（单句延迟提交，见文件头说明） */
    void EmitSentence(const std::string& sentence);

    /* 合成线程回传结果（乱序到达；轮次已切换则丢弃） */
    void OnResult(size_t epoch, size_t index, bool success, std::vector<int16_t> pcm);

    void WakeAll();
    void JoinAll();

    AudioPlayback* playback_ = nullptr;
    TTSFactory tts_factory_;
    size_t thread_count_ = 3;   /* 受讯飞并发配额约束，默认 3（见 CODE.md §12.12） */

    std::vector<std::thread> synth_threads_;
    std::thread playback_thread_;
    /* 线程创建/回收串行化（EnsureStarted 与 JoinAll 互斥） */
    std::mutex join_mutex_;
    /* 线程池是否存活（常驻标志）：false 时全部工作线程退出 */
    std::atomic<bool> alive_{false};

    std::mutex mtx_;
    std::condition_variable cv_task_;   /* 新任务 → 唤醒合成线程 */
    std::condition_variable cv_play_;   /* 新结果/放行播放/换轮 → 唤醒播放调度 */
    std::condition_variable cv_done_;   /* 本轮播放完成 → 唤醒等待者 */

    std::deque<Task> pending_tasks_;    /* 待合成任务队列（仅本轮的） */
    std::map<size_t, Result> results_;  /* Reorder Buffer：序号 → 合成结果 */

    size_t round_epoch_ = 0;            /* 轮次号：每次 Begin/Cancel 递增 */
    size_t next_index_ = 0;             /* 下一个待播放序号（单调递增） */
    size_t submit_index_ = 0;           /* 下一个待提交序号 */
    size_t total_sentences_ = 0;        /* 本轮已提交句子数 */
    bool round_active_ = false;         /* 本轮是否有效（取消后为 false） */
    bool playback_enabled_ = false;     /* 播放门控：FinishRound 才放行 */
    bool round_done_ = false;           /* 本轮已播完 */
    std::string held_sentence_;         /* 单句延迟提交：暂存的待提交句子 */

    SentenceSplitter splitter_;
};

#endif /* AI_ASSISTANT_PARALLEL_TTS_H */
