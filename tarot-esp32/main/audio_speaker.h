#pragma once
// 扬声器启动：I2S TX 初始化 + 开机自检音 + UDP 下行播放任务。
// 任一环节失败仅告警，不影响麦克风上行。
#ifdef __cplusplus
extern "C" {
#endif
void speaker_start(void);
#ifdef __cplusplus
}
#endif
