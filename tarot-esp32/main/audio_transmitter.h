#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_transmitter_start(void);

// OTA 期间暂停/恢复上行发送（帧仍采集，只是不发）
void audio_tx_set_paused(bool paused);

// 已发送帧数（开机自检用）
uint32_t audio_tx_sent_count(void);

#ifdef __cplusplus
}
#endif
