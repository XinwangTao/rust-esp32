// BLE 音频上行：把 AEC→NS→VAD 之后的 20 ms 帧用 IMA ADPCM 压到 1/4，
// 通过 GATT 通知发给 PC。用来在**没有 Wi-Fi 路由器**的场合当无线麦。
//
// 为什么必须压缩：原始 16 kHz × 16 bit = 32 KB/s（256 kbps），BLE 直传会踩着
// 带宽上限跑，一次重传就掉音；ADPCM 4:1 后 8 KB/s（64 kbps），留 4 倍余量。
// 为什么每帧独立编码：ADPCM 是差分编码，跨帧预测一旦丢包会持续失真；
// 每帧重置预测器与步长，丢一包只坏 20 ms。
//
// 帧格式（166 字节）：seq_lo seq_hi flags pred_lo pred_hi index + 160 字节 ADPCM
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 起 NimBLE 并开始广播。失败返回 false，调用方回退到 UDP。
bool audio_ble_start(void);

bool audio_ble_ready(void);

// PC 是否已连接并订阅了通知特征。
bool audio_ble_connected(void);

// 编码并发送一帧（20 ms，320 采样）。未连接时直接丢弃并计数。
void audio_ble_send(const int16_t *samples, size_t count, uint8_t flags);

// 统计：已发帧数、因未连接/缓冲不足丢掉的帧数、协商到的 MTU。
void audio_ble_stats(uint32_t *sent, uint32_t *dropped, uint16_t *mtu);

#ifdef __cplusplus
}
#endif
