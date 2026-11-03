// 扬声器（I2S TX）支持：开机自检音 + UDP 下行播放 + 运行时改引脚（用于接线排查）。
// 引脚由 CMake 注入（SPEAKER_BCLK/WS/DOUT/EN），默认 17/18/16/15。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio_aec.h"
#include "audio_ns.h"
#include "audio_vad.h"
#include "audio_speaker.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <netinet/in.h>
#include <sys/socket.h>

// 上行目标自学习（Rust 侧实现）：收到的下行包来源 IP 会成为新的上行目标
extern "C" void rust_udp_set_target_ip(uint32_t ip_be);
extern "C" uint32_t rust_udp_target_ip(void);
extern "C" uint16_t rust_udp_target_port(void);

#ifndef SPEAKER_BCLK
#define SPEAKER_BCLK 15
#endif
#ifndef SPEAKER_WS
#define SPEAKER_WS 16
#endif
#ifndef SPEAKER_DOUT
#define SPEAKER_DOUT 7
#endif
#ifndef SPEAKER_EN
#define SPEAKER_EN -1
#endif
#ifndef SPEAKER_UDP_PORT
#define SPEAKER_UDP_PORT 5001
#endif
#ifndef SPEAKER_RATE
#define SPEAKER_RATE 16000
#endif

