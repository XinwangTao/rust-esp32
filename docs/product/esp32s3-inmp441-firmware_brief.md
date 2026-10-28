# ESP32-S3 INMP441 无线麦克风固件 Brief

状态：ESP32-S3 构建已通过；等待 INMP441 与 UDP 实机验收。

## 用户与目标

基于 `https://github.com/XinwangTao/rust-esp32.git` 的 ESP-IDF/Rust/C++ 混合结构，改造成 ESP32-S3 + 现成 INMP441 模块的无线麦克风发射端固件：采集单声道 I2S 音频，经 Wi-Fi UDP 发送，并保留按键、UART 恢复、OTA/局域网调试方向。

## 已验证基线

- 仓库当前主项目：`tarot-esp32`。
- 当前文档与构建基线：ESP32-C3、ESP-IDF 5.3.1、Rust `riscv32imc-esp-espidf`。
- 当前硬件代码：屏幕/塔罗 UI、I2S 播放 TX；现有 `GPIO8/6/5` 是 C3 项目的播放映射。
- 当前网络代码：Rust `EspWifi` AP + HTTP 服务，不是 UDP 音频传输。
- 本项目硬件候选映射：ESP32-S3 DevKitM-1，INMP441 `SCK/WS/SD` 候选 `GPIO4/5/6`，按键候选 `GPIO7/8/9`，UART0 `GPIO43/44`。

## In Scope（本批建议）

- 将目标芯片/构建配置切换到 ESP32-S3，确认目标工具链和 ESP-IDF 兼容性。
- 抽出与塔罗屏幕/UI 无关的音频固件入口，保留混合 Rust/C++ 架构可复用部分。
- 实现 INMP441 I2S RX：单声道、16 kHz、16 bit 的暂定采集配置。
- 实现 Wi-Fi STA 配置与 UDP 音频帧发送；SSID、密钥、接收端地址采用配置项，不提交真实凭据。
- 实现 GPIO7/8/9 按键事件抽象，不绑定未确认的业务动作。
- 保留 UART0 恢复接口；OTA/局域网调试先定义接口边界，按确认范围实现最小骨架。
- 增加构建、单元级协议/帧格式测试和无硬件条件下的静态验证。

## Out of Scope（本批不做）

- 不修改嘉立创EDA、原理图或 PCB。
- 不实现接收端固件和音频输出，除非另行确认。
- 不设计裸 ESP32-S3/INMP441 电路，不假定未确认的电源模块或 L/R 电平。
- 不提交 Wi-Fi 密码、设备密钥或生产 OTA 凭据。
- 不把现有塔罗屏幕 UI 作为音频产品功能继续扩展。

## 成功标准

- `idf.py build` 能针对 ESP32-S3 完成（需要本机 ESP-IDF 环境）。
- I2S RX 初始化参数、GPIO 映射和数据格式有明确日志/配置来源。
- 音频帧协议有版本、序号、采样率、样本格式和 payload 长度字段。
- UDP 发送失败不会阻塞采集任务；有丢帧/重连统计。
- 按键事件、UART 恢复接口和配置入口有测试或静态验证。
- 无硬件时能完成编译级/协议级验证；硬件采集和无线吞吐需实机验收。

## 主路径与关键失败

1. 启动配置与 NVS 初始化。
2. Wi-Fi STA 连接并取得 IP。
3. I2S RX 初始化 INMP441。
4. 采集任务读取 PCM 帧并放入有界队列。
5. UDP 发送任务封包、发送并统计结果。
6. 按键/恢复/调试任务处理控制事件。

关键失败：Wi-Fi 无法连接时限流重连并继续保护采集；UDP 接收端不可达时丢帧并记录统计；I2S 初始化/数据格式失败时停止发送并输出可诊断错误；OTA 校验/启动失败时保留回滚/有线恢复路径。

## 关键约束

- 目标板：ESP32-S3-DevKitM-1 标准模块；候选 I2S `GPIO4/5/6`、按键 `GPIO7/8/9`、UART0 `GPIO43/44`。
- 现有仓库目标为 ESP32-C3，必须先完成目标迁移，不得把 C3 专属配置当作 S3 配置。
- Rust/C++ 边界优先保持现有架构，除非音频驱动在 C++/ESP-IDF 更易验证。
- 配置与凭据使用示例/环境注入，不写入仓库。

## 参考与直接影响

- Espressif ESP32-S3-DevKitM-1 官方原理图：硬件排针和 UART/电源约束真源。
- `Sukecz/esp32-birdnet-mic`：Wi-Fi 音频、管理和 OTA 方向；不直接复制实现。
- `Inouye165/esp32s3-inmp441-project`：ESP32-S3 + INMP441 采集方向；不直接复制 GPIO。
- Espressif ESP-IDF OTA 示例：回滚和升级边界。
- 当前仓库 `tarot-esp32`：复用 ESP-IDF/Rust/C++ 构建骨架、NVS/Wi-Fi 初始化方式；淘汰屏幕/塔罗业务和 I2S TX 作为音频主链路。
