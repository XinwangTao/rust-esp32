#include "audio_aec.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {
constexpr char TAG[] = "audio_aec";
constexpr int kSampleRate = 16000;
constexpr int kFilterLength = 4;      // 官方推荐值：越大越吃资源
constexpr size_t kAlign = 16;         // AEC 要求 16 字节对齐（内部按 SIMD 读写）
constexpr size_t kRefRing = 8192;     // 512 ms 参考信号，2 的幂才能用掩码取模
constexpr size_t kOutRing = 4096;     // 已消回声、尚未回填给上行的采样

aec_handle_t *s_aec = nullptr;
int s_frame = 0;

int16_t *s_mic = nullptr;  // 攒帧用的输入缓冲
int16_t *s_ref = nullptr;  // 一帧参考信号
int16_t *s_out = nullptr;  // 一帧消除结果
size_t s_mic_fill = 0;

int16_t *s_out_ring = nullptr;
size_t s_out_w = 0;
size_t s_out_r = 0;

int16_t *s_ref_ring = nullptr;
std::atomic<uint32_t> s_ref_w{0};
uint32_t s_ref_r = 0;  // 只由采集任务推进

std::atomic<uint32_t> s_blocks{0};
uint64_t s_proc_us_total = 0;   // 累计处理耗时，只由采集任务写
uint32_t s_proc_us_max = 0;
uint32_t s_underflow = 0;
uint32_t s_overflow = 0;

// 优先放 PSRAM，失败退回内部 RAM，再失败才算初始化失败
int16_t *alloc_samples(size_t samples) {
    const size_t bytes = samples * sizeof(int16_t);
    void *p = heap_caps_aligned_alloc(kAlign, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = heap_caps_aligned_alloc(kAlign, bytes, MALLOC_CAP_8BIT);
    }
    return static_cast<int16_t *>(p);
}

void release_all() {
    if (s_mic != nullptr) heap_caps_free(s_mic);
    if (s_ref != nullptr) heap_caps_free(s_ref);
    if (s_out != nullptr) heap_caps_free(s_out);
    if (s_out_ring != nullptr) heap_caps_free(s_out_ring);
    if (s_ref_ring != nullptr) heap_caps_free(s_ref_ring);
    s_mic = s_ref = s_out = s_out_ring = s_ref_ring = nullptr;
    if (s_aec != nullptr) {
        aec_destroy(s_aec);
        s_aec = nullptr;
    }
}

// 取一帧参考信号；没有播放（欠载）就喂静音，让 AEC 自然退化为直通
void load_ref_block(int16_t *dst) {
    const uint32_t w = s_ref_w.load(std::memory_order_acquire);
    const uint32_t avail = w - s_ref_r;  // 无符号减法，回绕安全
    const size_t frame = static_cast<size_t>(s_frame);
    if (avail < frame) {
        std::memset(dst, 0, frame * sizeof(int16_t));
        ++s_underflow;
        return;
    }
    if (avail > kRefRing - frame) {
        // 参考流与采集时钟长期漂移后落后太多：丢弃旧数据，对齐到最新
        s_ref_r = w - static_cast<uint32_t>(frame);
        ++s_overflow;
    }
    for (size_t i = 0; i < frame; ++i) {
        dst[i] = s_ref_ring[(s_ref_r + i) & (kRefRing - 1)];
    }
    s_ref_r += static_cast<uint32_t>(frame);
}
}  // namespace