namespace {
constexpr char TAG[] = "audio_spk";
constexpr size_t TX_BUF_SAMPLES = 1024;
// 下行抖动缓冲：0.5s @16k。BLE 20ms 一帧，网络抖动可达几十上百毫秒；
// 没有缓冲时 GATT 回调直接阻塞写 I2S，DMA 一空就咔哒/断流。
constexpr size_t RING_SAMPLES = 16000;
constexpr size_t FEED_CHUNK = 320;   // 20 ms

i2s_chan_handle_t tx_handle = nullptr;
int cur_bclk = SPEAKER_BCLK;
int cur_ws = SPEAKER_WS;
int cur_dout = SPEAKER_DOUT;
int16_t *s_ring = nullptr;
size_t s_w = 0, s_r = 0, s_used = 0;
SemaphoreHandle_t s_ring_mux = nullptr;
TaskHandle_t s_feed_task = nullptr;
volatile uint32_t s_underruns = 0;
// 播放期统计（STAT 读取后清零）：pushes=收到帧数，gapmax=帧间最大间隔 ms，
// urp=播放期欠载次数，rmin=播放期环形缓冲最低水位（采样）
volatile uint32_t s_stat_pushes = 0;
volatile uint32_t s_stat_gapmax_ms = 0;
volatile uint32_t s_stat_urp = 0;
volatile uint32_t s_stat_rmin = 0xFFFFFFFFu;
volatile int64_t s_last_push_us = 0;

void speaker_feed_task(void *);   // 定义在文件后部，tx_init 里启动

void amp_enable(bool on) {
#if SPEAKER_EN >= 0
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << SPEAKER_EN;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(static_cast<gpio_num_t>(SPEAKER_EN), on ? 1 : 0);
#else
    (void)on;
#endif
}

// 按给定引脚重建 I2S TX 通道（已在用则先释放）
bool tx_init(int bclk, int ws, int dout) {
    if (tx_handle != nullptr) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = nullptr;
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.dma_desc_num = 8;     // 原来 6×240 帧 ≈ 90ms，加厚到 ≈120ms 兜底
    chan_cfg.dma_frame_num = 240;
    if (i2s_new_channel(&chan_cfg, &tx_handle, nullptr) != ESP_OK) {
        tx_handle = nullptr;
        return false;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = {.sample_rate_hz = SPEAKER_RATE, .clk_src = I2S_CLK_SRC_DEFAULT,
                    .ext_clk_freq_hz = 0, .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                     .bclk = static_cast<gpio_num_t>(bclk),
                     .ws = static_cast<gpio_num_t>(ws),
                     .dout = static_cast<gpio_num_t>(dout),
                     .din = I2S_GPIO_UNUSED,
                     .invert_flags = {false, false, false}},
    };
    if (i2s_channel_init_std_mode(tx_handle, &std_cfg) != ESP_OK ||
        i2s_channel_enable(tx_handle) != ESP_OK) {
        i2s_del_channel(tx_handle);
        tx_handle = nullptr;
        return false;
    }

    // 抖动缓冲与播放任务只建一次；换引脚重配时继续复用
    if (s_ring == nullptr) {
        s_ring = static_cast<int16_t *>(
            heap_caps_malloc(RING_SAMPLES * sizeof(int16_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_ring == nullptr) {
            s_ring = static_cast<int16_t *>(malloc(RING_SAMPLES * sizeof(int16_t)));
        }
        s_ring_mux = xSemaphoreCreateMutex();
        if (s_ring != nullptr && s_ring_mux != nullptr) {
            xTaskCreate(speaker_feed_task, "spk_feed", 4096, nullptr, 4, &s_feed_task);
            ESP_LOGI(TAG, "downlink jitter buffer active (%u samples)",
                     static_cast<unsigned>(RING_SAMPLES));
        } else {
            ESP_LOGW(TAG, "jitter buffer alloc failed, direct write fallback");
        }
    }
    cur_bclk = bclk;
    cur_ws = ws;
    cur_dout = dout;
    return true;
}

// 写 PCM（s16LE mono）：喂 AEC 参考后放进环形缓冲，由 speaker_feed_task 按 20ms 节奏
// 稳定写 I2S。绝不能在这里阻塞——它会被 BLE GATT 回调调用，阻塞会拖垮蓝牙栈。
void tx_write(const int16_t *samples, size_t count) {
    audio_aec_feed_ref(samples, count);
    if (tx_handle == nullptr || s_ring == nullptr || s_ring_mux == nullptr) return;
    const int64_t now_us = esp_timer_get_time();
    if (s_last_push_us != 0) {
        const uint32_t gap_ms = static_cast<uint32_t>((now_us - s_last_push_us) / 1000);
        if (gap_ms > s_stat_gapmax_ms) s_stat_gapmax_ms = gap_ms;
    }
    s_last_push_us = now_us;
    ++s_stat_pushes;
    xSemaphoreTake(s_ring_mux, portMAX_DELAY);
    for (size_t i = 0; i < count; ++i) {
        if (s_used == RING_SAMPLES) {          // 满了丢最旧
            s_r = (s_r + 1) % RING_SAMPLES;
            --s_used;
        }
        s_ring[s_w] = samples[i];
        s_w = (s_w + 1) % RING_SAMPLES;
        ++s_used;
    }
    xSemaphoreGive(s_ring_mux);
}

// 专用播放任务：固定 20ms 节奏取 320 采样写 I2S；不足补零。
// BLE 到达抖动只会让缓冲变浅/变深，不会造成 I2S 欠载咔哒。
void speaker_feed_task(void *) {
    static int16_t chunk[FEED_CHUNK];
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
        if (tx_handle == nullptr || s_ring == nullptr || s_ring_mux == nullptr) continue;
        size_t n = 0;
        xSemaphoreTake(s_ring_mux, portMAX_DELAY);
        // 只有凑满 20ms 才写：不足时保持不动，让 I2S DMA（~120ms）顶上。
        // 以前不足就补零，会在播放中周期性插入 20ms 静音，听起来就是咔哒。
        if (s_used >= FEED_CHUNK) {
            while (n < FEED_CHUNK) {
                chunk[n++] = s_ring[s_r];
                s_r = (s_r + 1) % RING_SAMPLES;
                --s_used;
            }
        }
        const size_t fill = s_used;
        xSemaphoreGive(s_ring_mux);
        const bool streaming = s_last_push_us != 0 && (esp_timer_get_time() - s_last_push_us) < 300000;
        if (streaming) {
            if (fill < s_stat_rmin) s_stat_rmin = static_cast<uint32_t>(fill);
            if (n == 0 && fill == 0) ++s_stat_urp;
        }
        if (n == 0) {
            if (fill == 0) ++s_underruns;
            continue;
        }
        size_t written = 0;
        i2s_channel_write(tx_handle, chunk, sizeof(chunk), &written, pdMS_TO_TICKS(100));
    }
}

// 自检音：两段短促双音，既用于开机，也可由下行 "TONE" 控制帧触发，
// 便于没有示波器时用咪头回采验证扬声器链路。
void play_selftest_tone() {
    const int freqs[] = {880, 1320};
    const int durs_ms[] = {180, 140};
    for (int k = 0; k < 2; ++k) {
        const int n = SPEAKER_RATE * durs_ms[k] / 1000;
        static int16_t buf[TX_BUF_SAMPLES];
        int done = 0;
        while (done < n) {
            int chunk = (n - done > TX_BUF_SAMPLES) ? TX_BUF_SAMPLES : (n - done);
            for (int i = 0; i < chunk; ++i) {
                double t = (double)(done + i) / SPEAKER_RATE;
                buf[i] = (int16_t)(9000 * sin(2 * 3.14159265358979 * freqs[k] * t));
            }
            tx_write(buf, chunk);
            done += chunk;
        }
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

// 控制帧 "PIN:b,w,d" → 换引脚重配 I2S 再放自检音
void handle_pin_frame(const uint8_t *pkt, int len) {
    char text[48] = {0};
    const int n = (len < (int)sizeof(text) - 1) ? len : (int)sizeof(text) - 1;
    memcpy(text, pkt, (size_t)n);
    int b = 0, w = 0, d = 0;
    if (sscanf(text, "PIN:%d,%d,%d", &b, &w, &d) != 3) {
        ESP_LOGW(TAG, "bad PIN frame: %s", text);
        return;
    }
    if (b < 0 || w < 0 || d < 0) return;
    ESP_LOGI(TAG, "retarget pins bclk=%d ws=%d dout=%d -> %s", b, w, d,
             tx_init(b, w, d) ? "ok" : "FAIL");
    play_selftest_tone();
}

void downlink_task(void *) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(SPEAKER_UDP_PORT);
    bind(sock, (sockaddr *)&local, sizeof(local));
    ESP_LOGI(TAG, "speaker downlink listening on UDP %d", SPEAKER_UDP_PORT);

    static uint8_t pkt[2048];
    while (true) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, pkt, sizeof(pkt), 0, (sockaddr *)&from, &from_len);
        if (len <= 0) continue;
        // 谁给我们发下行，接下来上行就发给谁（端口沿用编译期 AUDIO_UDP_TARGET 的端口）
        if (from.sin_family == AF_INET && from.sin_addr.s_addr != 0) {
            rust_udp_set_target_ip(from.sin_addr.s_addr);
        }
        // 控制帧 STAT → 回包报告堆/PSRAM/当前上行目标，PC 侧用来验证而不用碰串口
        if (len >= 4 && memcmp(pkt, "STAT", 4) == 0) {
            uint32_t raw = rust_udp_target_ip();
            uint32_t host = ntohl(raw);
            uint32_t aec_blocks = 0, ref_under = 0, ref_over = 0;
            int aec_frame = 0;
            uint32_t aec_avg_us = 0, aec_max_us = 0;
            audio_aec_stats(&aec_blocks, &ref_under, &ref_over, &aec_frame);
            audio_aec_timing(&aec_avg_us, &aec_max_us);
            uint32_t ns_frames = 0, ns_skipped = 0;
            uint32_t ns_avg_us = 0, ns_max_us = 0;
            audio_ns_stats(&ns_frames, &ns_skipped);
            audio_ns_timing(&ns_avg_us, &ns_max_us);
            uint32_t vad_frames = 0, vad_speech = 0;
            audio_vad_stats(&vad_frames, &vad_speech);
            char reply[640];
            int n = snprintf(reply, sizeof(reply),
                             "STAT heap=%u inner_free=%u psram_free=%u psram_total=%u "
                             "aec=%s frame=%d blocks=%u us_avg=%u us_max=%u ref_uf=%u ref_of=%u "
                             "ns=%s ns_frames=%u ns_us_avg=%u ns_us_max=%u ns_skip=%u "
                             "vad=%s vad_frames=%u vad_speech=%u "
                             "learned=%s "
                             "target=%u.%u.%u.%u:%d\n",
                             (unsigned)esp_get_free_heap_size(),
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                             (unsigned)esp_psram_get_size(),
                             audio_aec_ready() ? "on" : "off", aec_frame,
                             (unsigned)aec_blocks, (unsigned)aec_avg_us, (unsigned)aec_max_us,
                             (unsigned)ref_under, (unsigned)ref_over,
                             audio_ns_enabled() ? "on" : "off", (unsigned)ns_frames,
                             (unsigned)ns_avg_us, (unsigned)ns_max_us, (unsigned)ns_skipped,
                             audio_vad_ready() ? "on" : "off", (unsigned)vad_frames,
                             (unsigned)vad_speech,
                             raw ? "yes" : "no",
                             (unsigned)((host >> 24) & 0xFF), (unsigned)((host >> 16) & 0xFF),
                             (unsigned)((host >> 8) & 0xFF), (unsigned)(host & 0xFF),
                             (unsigned)rust_udp_target_port());
            sendto(sock, reply, n > 0 ? n : 0, 0, (sockaddr *)&from, from_len);
            continue;
        }
        if (len >= 5 && memcmp(pkt, "PIN:", 4) == 0) {
            handle_pin_frame(pkt, len);
            continue;
        }
        // 控制帧 "NS:on" / "NS:off" → 运行期旁路设备端降噪，PC 侧用来做 A/B 对照
        if (len >= 5 && memcmp(pkt, "NS:", 3) == 0) {
            const bool on = (pkt[3] == 'o' && pkt[4] == 'n');
            audio_ns_set_enabled(on);
            continue;
        }
        // 控制帧: 前 4 字节 "TONE" → 播放自检音（PC 侧用它验证扬声器链路）
        if (len >= 4 && memcmp(pkt, "TONE", 4) == 0) {
            ESP_LOGI(TAG, "selftest tone requested");
            play_selftest_tone();
            continue;
        }
        if (len <= 20) continue;
        // AUD0 头: magic(4) ver(1) flags(1) hlen(2) seq(4) rate(4) samples(2) payload_len(2)
        uint16_t hlen = pkt[6] | (pkt[7] << 8);
        if (hlen < 20 || hlen > len) continue;
        uint16_t payload_len = static_cast<uint16_t>(len - hlen);
        // s16LE mono 原样写 I2S（16k 上行采样率匹配）
        int16_t *samples = (int16_t *)(pkt + hlen);
        tx_write(samples, payload_len / 2);
    }
}

// 只做硬件部分：I2S TX 初始化 + 功放使能 + 开机自检音。
// UDP 下行任务单独起，方便 BLE 模式只取这一半。
void speaker_init_hw() {
    if (!tx_init(SPEAKER_BCLK, SPEAKER_WS, SPEAKER_DOUT)) {
        ESP_LOGW(TAG, "I2S TX init failed(%d/%d/%d); speaker disabled", SPEAKER_BCLK,
                 SPEAKER_WS, SPEAKER_DOUT);
        // 仍然起下行任务，这样可以通过 PIN: 控制帧重新尝试其它接线
    } else {
        amp_enable(true);
        ESP_LOGI(TAG, "speaker ready: bclk=%d ws=%d dout=%d en=%d rate=%d", SPEAKER_BCLK,
                 SPEAKER_WS, SPEAKER_DOUT, SPEAKER_EN, SPEAKER_RATE);
        play_selftest_tone();
    }
}

}  // namespace

extern "C" void speaker_start(void) {
    speaker_init_hw();
    xTaskCreate(downlink_task, "spk_downlink", 4096, nullptr, 5, nullptr);
}

extern "C" void speaker_start_i2s_only(void) {
    speaker_init_hw();
    ESP_LOGI(TAG, "speaker: I2S only (no UDP downlink task)");
}

extern "C" void speaker_write(const int16_t *samples, size_t count) {
    tx_write(samples, count);
}

extern "C" void speaker_play_tone(void) { play_selftest_tone(); }

extern "C" uint32_t speaker_underruns(void) { return s_underruns; }

extern "C" void speaker_stats_take(uint32_t *pushes, uint32_t *gapmax_ms, uint32_t *urp, uint32_t *rmin) {
    if (pushes) *pushes = s_stat_pushes;
    if (gapmax_ms) *gapmax_ms = s_stat_gapmax_ms;
    if (urp) *urp = s_stat_urp;
    if (rmin) *rmin = s_stat_rmin;
    s_stat_pushes = 0;
    s_stat_gapmax_ms = 0;
    s_stat_urp = 0;
    s_stat_rmin = 0xFFFFFFFFu;
    s_last_push_us = 0;   // 下一个窗口重新计最大间隔
}
