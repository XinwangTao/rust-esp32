#include "audio_ns.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ns.h"
#include "esp_timer.h"

namespace {
constexpr char TAG[] = "audio_ns";
constexpr int kSampleRate = 16000;
constexpr int kFrameMs = 10;  // esp_ns.h 注释写支持 10/20/30，但 2.4.7 实现在 esp_ns.c:29 断言只有 10
constexpr size_t kFrame = kSampleRate * kFrameMs / 1000;  // 160 采样，上行 320 拆两半
constexpr size_t kAlign = 16;

ns_handle_t s_ns = nullptr;
int16_t *s_out = nullptr;   // ns_process 的输出缓冲，算完再拷回原地

uint64_t s_us_total = 0;    // 累计耗时，只由采集任务写
uint32_t s_us_max = 0;
uint32_t s_frames = 0;
uint32_t s_skipped = 0;
volatile bool s_enabled = true;   // NS:off 后采集端整段跳过，上行直通 AEC 输出
}  // namespace

extern "C" bool audio_ns_start(void) {
    if (s_ns != nullptr) return true;

    s_ns = ns_create(kFrameMs);
    if (s_ns == nullptr) {
        ESP_LOGE(TAG, "ns_create failed; uplink keeps AEC output (no denoise)");
        return false;
    }
    // 320 字节的小缓冲放内部 RAM：NS 在采集任务里同步跑，走 PSRAM 只会拖慢
    s_out = static_cast<int16_t *>(
        heap_caps_aligned_alloc(kAlign, kFrame * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_out == nullptr) {
        ESP_LOGE(TAG, "scratch alloc failed; internal_free=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        ns_destroy(s_ns);
        s_ns = nullptr;
        return false;
    }
    s_us_total = 0;
    s_us_max = 0;
    s_frames = 0;
    s_skipped = 0;
    ESP_LOGI(TAG, "NS ready: %d Hz frame=%d (%d ms) webrtc", kSampleRate, static_cast<int>(kFrame),
             kFrameMs);
    return true;
}

extern "C" bool audio_ns_ready(void) { return s_ns != nullptr; }

extern "C" bool audio_ns_enabled(void) { return s_ns != nullptr && s_enabled; }

extern "C" void audio_ns_set_enabled(bool on) {
    if (s_enabled == on) return;
    s_enabled = on;
    // 重新打开时清掉峰值：关掉期间的 0 没意义，留着会让人误判 CPU 掉了
    s_us_max = 0;
    ESP_LOGI(TAG, "NS %s by control frame", on ? "on" : "off (uplink = AEC output)");
}

extern "C" void audio_ns_process(int16_t *samples, size_t count) {
    if (s_ns == nullptr || !s_enabled || samples == nullptr || count == 0) return;

    size_t i = 0;
    while (count - i >= kFrame) {
        const int64_t t_begin = esp_timer_get_time();
        ns_process(s_ns, samples + i, s_out);
        const int64_t elapsed_us = esp_timer_get_time() - t_begin;
        s_us_total += static_cast<uint64_t>(elapsed_us);
        if (static_cast<uint32_t>(elapsed_us) > s_us_max) {
            s_us_max = static_cast<uint32_t>(elapsed_us);
        }
        std::memcpy(samples + i, s_out, kFrame * sizeof(int16_t));
        ++s_frames;
        i += kFrame;
    }
    if (i < count) ++s_skipped;  // 不足一帧的尾巴：原样放过，不攒（上行永远是整帧）
}

extern "C" void audio_ns_stats(uint32_t *frames, uint32_t *skipped) {
    if (frames != nullptr) *frames = s_frames;
    if (skipped != nullptr) *skipped = s_skipped;
}

extern "C" void audio_ns_timing(uint32_t *avg_us, uint32_t *max_us) {
    if (avg_us != nullptr) *avg_us = s_frames ? static_cast<uint32_t>(s_us_total / s_frames) : 0;
    if (max_us != nullptr) *max_us = s_us_max;
}
