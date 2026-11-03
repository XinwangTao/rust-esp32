# tarot-esp32 OTA 简报（Wi-Fi 拉取式）

日期：2026-11-02 ｜ 状态：**已实现并实机验收**（含回滚测试） ｜ 图：`docs/archify/esp32s3-ota_workflow.html` + `.json`

## 目标
板子不拆线即可升级固件；失败不影响旧固件运行。

## Scope
- 分区表：factory 4MB → `ota_0` + `ota_1` 各 4MB（16MB flash），spiffs 缩到约 7.5MB（当前音频资源在 spiffs，需确认剩余够用）
- 触发：BLE 控制帧 `OTA`（新增；复用现有通知回包）
- 下载：板端 `esp_http_client` 从 PC 的 HTTP 地址拉 `.bin`（地址编译期来自 `audio.env` 的 `OTA_URL`，与现有 Wi-Fi 凭据同一机制）
- 写入：`esp_ota_write` 流式写 + `esp_ota_end` 校验（镜像格式/长度）
- 切换：`esp_ota_set_boot_partition` → 回 `OTA ok` → `esp_restart`
- 回滚：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`；新固件启动自检（BLE 广播成功 + 麦克风首帧 + PSRAM 可用）通过后 `esp_ota_mark_app_valid_cancel_rollback()`；未标记则下次启动自动回滚
- 失败出口：Wi-Fi 连不上 / HTTP 失败 / 校验错 → 不切换分区，通知回 `OTA fail:<reason>`

## Out of Scope
- BLE 推送式 OTA（分片协议）
- 签名/加密校验（LAN 内，先用镜像校验而非 secure boot）
- App 内 OTA UI（先用 PC 脚本/ADB 触发；后续可加）

## 约束/风险
- BLE 模式下板端未初始化 Wi-Fi：OTA 时才启动 STA（与 BLE 共存）；下载期间上行音频可能掉帧（默认暂停上行）
- 新固件必须保持向后兼容的 GATT 服务才能被 App 再次连上
- 首次烧写分区表需要一次 USB（bootloader/分区表不能靠 OTA 自己改）

## 待确认
1. 触发：BLE `OTA` + 编译期 `OTA_URL`（推荐，简单）还是 `OTA <url>`（需把控制帧缓冲从 32B 扩到 256B）
2. OTA 下载期间：暂停上行音频（推荐）还是保持
3. 自检项：BLE+麦克风+PSRAM（推荐）还是仅“启动成功”
