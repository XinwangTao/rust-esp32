# 音频链路优化调研（2026-10-29）

对象：ESP32-S3 + INMP441 上行 / 板载 I2S 功放下行 / UDP 双向的无线麦克风项目。
目的：找出回声、抖动、CPU 余量三类问题在业界的成熟做法，并挑出能落地的改动。

## 调研渠道说明

本机 `WebSearch` 返回 403、`WebFetch` 对 github.com 域名校验失败，因此改用：

- `gh api`（已登录）读仓库 README / 源码 / 目录
- `curl` 抓 `docs.espressif.com` 页面后本地去标签

下文每条结论标注 **已验证**（实际抓到内容）/ **第三方宣称**（对方给出的数据，本机未复现）/ **推断**（由已验证事实推出）。

---

## 〇、落地结论（2026-10-29）

设备端 AEC 已实机跑通，走 esp-sr 的 **AEC 直调**（不走 AFE），档位 `FD_LOW_COST`。
踩到并已定档的坑：**esp-sr 只能用 2.4.x**——2.5/2.6 依赖 esp-dl 3.3.x，后者用了 IDF 5.4 的
`MALLOC_CAP_SIMD`，IDF 5.3.1 编不过（`main/idf_component.yml` 已注释说明）。
实测帧长 512 采样/32 ms（与官方文档一致），PSRAM 占用约 107 KB，固件 +74 KB。
详见 `tarot-esp32/hardware-esp32s3-inmp441.md` 的「设备端回声消除」章节。

## 一、AEC：乐鑫官方的路线

### 1. esp-sr 的 AEC 接口（已验证）

源码文档：<https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html>

```c
aec_handle_t *aec = aec_create(16000, /* 只支持 16k */
                               ...,
                               AEC_MODE_FD_LOW_COST);   // 官方推荐的全双工默认档

int frame_size = aec_get_chunksize(aec);   // 32ms 一帧 = 512 采样
aec_process(aec, mic, ref, out);           // ref = 送给扬声器的参考信号
```

要点：
- **必须有参考信号 `ref`**（即「我们正在播放什么」），这正是双向链路天然具备的。
- 16 kHz 单声道、FD/SR 模式帧长 32 ms，VOIP 模式 16 ms。
- NLP 档位 `NORMAL / AGGR / VERYAGGR`，仅 FD 模式生效。

官方资源表（**实测平台是 ESP32-P4 @240 MHz，不是 S3**，文中有注）：

| 模式 | 内部 RAM | PSRAM | 每帧耗时 | CPU |
|---|---|---|---|---|
| SR_LOW_COST | 18.8 KB | 64.0 KB | 2.29 / 32 ms | 7.2% |
| FD_LOW_COST | 30.9 KB | 90.0 KB | 6.28 / 32 ms | 19.6% |
| FD_HIGH_PERF | 20.3 KB | 126.2 KB | 8.08 / 32 ms | 25.3% |
| VOIP_HIGH_PERF | 69.2 KB | 66.6 KB | 5.05 / 16 ms | 31.6% |

### 2. 集成范例（已验证）

`espressif/esp-skainet` 的 `examples/voice_communication` 就是「麦克风 + 扬声器 + AEC」的标准写法：

```c
afe_config_t *cfg = afe_config_init(input_format, models, AFE_TYPE_VC, AFE_MODE_LOW_COST);
afe_handle = esp_afe_handle_from_config(cfg);
afe_data   = afe_handle->create_from_config(cfg);   // 之后 feed / fetch 循环
```

其 `idf_component.yml` 依赖 `espressif/esp-sr: "~2.4.7"`，且 AFE 缓冲区用
`heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` 分配 —— **默认吃 PSRAM**。

### 3. 第三方 S3 实测（第三方宣称，未复现）

<https://github.com/seekaudio/seekaudio_aec_test>（A/B 对比 esp-sr 与自家 AEC）：

| 指标 | esp-sr FD-AEC（基线 B1） |
|---|---|
| CPU 平均负载 | 60.5%（S3 @240 MHz，16 kHz，32 ms 帧） |
| 实时率 RTF | 1.65× |
| 内部 SRAM | 12.4 KB |
| PSRAM | 182.9 KB |

→ **推断**：S3 跑 esp-sr AEC 是可行的，但要 ~90–180 KB PSRAM、吃掉一半以上 CPU。

**2026-10-29 已确认（esptool 读 eFuse）**：这块板子是 ESP32-S3 + 16MB Flash +
**封装内 8MB PSRAM**。所以 PSRAM 不是障碍；真正要先做的是在 `sdkconfig.audio-s3` 里
打开 `CONFIG_SPIRAM`（当前没开，PSRAM 一直闲置）。

（注：仓库根目录 `hardware.md` 写「PSRAM: 无」，那是 ESP32-C3 塔罗屏项目的旧资料，
不适用于这块 S3 音频板。）

### 4. 结论

