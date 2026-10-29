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

## 内存与 PSRAM（2026-10-29 实测）

esptool 读芯片特征时报告：`Chip is ESP32-S3 (QFN56) (revision v0.2)` /
`Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)` / 16MB Flash
—— 即 **封装内 8MB PSRAM 是真实存在的**（对应 WROOM-1-N16R8 级别模组）。

2026-10-29 已在 `sdkconfig.audio-s3` 里开启 PSRAM（`CONFIG_SPIRAM` / 八线 `SPIRAM_MODE_OCT`
/ 80 MHz / `SPIRAM_USE_MALLOC` / `SPIRAM_MEMTEST=y`），固件启动即初始化。

实测（向 `板子IP:5001` 发 `STAT` 控制帧读回，2026-10-29）：

```
STAT heap=8598988 psram_free=8384704 psram_total=8388608 learned=yes target=192.168.237.7:5000
```

`psram_total=8388608` 即 8 MB 全部识别；`psram_free` 只少 3904 B（运行时自身开销），
说明目前还没有大块从 PSRAM 分配——这符合预期，esp-sr 的 AEC 尚未接入。
这一点直接影响设备端回声消除的可行性：esp-sr 的 AEC 需要 64–126 KB PSRAM
（官方资源表，ESP32-P4 @240MHz 实测），现在条件已具备。
详见 `docs/research/2026-10-29-audio-pipeline-research.md`。

PSRAM 占用 GPIO26(CS)/GPIO30(CLK) 与 33–37，与麦克风 4/5/6、扬声器 15/16/7 均不冲突。
`main` 组件需要 `PRIV_REQUIRES esp_psram` 才能 `#include "esp_psram.h"`。

## 串口与复位（2026-10-29 实测）

板子插的是原生 USB（USB-Serial/JTAG，设备名「USB 串行设备 (COM4)」），不是 UART 桥。
排查中遇到：烧录成功且 `verify_flash` 三处全过，但每次复位都停在
`rst:0x15 (USB_UART_CHIP_RESET),boot:0x1 (DOWNLOAD(USB/UART0))`，
DTR/RTS 的三种时序（含 esptool `USBJTAGSerialReset` 原样时序）都无法让它进应用。
ROM 报 `boot:0x1` 说明**复位瞬间 GPIO0 是低电平**（GPIO0 就是 BOOT 脚）。
遇到这种情况先确认 BOOT 键没被按住/没被外部拉低，再按一次 RST。

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
- **`STAT` 控制帧**：向 `板子IP:5001` 发 4 字节 `STAT`，回包一行
  `STAT heap=<内部堆> psram_free=<PSRAM 空闲> psram_total=<PSRAM 总量> learned=<yes|no> target=<当前上行目标IP:端口>`。
  用来在**不接串口**的情况下验收内存与上行目标，PC 侧探针见 `F:\cache\stat_probe.py`。
  板子 IP 不知道时可以广播 `255.255.255.255:5001`，回包来源就是板子地址。

已验证（2026-10-28，咪头回采）：自检音两段频率在 880/1320 Hz 各命中 8/7 帧；PC 推 2 s 1 kHz 正弦，回采命中 95 帧（能量 >3000）。扬声器初始化失败只告警，不影响上行采集。

```bash
# 触发自检音（Linux/macOS）
printf 'TONE' | nc -u -w1 <board-ip> 5001
```

## 文字转语音（TTS）下行

`tools/tts_play.py` 用 Windows SAPI（离线，默认 `Microsoft Huihui Desktop`）合成 wav，
重采样为 16 kHz/单声道/s16LE 后按 20 ms 一帧推到板子，板子直接写 I2S 播放：

```bash
python tools/tts_play.py --ip <board-ip> --text "你好"
python tools/tts_play.py --ip <board-ip> --text "你好" --verify   # 用咪头回采自证
python tools/tts_play.py --ip <board-ip> --text "hello" --voice "Microsoft Zira Desktop"
```

已验证（2026-10-28）：合成「你好，我是这块板子的扬声器。」3.90 s，194 帧实时推送，
咪头回采静音基线 RMS 108、峰值 448、21 帧越过 2 倍基线阈值，判定 PASS。
语音能量天然低于满幅正弦，验证阈值按「播放前基线 ×2」动态取，不要用固定 600。

## 上行目标自学习（2026-10-29 实测）

上行目标不再只认编译期的 `AUDIO_UDP_TARGET`：**任何**到达 UDP 5001 的下行包，其来源 IP 会成为新的上行目标
（只换 IP，端口仍用编译期端口；不写 NVS，重启回到编译期目标）。实现在 `components/tarot_core/src/lib.rs`
的 `LEARNED_TARGET_IP` 原子量 + `main/audio_speaker.cc` 下行任务里的 `rust_udp_set_target_ip()`。
流程图真源：`docs/archify/esp32s3-inmp441-uplink-target_workflow.html`。

