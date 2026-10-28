# ESP32-S3 DevKitM-1 + INMP441 硬件接口

本文件描述当前音频发射端固件的目标硬件。旧版 `hardware.md` 保留为 ESP32-C3/PANBOPO 屏幕项目历史资料，不作为本固件的引脚真源。

## 模块与接口

| 功能 | DevKitM-1 信号 | GPIO/排针 | INMP441/外部模块 |
|---|---|---|---|
| I2S SCK | GPIO4 | J1-6 | `SCK/BCLK` |
| I2S WS | GPIO5 | J1-7 | `WS/LRCLK` |
| I2S data in | GPIO6 | J1-8 | `SD` |
| microphone power | 3V3 | J3-2 | `VDD` |
| common ground | GND | J1-1/J1-22/J3-1 | `GND` |
| 扬声器 I2S BCLK | GPIO15 | — | 功放 `BCLK` |
| 扬声器 I2S LRCLK | GPIO16 | — | 功放 `LRCLK/WS` |
| 扬声器 I2S DIN | GPIO7 | — | 功放 `DIN` |
| key 2 | GPIO8 | J1-10 | active-low input |
| key 3 | GPIO9 | J1-11 | active-low input |
| UART0 RX | GPIO44 / U0RXD | J3-5 | recovery/debug |
| UART0 TX | GPIO43 / U0TXD | J3-6 | recovery/debug |
| BOOT | GPIO0 | J1-2 | controlled download entry |

扬声器三根信号线的顺序不是按排针顺序排列的：GPIO7 是**数据**（DIN），15/16 才是时钟。这是 2026-10-28 在实机上用「发 PIN 控制帧逐个排列试 + 咪头回采」定出来的结论，`main/CMakeLists.txt` 的默认值即由此固定。原先把 GPIO7 当按键输入，已让位给扬声器。

## Audio format

- I2S master receive, Philips mode.
- 16 kHz, mono output frames.
- INMP441 data is read from 32-bit I2S slots and converted to signed 16-bit PCM before UDP framing.
- The module's `L/R` strap must be set according to its actual board marking; firmware does not assume the strap level.

## Acceptance testing

PC 端接收校验脚本位于 `tools/udp_audio_receiver.py`（仅标准库，Python 3.8+）：

```bash
# 只统计：包数、序号丢包、坏头、实际采样率
python tools/udp_audio_receiver.py --port 5000 --seconds 60

# 同时落盘原始 s16le PCM
python tools/udp_audio_receiver.py --port 5000 --seconds 60 --dump dump.pcm

# 回放：ffplay -f s16le -ar 16000 -ch_layout mono dump.pcm
```

`AUDIO_UDP_TARGET` 须指向运行脚本那台 PC 的局域网 IP 与 `--port` 端口。验收通过标准：

- 脚本报告 `sample rate   : 16000 Hz`，`bad packets   : 0`。
- `capture health` 接近 100%（UDP 无重传，偶发丢包可接受，持续大比例丢失需查 Wi-Fi 或队列丢帧日志）。
- `dump.pcm` 回放能听到环境声，拍手应有明显突变。

注意：Wi-Fi 与 UDP 目标为编译期注入（`option_env!`），设置 `AUDIO_WIFI_SSID`/`AUDIO_WIFI_PASSWORD`/`AUDIO_UDP_TARGET` 后必须重新 `idf.py build` 再 flash，凭据不要提交进仓库。

## 扬声器（I2S TX）验收

I2S1 做主控发送，16 kHz / 16-bit / mono，默认引脚 `GPIO15(BCLK)/16(LRCLK)/7(DIN)`，无独立功放使能脚（`SPEAKER_EN=-1`）。

- **开机自检音**：初始化成功后播 880 Hz 180 ms + 1320 Hz 140 ms。
- **`TONE` 控制帧**：向 `板子IP:5001` 发 4 字节 `TONE`，重播自检音（无需重启）。
- **`PIN:b,w,d` 控制帧**：运行时改 I2S TX 引脚并立即重播自检音，用于接线排查（已用它确定 DIN 在 GPIO7）。
- **下行音频**：向 `板子IP:5001` 发与上行同格式的 `AUD0` 帧，payload 为 s16le mono，板子直接写 I2S 播放。

已验证（2026-10-28，咪头回采）：自检音两段频率在 880/1320 Hz 各命中 8/7 帧；PC 推 2 s 1 kHz 正弦，回采命中 95 帧（能量 >3000）。扬声器初始化失败只告警，不影响上行采集。

```bash
# 触发自检音（Linux/macOS）
printf 'TONE' | nc -u -w1 <board-ip> 5001
```

## Constraints

- Do not drive GPIO0 low during ordinary startup.
- Keep GPIO43/44 available for UART0 recovery.
- Do not use GPIO45/46, GPIO19/20, or GPIO48 for this slice.
- Do not connect an unregulated Li-Po cell directly to `VCC_5V`.
- The desktop receiver application is not part of this firmware slice; the speaker (I2S TX) is.