| 方案 | 代价 | 适用 |
|---|---|---|
| PC 端在朗读期间静音麦克风（半双工） | 0 CPU、0 内存 | **默认采用**，彻底消除回采重复转写 |
| 设备端 esp-sr AEC | ~180 KB PSRAM + 60% CPU | 确认有 PSRAM 且要真全双工时再上 |
| PC 端 WebRTC AEC3 | PC 侧算力，但需额外二进制依赖 | 不划算 |

---

## 二、CPU 与编译选项（本机已验证的问题）

改造前实测配置（`tarot-esp32/sdkconfig.audio-s3`）：

- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`（S3 支持 240 MHz）
- `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y`（-Og，断言全开）

对照上面所有 AEC 数据都是在 **240 MHz** 下测的，我们等于先自废约 1/3 算力。
已改为 240 MHz + `CONFIG_COMPILER_OPTIMIZATION_PERF`（-O2），这是零成本余量，
后续任何 DSP（AEC / 降噪 / Opus）都依赖它。

---

## 三、编码：要不要上 Opus

- `sh123/esp32_loradv`（⭐127，Codec2/Opus 数字对讲机）已验证支持
  **Codec2 700–3200 bps、Opus 2400–512000 bps**。
- 我们当前：16 kHz × 16 bit = 256 kbit/s，局域网完全够用。

**推断**：除非要走公网/弱网/多路，256 kbit/s 不是瓶颈；Opus 的收益（带宽↓、抗丢包 FEC）
抵不上引入 libopus 的移植与 CPU 成本。**暂不引入**，只记录为可选路线。

---

## 四、传输层：上游项目怎么处理

- `atomic14/esp32-walkie-talkie`（⭐651）：音频走 **UDP 广播或 ESP-NOW**，配置在 `config.h`，
  这样接收端 IP 变了也不用重新烧录固件。
- 我们的固件把目标地址编译进 `AUDIO_UDP_TARGET`（`components/tarot_core/src/lib.rs:18`），
  PC 端 IP 一变就得重烧 —— 是真实痛点，可用「从下行包学习对端地址」或「广播」解决。

---

## 五、本次已落地的改动

| # | 问题 | 位置 | 验证 |
|---|---|---|---|
| 1 | 每段语音前 200 ms 被丢弃（首字吞掉） | `mic-desktop/micapp/vad.py` | 合成信号回归：首帧峰值 3000 恢复 |
| 2 | 一个乱序/重复包让丢包数暴涨 4294967294 | `micapp/udp_receiver.py` | 已换成按播放顺序计数 |
| 3 | 乱序包直接乱序播放，转写串字 | `micapp/udp_receiver.py` | 3 帧重排缓冲 + 60 ms 超时放行，用例通过 |
| 4 | 朗读声被麦克风采回，重复转写 | `micapp/gate.py`（新增）+ `main.py` | 朗读期间丢弃 25 帧、恢复后切段正常 |
| 5 | 固件 160 MHz + -Og | `tarot-esp32/sdkconfig.audio-s3` | 已改 240 MHz + -O2，实机验收通过（60 s 2981 包 / 49.67 pkt/s / 丢包 0.67%） |
| 6 | 8 MB PSRAM 闲置 | `sdkconfig.audio-s3`（`CONFIG_SPIRAM` 八线 80 MHz） | `STAT` 回包 `psram_total=8388608 psram_free=8384704` |
| 7 | 上行目标写死，换 PC 网段就要重烧 | `tarot_core/src/lib.rs` + `main/audio_speaker.cc` | 假目标 192.168.237.250 烧录后：收到下行前 0 pkt/s，STAT 后 50.00 pkt/s |
| 8 | 改 `audio.env` 不触发 Rust 重编（凭据/目标静默不更新） | `tarot_core/build.rs`（新增）+ `tarot_core/CMakeLists.txt` stamp | 换目标后确实重编（本次假目标验证依赖它） |
| 9 | 朗读声被麦克风采回（此前靠 PC 侧半双工硬挡） | `tarot-esp32/main/audio_aec.cc`（新增）+ 采集/播放各一处钩子 | 播 7.82 s TTS：回采平均 RMS 97（底噪 104），改造前峰值 669；`STAT` 显示 `aec=on frame=512 blocks` 递增 |

## 六、后续（需先改图再动流程）

1. **设备端 AEC**：已完成并验收（见第〇节与第 9 条），图真源
   `docs/archify/esp32s3-device-aec_workflow.html`。PC 侧 `mic_gate` 已可切 `off` 跑全双工。
2. **静音不上行**：设备端做能量门控，安静时不发包（省电、省空口），代价是接收端 VAD 需要保活机制。同为 L1。
3. **编码**：Opus 需改 AUD0 帧格式（对外契约），非必要不动；真要省带宽先考虑 20 ms→40 ms 聚合。

已落地：
- **上行目标地址自学习**（原第 1 条）：已按确认后的图实现并实机验证，
  图真源 `docs/archify/esp32s3-inmp441-uplink-target_workflow.html`。
  决策：任何来源的下行包都学、只学 IP 不学端口、不写 NVS、不加 `TARGET:reset`。