改动 `AUDIO_WIFI_SSID`/`AUDIO_WIFI_PASSWORD`/`AUDIO_UDP_TARGET`（或 `audio.env`）会触发 Rust 重编：
`components/tarot_core/build.rs` 里对这三个变量做了 `cargo:rerun-if-env-changed`，
`CMakeLists.txt` 再用「audio.env 摘要 + 三个变量」做 stamp 依赖（此前改了环境变量不会重编，容易烧进旧凭据）。

实测（2026-10-29，故意烧 `AUDIO_UDP_TARGET=192.168.237.250:5000` 假目标）：

| 阶段 | 结果 |
|------|------|
| 收到任何下行包之前 | `0 pkt/s`（上行确实发往假目标，PC 收不到） |
| 发 `STAT` 后 | 回包 `learned=yes target=192.168.237.7:5000` |
| 学完之后 | `200 包/4s = 50.00 pkt/s`，满包率，来源 `192.168.237.168` |

## 设备端回声消除（esp-sr AEC，2026-10-29 实测）

固件在「I2S0 采集 → 上行」之间插了一层 AEC：把正在送往 I2S1 的播放数据当作参考信号，
从麦克风信号里减掉扬声器的声音，PC 侧才有条件跑真全双工（`mic_gate: off`）。
代码在 `main/audio_aec.cc`（新增）+ `audio_transmitter.cc` / `audio_speaker.cc` 各一处钩子，
流程图真源 `docs/archify/esp32s3-device-aec_workflow.html`。

- **组件版本**：`espressif/esp-sr` 只能用 **2.4.x（实测解析到 2.4.7）**。
  2.5/2.6 会拉 esp-dl 3.3.x，它用了 IDF 5.4 才有的 `MALLOC_CAP_SIMD`，
  在本机 IDF 5.3.1 上编译报 `'MALLOC_CAP_SIMD' was not declared`。已在 `main/idf_component.yml` 注释里写明。
- **API**（以 `managed_components/espressif__esp-sr/include/esp32s3/esp_aec.h` 为准）：
  `aec_create(sample_rate, filter_length, channel_num, mode)`，`filter_length` 推荐 4；
  `aec_get_chunksize()` 实测 **512 采样 = 32 ms**；`aec_process(handle, mic, ref, out)` 原地三缓冲；
  缓冲必须 16 字节对齐（`heap_caps_aligned_alloc`），大块走 PSRAM。
  档位 `AEC_MODE_FD_LOW_COST` + NLP `AEC_NLP_LEVEL_AGGR`（默认值，代码里显式写出）。
- **帧长不匹配**：上行是 320 采样/20 ms，AEC 是 512 采样/32 ms，`audio_aec.cc` 里做双向重分块
  （8×320 拼 5×512），输出环欠载时补零，因此**开头固定多 32 ms 延迟**。
- **参考信号**：`tx_write()` 在写 I2S 之前把 PCM 灌进 ref 环形缓冲（8192 采样 = 512 ms，
  2 的幂用掩码取模）。没有播放时 ref 欠载 → 喂静音 → AEC 近似直通。
- **内存实测**（`STAT` 回包）：`psram_free` 从 8 384 704 降到 8 254 252，共吃约 **134 KB PSRAM**
  （AEC 内部约 107 KB + 我们自己的 27 KB 缓冲）；`inner_free=223 963`（约 219 KB 内部 RAM 剩余）。
  固件 1 099 328 → **1 173 184 字节**（+74 KB，只链接 AEC 相关目标文件），factory 分区 4 MB 仍宽裕。

`STAT` 回包新增字段：`inner_free` / `aec=on|off` / `frame` / `blocks` / `ref_uf`（ref 欠载次数）/
`ref_of`（漂移后重对齐次数）。

验收（2026-10-29，`tools/tts_play.py` 播 7.82 s 中文长句，PC 侧同时抓上行）：

| 指标 | 改造前（无 AEC） | 改造后（AEC） |
|---|---|---|
| 回采峰值 RMS | 669（另有 448 峰值） | 418（单帧瞬态），播放期平均 **97** |
| 环境底噪 RMS | 99–108 | 104 |
| 越过 2 倍基线阈值帧数 | 21 帧 | 0–2 帧（只在每句开头 300 ms） |

证明「声音确实播出去了」而不是「没声所以没回声」：两次 `STAT` 之间 `blocks +372`、`ref_uf +128`，
即有 **244 个块拿到了参考信号**，正好等于 7.82 s × 31.25 块/s。

**已知残留**：每句话开头约 300 ms AEC 还在收敛，会有 1–2 帧冲到 156–171（偶发 418），
下游 VAD 阈值在 170 附近时可能被触发；如果真出现重复转写，把 `mic_gate` 切回 `"auto"` 即可退回半双工。

## Constraints

- Do not drive GPIO0 low during ordinary startup.
- Keep GPIO43/44 available for UART0 recovery.
- Do not use GPIO45/46, GPIO19/20, or GPIO48 for this slice.
- Do not connect an unregulated Li-Po cell directly to `VCC_5V`.
- The desktop receiver application is not part of this firmware slice; the speaker (I2S TX) is.
