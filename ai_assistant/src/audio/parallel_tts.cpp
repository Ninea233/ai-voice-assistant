/*
 * parallel_tts.cpp
 * 并行 TTS 实现：增量切句 → 常驻线程池并行合成 → Reorder Buffer → 顺序 DMA 播放
 *
 * 线程池常驻（首次 BeginRound 创建，Shutdown 回收），因此：
 *   - 每轮只复位轮次状态，无线程创建/销毁开销；
 *   - 在途合成结果用轮次号（epoch）甄别并丢弃，防止跨轮污染；
 *   - CancelRound 不再 join（否则会被卡住的合成拖住），只置轮次失效。
 */

#include "assistant/audio/parallel_tts.h"

#include <chrono>
#include <iostream>
#include <utility>

static const char* kTag = "[ParallelTTS]";

/* 过短句子（≤2 字）与相邻句合并，避免碎片化合成 */
static const size_t kMergeCharThreshold = 2;

/* 单句合成等待预算：下一句连续序号超过该时长仍未就绪则跳过该句。
 * 播放门控保证进入播放时本轮生成已结束，正常情况下句子早已合成完毕，
 * 该预算只用于兜住「某句网络卡死」——避免整轮永远等不到序号推进。 */
static const int kSentenceWaitTimeoutSec = 5;
static const std::chrono::seconds kSentenceWaitTimeout(kSentenceWaitTimeoutSec);

/* =========================================================
 * UTF-8 辅助
 * ========================================================= */

/* 返回 UTF-8 首字节对应的字符字节长度；非法首字节返回 0 */
static size_t Utf8CharLen(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

/* 判断 pos 处是否为句末标点（支持 ASCII 与中文全角标点） */
static bool IsSentenceEndAt(const std::string& text, size_t pos) {
    unsigned char c = static_cast<unsigned char>(text[pos]);
    if (c == '\n' || c == '!' || c == '?' || c == ';') return true;

    if (pos + 2 < text.size()) {
        unsigned char c1 = static_cast<unsigned char>(text[pos]);
        unsigned char c2 = static_cast<unsigned char>(text[pos + 1]);
        unsigned char c3 = static_cast<unsigned char>(text[pos + 2]);
        if (c1 == 0xE3 && c2 == 0x80 && c3 == 0x82) return true; /* 。*/
        if (c1 == 0xEF && c2 == 0xBC && c3 == 0x81) return true; /* ！*/
        if (c1 == 0xEF && c2 == 0xBC && c3 == 0x9F) return true; /* ？*/
        if (c1 == 0xEF && c2 == 0xBC && c3 == 0x9B) return true; /* ；*/
    }
    return false;
}

static std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

size_t SentenceSplitter::CharCount(const std::string& utf8) {
    size_t count = 0;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        size_t len = Utf8CharLen(c);
        if (len == 0) { i++; continue; }
        if (len == 1) {
            if (c > 32 && c != 127) count++; /* 空白不计入 */
        } else {
            count++;
        }
        i += len;
    }
    return count;
}

/* =========================================================
 * SentenceSplitter
 * ========================================================= */

void SentenceSplitter::Reset() {
    buffer_.clear();
    pending_.clear();
}

void SentenceSplitter::EmitSentence(const std::string& sentence, const EmitCallback& emit) {
    if (sentence.empty()) return;

    /* 与前一句过短的残留合并 */
    std::string merged = pending_ + sentence;
    pending_.clear();

    if (CharCount(merged) <= kMergeCharThreshold) {
        /* 仍太短：挂起，等下一句一起输出 */
        pending_ = merged;
        return;
    }
    emit(merged);
}

void SentenceSplitter::ForceSplit(const EmitCallback& emit) {
    while (CharCount(buffer_) > max_chars_) {
        /* 定位第 max_chars_ 个字符的字节边界 */
        size_t pos = 0, count = 0;
        while (pos < buffer_.size() && count < max_chars_) {
            size_t len = Utf8CharLen(static_cast<unsigned char>(buffer_[pos]));
            if (len == 0 || pos + len > buffer_.size()) break; /* 等待完整字符 */
            pos += len;
            count++;
        }
        if (pos == 0) break;

        std::string chunk = buffer_.substr(0, pos);
        buffer_.erase(0, pos);
        EmitSentence(Trim(chunk), emit);
    }
}

