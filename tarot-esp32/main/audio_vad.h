// 设备端 VAD：在 AEC + NS 之后用 esp-sr 自带的 WebRTC VAD 判「这一帧是不是人声」。
//
// 为什么不用 AFE：esp-sr 2.4.7 + IDF 5.3.1 下 esp_afe_sr 的 fetch 恒定 ret=-1，
// 换了 AFE_TYPE_FD / AFE_TYPE_VC、调过 se 任务优先级、过过 afe_config_check、
// 块参数（512/512 @16k、2 通道）也都对上，输出侧依然拿不到数据（详见
// docs/archify/esp32s3-afe-integration_workflow.html 的验收记录）。
// 这里退回独立 API：输入是 AEC→NS 的产物，比 PC 端拿到的更干净，且已验证可靠。
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 创建 VAD 实例；失败返回 false，调用方退化为「没有 VAD 提示」的老行为。
bool audio_vad_start(void);

bool audio_vad_ready(void);

// 逐帧判语音：samples 是 20 ms（320 采样）的 16 kHz s16。返回 true 表示判为人声。
bool audio_vad_process(int16_t *samples, size_t count);

// 统计：已判帧数、判为语音的帧数。
void audio_vad_stats(uint32_t *frames, uint32_t *speech);

#ifdef __cplusplus
}
#endif