extern "C" bool audio_aec_start(void) {
    if (s_aec != nullptr) return true;

    aec_config_t cfg{};
    cfg.mic_num = 1;
    cfg.ref_num = 1;
    cfg.out_num = 1;
    cfg.filter_length = kFilterLength;
    cfg.sample_rate = kSampleRate;
    cfg.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;  // 大缓冲走 PSRAM
    cfg.mode = AEC_MODE_FD_LOW_COST;                 // 官方推荐的全双工默认档
    cfg.nlp_level = AEC_NLP_LEVEL_AGGR;              // 官方默认档，显式写出来免得随版本变
    s_aec = aec_create_from_config(&cfg);
    if (s_aec == nullptr) {
        s_aec = aec_create(kSampleRate, kFilterLength, 1, AEC_MODE_FD_LOW_COST);
    }
    if (s_aec == nullptr) {
        ESP_LOGE(TAG, "aec_create failed; uplink stays raw (no echo cancellation)");
        return false;
    }

    s_frame = aec_get_chunksize(s_aec);
    if (s_frame <= 0 || static_cast<size_t>(s_frame) > kOutRing) {
        ESP_LOGE(TAG, "unexpected aec frame size %d", s_frame);
        release_all();
        return false;
    }

    const size_t frame = static_cast<size_t>(s_frame);
    s_mic = alloc_samples(frame);
    s_ref = alloc_samples(frame);
    s_out = alloc_samples(frame);
    s_out_ring = alloc_samples(kOutRing);
    s_ref_ring = alloc_samples(kRefRing);
    if (s_mic == nullptr || s_ref == nullptr || s_out == nullptr || s_out_ring == nullptr ||
        s_ref_ring == nullptr) {
        ESP_LOGE(TAG, "buffer alloc failed; psram_free=%u internal_free=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        release_all();
        return false;
    }
    s_mic_fill = 0;
    s_out_w = s_out_r = 0;
    s_ref_w.store(0, std::memory_order_relaxed);
    s_ref_r = 0;

    ESP_LOGI(TAG, "AEC ready: %d Hz frame=%d (%d ms) mode=%s nlp=%s", kSampleRate,
             s_frame, s_frame * 1000 / kSampleRate, aec_get_mode_string(cfg.mode),
             aec_get_nlp_string(cfg.nlp_level));
    return true;
}

extern "C" bool audio_aec_ready(void) { return s_aec != nullptr; }

extern "C" void audio_aec_feed_ref(const int16_t *pcm, size_t count) {
    if (s_ref_ring == nullptr || pcm == nullptr || count == 0) return;
    uint32_t w = s_ref_w.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        s_ref_ring[(w + i) & (kRefRing - 1)] = pcm[i];
    }
    s_ref_w.store(w + static_cast<uint32_t>(count), std::memory_order_release);
}

extern "C" void audio_aec_filter(int16_t *samples, size_t count) {
    if (s_aec == nullptr || samples == nullptr || count == 0) return;
    const size_t frame = static_cast<size_t>(s_frame);

    // 1) 攒够一帧就消一次，结果压进输出环
    size_t i = 0;
    while (i < count) {
        size_t take = frame - s_mic_fill;
        if (take > count - i) take = count - i;
        std::memcpy(s_mic + s_mic_fill, samples + i, take * sizeof(int16_t));
        s_mic_fill += take;
        i += take;
        if (s_mic_fill == frame) {
            load_ref_block(s_ref);
            const int64_t t_begin = esp_timer_get_time();
            aec_process(s_aec, s_mic, s_ref, s_out);
            const int64_t elapsed_us = esp_timer_get_time() - t_begin;
            s_proc_us_total += static_cast<uint64_t>(elapsed_us);
            if (static_cast<uint32_t>(elapsed_us) > s_proc_us_max) {
                s_proc_us_max = static_cast<uint32_t>(elapsed_us);
            }
            s_blocks.fetch_add(1, std::memory_order_relaxed);
            for (size_t k = 0; k < frame; ++k) {
                s_out_ring[(s_out_w + k) & (kOutRing - 1)] = s_out[k];
            }
            s_out_w += frame;
            s_mic_fill = 0;
        }
    }

    // 2) 回填 count 个；输出环还空着时补零（开头 32 ms 必然欠载一次）
    for (size_t k = 0; k < count; ++k) {
        if (s_out_r != s_out_w) {
            samples[k] = s_out_ring[s_out_r & (kOutRing - 1)];
            ++s_out_r;
        } else {
            samples[k] = 0;
        }
    }
}

extern "C" void audio_aec_timing(uint32_t *avg_us, uint32_t *max_us) {
    const uint32_t blocks = s_blocks.load(std::memory_order_relaxed);
    if (avg_us != nullptr) {
        *avg_us = blocks ? static_cast<uint32_t>(s_proc_us_total / blocks) : 0;
    }
    if (max_us != nullptr) *max_us = s_proc_us_max;
}

extern "C" void audio_aec_stats(uint32_t *blocks, uint32_t *underflow, uint32_t *overflow,
                                int *frame) {
    if (blocks != nullptr) *blocks = s_blocks.load(std::memory_order_relaxed);
    if (underflow != nullptr) *underflow = s_underflow;
    if (overflow != nullptr) *overflow = s_overflow;
    if (frame != nullptr) *frame = s_frame;
}