void SentenceSplitter::Feed(const std::string& delta, const EmitCallback& emit) {
    if (delta.empty()) return;
    buffer_ += delta;

    /* 反复切出完整句子；遇到不完整 UTF-8 字符则停止，等后续增量补齐 */
    while (true) {
        size_t end_pos = std::string::npos;
        size_t end_len = 0;

        for (size_t i = 0; i < buffer_.size();) {
            size_t len = Utf8CharLen(static_cast<unsigned char>(buffer_[i]));
            if (len == 0 || i + len > buffer_.size()) break;
            if (IsSentenceEndAt(buffer_, i)) {
                end_pos = i;
                end_len = len;
                break;
            }
            i += len;
        }

        if (end_pos == std::string::npos) break;

        std::string sentence = buffer_.substr(0, end_pos);
        buffer_.erase(0, end_pos + end_len);
        EmitSentence(Trim(sentence), emit);
    }

    ForceSplit(emit);
}

void SentenceSplitter::Finish(const EmitCallback& emit) {
    ForceSplit(emit);

    std::string rest = Trim(buffer_);
    buffer_.clear();
    if (!rest.empty()) {
        EmitSentence(rest, emit);
    }
    /* 收尾：把仍然挂起的过短句单独输出 */
    if (!pending_.empty()) {
        std::string last;
        last.swap(pending_);
        emit(last);
    }
}

/* =========================================================
 * ParallelTTS
 * ========================================================= */

ParallelTTS::ParallelTTS() = default;

ParallelTTS::~ParallelTTS() {
    Shutdown();
}

void ParallelTTS::WakeAll() {
    cv_task_.notify_all();
    cv_play_.notify_all();
    cv_done_.notify_all();
}

void ParallelTTS::JoinAll() {
    /* 加锁保证幂等：FinishRound 与 Shutdown 可能并发回收同一批线程 */
    std::lock_guard<std::mutex> lock(join_mutex_);

    for (size_t i = 0; i < synth_threads_.size(); i++) {
        if (synth_threads_[i].joinable()) synth_threads_[i].join();
    }
    synth_threads_.clear();

    if (playback_thread_.joinable()) playback_thread_.join();
}

void ParallelTTS::EnsureStarted() {
    std::lock_guard<std::mutex> lock(join_mutex_);
    if (alive_.load()) return;

    size_t count = (thread_count_ > 0 ? thread_count_ : 1);

    /* 先置存活标志：工作线程的等待谓词会检查它，置晚了线程会立即退出 */
    alive_ = true;

    synth_threads_.reserve(count);
    for (size_t i = 0; i < count; i++) {
        synth_threads_.emplace_back(&ParallelTTS::SynthesizeLoop, this);
    }
    playback_thread_ = std::thread(&ParallelTTS::PlaybackLoop, this);

    std::cout << kTag << " 线程池常驻启动：" << count
              << " 个合成线程 + 1 个播放调度线程" << std::endl;
}

/* =========================================================
 * 轮次生命周期
 * ========================================================= */

void ParallelTTS::BeginRound() {
    EnsureStarted();

    splitter_.Reset();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        /* 递增轮次：上一轮仍在途的合成结果全部失效 */
        round_epoch_++;
        pending_tasks_.clear();
        results_.clear();
        held_sentence_.clear();
        next_index_ = 0;
        submit_index_ = 0;
        total_sentences_ = 0;
        playback_enabled_ = false;
        round_done_ = false;
        round_active_ = true;
    }
    WakeAll();
}

void ParallelTTS::FeedText(const std::string& delta) {
    splitter_.Feed(delta, [this](const std::string& sentence) {
        EmitSentence(sentence);
    });
}

