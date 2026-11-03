// Wi-Fi 拉取式 OTA：BLE 控制帧 "OTA" 触发 → 连接 Wi-Fi（BLE 模式下按需启动 STA）
// → HTTP 下载 .bin 流式写入空闲 OTA 分区 → 校验 → 切换启动分区 → 重启。
// 失败不切换分区，旧固件继续运行；新固件自检失败由 bootloader 自动回滚。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 启动 OTA 任务。返回 false 表示已有 OTA 在进行。
bool audio_ota_start(const char *url);

#ifdef __cplusplus
}
#endif
