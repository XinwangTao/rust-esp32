// 设备端降噪：用 esp-sr 自带的 WebRTC NS 压稳态环境噪声（风扇/空调/风道）。
// 必须放在 AEC 之后：AEC 要拿原始 mic 和播放参考做自适应，降噪放前面会污染它的参考路径。
// NS 原地处理 10 ms（160 采样）一帧（库只支持 10 ms），上行的 320 采样拆成两个子帧跑，
// 不改变 20 ms 的上行节奏、不引入额外延迟。初始化失败一律退化成直通。
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 创建 NS 实例；成功返回 true。失败只打日志，调用方继续用 AEC 的输出。
bool audio_ns_start(void);

// NS 是否真的在跑（失败时上行直通）。
bool audio_ns_ready(void);

// 采集端调用：原地对 count 个采样降噪；不足一帧的尾巴原样放过。
void audio_ns_process(int16_t *samples, size_t count);

// 统计：已处理帧数、不足一帧而跳过的次数，供 STAT 回包报告。
void audio_ns_stats(uint32_t *frames, uint32_t *skipped);

// 处理耗时：平均/峰值微秒（每帧 160 采样 / 10 ms，load = avg_us / 10000）。
void audio_ns_timing(uint32_t *avg_us, uint32_t *max_us);

// 运行期旁路：关掉后 process 直接返回，上行变成 AEC 输出直通。
// 只为人做 A/B 对照（判断语音是被降噪压了还是被 VAD 阈值挡了），不随底噪自动切换。
void audio_ns_set_enabled(bool on);

// 当前是否真的在降噪（实例创建成功且没被关掉）。STAT 回包的 ns= 用这个。
bool audio_ns_enabled(void);

#ifdef __cplusplus
}
#endif