void ParallelTTS::EmitSentence(const std::string& sentence) {
    std::string ready;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!round_active_ || sentence.empty()) return;

        /* 单句延迟提交：当前句暂存，上一句才真正入队。
         * 若本轮最终被判定为工具调用，最多只损失一句；常见的
         * 「一句寒暄 + 工具调用」形态下寒暄尚未入队，CancelRound 直接丢弃，
         * 不会向 TTS 发起任何请求。 */
        ready.swap(held_sentence_);
        held_sentence_ = sentence;
    }
    if (!ready.empty()) SubmitNext(ready);
}

void ParallelTTS::SubmitNext(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!round_active_) return;

        Task task;
        task.epoch = round_epoch_;
        task.index = submit_index_++;
        task.text = text;
        total_sentences_++;
        pending_tasks_.push_back(std::move(task));
    }
    cv_task_.notify_one();
}

void ParallelTTS::CancelRound() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        round_active_ = false;
        round_epoch_++;          /* 使在途合成结果失效 */
        pending_tasks_.clear();
        results_.clear();
        held_sentence_.clear();
        playback_enabled_ = false;
    }
    WakeAll();

    /* 若播放调度线程正阻塞在 DMA 播放中，drop 以解除阻塞；
     * 不 join —— 被卡住的合成线程留在下一轮复用（最坏损失一个并发额度）。 */
    if (playback_) playback_->Stop();
}

void ParallelTTS::FinishRound() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!round_active_) return; /* 本轮已被取消 */
    }

    /* 补齐最后一句（无标点的残留文本） */
    splitter_.Finish([this](const std::string& sentence) {
        EmitSentence(sentence);
    });

    /* 释放「单句延迟提交」策略暂存的最后一句 */
    std::string held;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        held.swap(held_sentence_);
    }
    if (!held.empty()) SubmitNext(held);

    bool has_audio = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        playback_enabled_ = true;
        has_audio = (total_sentences_ > 0);
    }
    WakeAll();

    if (!has_audio) {
        std::cout << kTag << " 本轮无音频内容" << std::endl;
        CancelRound();
        return;
    }

    {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_done_.wait(lock, [this]() { return round_done_ || !alive_.load(); });
    }

    std::cout << kTag << " 本轮完成，共 " << total_sentences_ << " 句" << std::endl;

    /* 轮次收尾：复位本轮状态，线程池保持常驻 */
    {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_tasks_.clear();
        results_.clear();
        held_sentence_.clear();
        next_index_ = 0;
        submit_index_ = 0;
        total_sentences_ = 0;
        playback_enabled_ = false;
        round_done_ = false;
        round_active_ = false;
    }
}

void ParallelTTS::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        alive_ = false;
        round_active_ = false;
        round_epoch_++;
        pending_tasks_.clear();
        results_.clear();
        held_sentence_.clear();
        playback_enabled_ = false;
    }
    WakeAll();

    if (playback_) playback_->Stop();
    JoinAll();
}

/* =========================================================
 * 工作线程
 * ========================================================= */

void ParallelTTS::SynthesizeLoop() {
    /* 线程私有 TTS 实例：audio_callback_ 为实例成员，共享会互相覆盖 */
    std::unique_ptr<TTSClient> tts;

    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_task_.wait(lock, [this]() {
                return !alive_.load() || (round_active_ && !pending_tasks_.empty());
            });
            if (!alive_.load()) return;
            task = std::move(pending_tasks_.front());
            pending_tasks_.pop_front();
        }

        if (!tts && tts_factory_) {
            tts = tts_factory_();
        }
        if (!tts) {
            OnResult(task.epoch, task.index, false, std::vector<int16_t>());
            continue;
        }

        std::vector<int16_t> pcm;
        tts->OnAudio([&pcm](const std::vector<int16_t>& audio) {
            pcm = audio;
        });
        bool ok = tts->Synthesize(task.text) && !pcm.empty();

        if (!ok) {
            std::cerr << kTag << " 合成失败，跳过: \"" << task.text.substr(0, 30)
                      << "\"" << std::endl;
        }
        /* 结果带上轮次号：轮次已切换时由 OnResult 丢弃 */
        OnResult(task.epoch, task.index, ok, std::move(pcm));
    }
}

