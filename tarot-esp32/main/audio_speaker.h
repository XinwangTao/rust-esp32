#pragma once
// 扬声器启动：I2S TX 初始化 + 开机自检音 + UDP 下行播放任务。
// 任一环节失败仅告警，不影响麦克风上行。
#include <cstddef>
#include <cstdint>
#ifdef __cplusplus
extern "C" {
#endif
void speaker_start(void);

// 只初始化 I2S 播放硬件（含开机自检音），不起 UDP 下行监听任务。
// BLE 模式下没有网络栈，起那个任务会让 lwIP 断言并无限重启；
// 播放数据改由 BLE 的 RX 特征喂进 tx_write。
void speaker_start_i2s_only(void);

// 把 PCM 推给扬声器（阻塞写，同时喂养 AEC 参考）。
// UDP 下行任务内部也走这个；BLE 模式由 RX 特征解出 PCM 后直接调用。
void speaker_write(const int16_t *samples, size_t count);
#ifdef __cplusplus
}
#endif
