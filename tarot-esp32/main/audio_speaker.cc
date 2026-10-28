// 扬声器（I2S TX）支持：开机自检音 + UDP 下行播放 + 运行时改引脚（用于接线排查）。
// 引脚由 CMake 注入（SPEAKER_BCLK/WS/DOUT/EN），默认 17/18/16/15。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio_speaker.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <netinet/in.h>
#include <sys/socket.h>

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

i2s_chan_handle_t tx_handle = nullptr;
int cur_bclk = SPEAKER_BCLK;
int cur_ws = SPEAKER_WS;
int cur_dout = SPEAKER_DOUT;

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
    cur_bclk = bclk;
    cur_ws = ws;
    cur_dout = dout;
    return true;
}

// 阻塞写 PCM（s16LE mono），I2S 时钟自动节流
void tx_write(const int16_t *samples, size_t count) {
    if (tx_handle == nullptr) return;
    size_t written = 0;
    i2s_channel_write(tx_handle, samples, count * sizeof(int16_t), &written,
                      pdMS_TO_TICKS(1000));
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
        int len = recvfrom(sock, pkt, sizeof(pkt), 0, nullptr, nullptr);
        if (len <= 0) continue;
        if (len >= 5 && memcmp(pkt, "PIN:", 4) == 0) {
            handle_pin_frame(pkt, len);
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
}  // namespace

extern "C" void speaker_start(void) {
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
    xTaskCreate(downlink_task, "spk_downlink", 4096, nullptr, 5, nullptr);
}
