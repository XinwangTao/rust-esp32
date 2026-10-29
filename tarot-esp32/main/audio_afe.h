// 设备端音频前端：用 esp-sr 的 esp_afe_sr 把 AEC + 非线性降噪 + VAD + AGC 合成一条链。
// 替换原来手工串的 aec_create → ns_create 两段式，并额外拿到逐帧 VAD 结果，
// 通过上行包 flags bit0 送到 PC —— 这样 PC 端就不必再用能量阈值去猜「这是不是人声」。
//
// 输入：mic + 播放参考（最后一路是参考），16 kHz s16，每块 get_feed_chunksize 个采样。
// 输出：单通道增强音频 + vad_state，按 320 采样（20 ms）的节奏交给调用方。
//
// 初始化失败一律返回 false，调用方回退到 audio_aec + audio_ns 的老链路。
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 创建 AFE 实例；成功返回 true。失败只打日志，调用方继续用老链路。
bool audio_afe_start(void);

// 是否可用（start 成功且没被停掉）。
bool audio_afe_ready(void);

// 播放线程调用：把正在播放的 PCM 喂进参考环形缓冲。欠载时按静音处理。
void audio_afe_feed_ref(const int16_t *pcm, size_t count);

// 采集线程调用：交进来一帧麦克风样本，够一块就内部 feed+fetch。
// 返回 true 表示 samples 已被增强后的音频覆盖，speech 给出该帧的 VAD 结果；
// 返回 false 表示没数据（启动初期或失败），调用方应回退到老链路。
bool audio_afe_process(int16_t *samples, size_t count, bool *speech);

// 统计：已处理块数、判为语音的块数、fetch 失败次数、参考欠载采样数。
void audio_afe_stats(uint32_t *blocks, uint32_t *speech_blocks, uint32_t *failed,
                     uint32_t *ref_underflow);

// 每次 feed+fetch 的耗时：平均/峰值微秒。这块是 512 采样（32 ms），load = avg_us / 32000。
void audio_afe_timing(uint32_t *avg_us, uint32_t *max_us);

// AFE 要求的每块采样数（get_feed_chunksize），初始化失败返回 0。
int audio_afe_feed_chunk(void);

// 诊断：最近一次 feed / fetch 的返回码、是否已因连续失败被停用，以及库报告的块参数。
void audio_afe_diag(int *feed_ret, int *fetch_ret, int *disabled, int *feed_chunk,
                    int *fetch_chunk, int *rate, int *channels);

#ifdef __cplusplus
}
#endif
