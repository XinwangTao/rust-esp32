#include "audio_vad.h"

#include "esp_log.h"
#include "esp_vad.h"

namespace {
constexpr char TAG[] = "audio_vad";
constexpr int kSampleRate = 16000;
constexpr int kFrameMs = 20;   // 与上行一帧（320 采样）一致

vad_handle_t s_vad = nullptr;
volatile uint32_t s_frames = 0;
volatile uint32_t s_speech = 0;
}  // namespace

extern "C" bool audio_vad_start(void) {
    if (s_vad != nullptr) return true;
    // VAD_MODE_1（aggressive）：宁可漏一点，也别让风扇声起句。
    // 远端小声靠前面的 NS/AEC 保住信噪比，不靠把 VAD 放宽。
    s_vad = vad_create(VAD_MODE_1);
    if (s_vad == nullptr) {
        ESP_LOGE(TAG, "vad_create failed; uplink keeps 0 flags (pc falls back to energy vad)");
        return false;
    }
    s_frames = 0;
    s_speech = 0;
    ESP_LOGI(TAG, "VAD ready: %d Hz, %d ms frames (webrtc, aggressive)", kSampleRate, kFrameMs);
    return true;
}

extern "C" bool audio_vad_ready(void) { return s_vad != nullptr; }

extern "C" bool audio_vad_process(int16_t *samples, size_t count) {
    if (s_vad == nullptr || samples == nullptr) return false;
    // vad_process 一次只吃 10/20/30 ms，这里一帧正好 20 ms；多出来的尾巴原样忽略
    if (count < static_cast<size_t>(kSampleRate * kFrameMs / 1000)) return false;
    const bool speech = vad_process(s_vad, samples, kSampleRate, kFrameMs) == VAD_SPEECH;
    ++s_frames;
    if (speech) ++s_speech;
    return speech;
}

extern "C" void audio_vad_stats(uint32_t *frames, uint32_t *speech) {
    if (frames != nullptr) *frames = s_frames;
    if (speech != nullptr) *speech = s_speech;
}
