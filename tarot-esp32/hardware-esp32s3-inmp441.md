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
| key 1 | GPIO7 | J1-9 | active-low input |
| key 2 | GPIO8 | J1-10 | active-low input |
| key 3 | GPIO9 | J1-11 | active-low input |
| UART0 RX | GPIO44 / U0RXD | J3-5 | recovery/debug |
| UART0 TX | GPIO43 / U0TXD | J3-6 | recovery/debug |
| BOOT | GPIO0 | J1-2 | controlled download entry |

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

## Constraints

- Do not drive GPIO0 low during ordinary startup.
- Keep GPIO43/44 available for UART0 recovery.
- Do not use GPIO45/46, GPIO19/20, or GPIO48 for this slice.
- Do not connect an unregulated Li-Po cell directly to `VCC_5V`.
- The receiver and audio output module are not part of this firmware slice.