void ParallelTTS::OnResult(size_t epoch, size_t index, bool success,
                           std::vector<int16_t> pcm) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        /* 常驻线程池必须过滤迟到结果：轮次已取消/切换则丢弃，
         * 否则上一轮被丢弃句子的音频会被当成新一轮的第 index 句播出去。 */
        if (!alive_.load() || epoch != round_epoch_ || !round_active_) {
            std::cout << kTag << " 丢弃过期合成结果（第 " << index << " 句）" << std::endl;
            return;
        }
        /* 该序号已被等待超时跳过 → 结果无处安放，直接丢弃（否则滞留到轮末） */
        if (index < next_index_) {
            std::cout << kTag << " 丢弃已跳过序号（第 " << index << " 句）" << std::endl;
            return;
        }
        Result r;
        r.pcm = std::move(pcm);
        r.success = success;
        results_[index] = std::move(r);
    }
    cv_play_.notify_one();
}

void ParallelTTS::PlaybackLoop() {
    while (true) {
        /* ① 等一轮开始且播放门控放行 */
        size_t epoch = 0;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_play_.wait(lock, [this]() {
                return !alive_.load() || (round_active_ && playback_enabled_);
            });
            if (!alive_.load()) return;
            epoch = round_epoch_;
        }

        /* ② 按序播放本轮，直到播完或被取消（轮次号变化） */
        bool cancelled = false;
        /* 单句等待预算：某句超过该时长仍未合成出来则跳过，避免整轮卡死。
         * 每换一个序号重置一次，因此预算是「每句」而不是「整轮」。 */
        auto deadline = std::chrono::steady_clock::now() + kSentenceWaitTimeout;
        while (true) {
            std::vector<int16_t> pcm;
            bool has_pcm = false;
            bool finished = false;

            {
                std::unique_lock<std::mutex> lock(mtx_);
                /* wait_until 的谓词版：返回 false 表示「到点且下一句仍未到」 */
                bool ready = cv_play_.wait_until(lock, deadline, [this, epoch]() {
                    if (!alive_.load() || round_epoch_ != epoch) return true;
                    return results_.count(next_index_) > 0 ||
                           next_index_ >= total_sentences_;
                });
                /* 本次等待已结束，为「下一个序号」重置预算 */
                deadline = std::chrono::steady_clock::now() + kSentenceWaitTimeout;

                if (!alive_.load()) return;
                if (round_epoch_ != epoch) { cancelled = true; break; }

                if (next_index_ >= total_sentences_) {
                    finished = true;
                } else if (!ready) {
                    /* 该句超时未响应：跳过并推进序号（与合成失败同语义），
                     * 后续句子照常播放，绝不让整轮无限等待。 */
                    std::cerr << kTag << " 第 " << next_index_ << " 句等待超过 "
                              << kSentenceWaitTimeoutSec << "s，跳过" << std::endl;
                    next_index_++;
                } else {
                    /* 只消费连续序号：只取当前序号，绝不越序播放 */
                    std::map<size_t, Result>::iterator it = results_.find(next_index_);
                    if (it == results_.end()) continue;

                    Result r = std::move(it->second);
                    results_.erase(it);
                    next_index_++;
                    pcm = std::move(r.pcm);
                    has_pcm = r.success && !pcm.empty();
                }
            }

            if (finished) break;
            if (has_pcm && playback_) playback_->Play(pcm);
        }

        if (cancelled) continue; /* 回到外层，等下一轮 */

        /* ③ 本轮播完：标记完成并回到外层等待 */
        {
            std::lock_guard<std::mutex> lock(mtx_);
            round_done_ = true;
            round_active_ = false;
        }
        cv_done_.notify_all();
    }
}
