// 设备端回声消除：把扬声器正在播放的 PCM 作为参考信号，从麦克风信号里减掉回声。
// 上行仍是 320 采样/20 ms，AEC 内部按 aec_get_chunksize() 的帧长（16k 下 512）重分块，
// 因此会引入约 32 ms 的固定延迟。初始化失败一律退化成直通，不影响现有链路。
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 创建 AEC 实例；成功返回 true。失败只打日志，调用方继续用原始麦克风数据。
bool audio_aec_start(void);

// AEC 是否真的在跑（失败时上行直通）。
bool audio_aec_ready(void);

// 播放端调用：把即将写入 I2S 的 PCM 拷进参考信号环形缓冲。
void audio_aec_feed_ref(const int16_t *pcm, size_t count);

// 采集端调用：原地对 count 个采样做回声消除（帧长不整除时内部缓冲，首帧输出补零）。
void audio_aec_filter(int16_t *samples, size_t count);

// 统计：已处理块数、ref 缓冲欠载/溢出次数，供 STAT 回包报告。
void audio_aec_stats(uint32_t *blocks, uint32_t *underflow, uint32_t *overflow, int *frame);

// 处理耗时：平均/峰值微秒。32 ms 一帧，load = avg_us / 32000。
void audio_aec_timing(uint32_t *avg_us, uint32_t *max_us);

#ifdef __cplusplus
}
#endif
