#include "audio_transmitter.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "audio_aec.h"
#include "audio_ble.h"
#include "audio_ns.h"
#include "audio_vad.h"
#include "audio_speaker.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

extern "C" void rust_udp_send_audio(const uint8_t *data, size_t len, uint32_t sequence,
                                     uint32_t sample_rate, uint16_t sample_count, uint8_t flags);
extern "C" void rust_audio_network_start(void);

namespace {
constexpr char TAG[] = "audio_tx";
constexpr gpio_num_t PIN_I2S_SCK = GPIO_NUM_4;
constexpr gpio_num_t PIN_I2S_WS = GPIO_NUM_5;
constexpr gpio_num_t PIN_I2S_SD = GPIO_NUM_6;
// GPIO7 已分配给扬声器 I2S BCLK，不再作为按键输入（避免上拉与 I2S 输出冲突）
constexpr gpio_num_t PIN_KEY_2 = GPIO_NUM_8;
constexpr gpio_num_t PIN_KEY_3 = GPIO_NUM_9;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t SAMPLES_PER_FRAME = 320;
constexpr size_t FRAME_QUEUE_LENGTH = 4;
constexpr uint8_t FLAG_SPEECH = 0x01;   // 上行 flags bit0：设备端判为语音
// bit1 = 本帧的 bit0 可信（AFE 生效）。老固件两位都是 0，PC 端据此区分
// 「没有 VAD 提示」和「AFE 判为静音」——不区分的话老固件就永远切不出句子。
constexpr uint8_t FLAG_VAD_VALID = 0x02;

struct AudioFrame {
    uint32_t sequence;
    uint8_t flags;
    std::array<int16_t, SAMPLES_PER_FRAME> samples;
};

std::array<int32_t, SAMPLES_PER_FRAME> i2s_samples;

QueueHandle_t frame_queue;
i2s_chan_handle_t rx_handle;

void capture_task(void *) {
    AudioFrame frame{};
    uint32_t dropped_frames = 0;
    while (true) {
        size_t bytes_read = 0;
        const esp_err_t err = i2s_channel_read(rx_handle, i2s_samples.data(),
                                               sizeof(i2s_samples), &bytes_read,
                                               pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read != sizeof(i2s_samples)) {
            ESP_LOGW(TAG, "I2S read failed: %s bytes=%u", esp_err_to_name(err),
                     static_cast<unsigned>(bytes_read));
            continue;
        }
        for (size_t i = 0; i < SAMPLES_PER_FRAME; ++i) {
            frame.samples[i] = static_cast<int16_t>(i2s_samples[i] >> 16);
        }
        // 音频前端三段：消回声 → 降噪 → 判语音，全部是独立 API。
        // （esp_afe_sr 一体化版本在本机上 fetch 恒失败，见 audio_afe.cc 顶部记录。）
        frame.flags = 0;
        // 设备端消回声：AEC 未就绪时这里直接返回，上行仍是原始麦克风数据
        audio_aec_filter(frame.samples.data(), SAMPLES_PER_FRAME);
        // 再降稳态环境噪声：必须排在 AEC 之后；NS 未就绪时同样直接返回
        audio_ns_process(frame.samples.data(), SAMPLES_PER_FRAME);
        // 最后判语音：看的是 AEC+NS 之后的信号，比 PC 端拿 RMS 猜准得多
        if (audio_vad_ready()) {
            frame.flags = FLAG_VAD_VALID;
            if (audio_vad_process(frame.samples.data(), SAMPLES_PER_FRAME)) {
                frame.flags |= FLAG_SPEECH;
            }
        }
        if (xQueueSend(frame_queue, &frame, 0) != pdTRUE) {
            ++dropped_frames;
            if ((dropped_frames % 32) == 0) {
                ESP_LOGW(TAG, "audio queue full, dropped=%u",
                         static_cast<unsigned>(dropped_frames));
            }
        }
        ++frame.sequence;
    }
}

void send_task(void *) {
    AudioFrame frame{};
    while (xQueueReceive(frame_queue, &frame, portMAX_DELAY) == pdTRUE) {
#ifdef AUDIO_LINK_BLE
        // BLE 模式：ADPCM 压缩后走 GATT 通知。未连接时 audio_ble_send 内部直接丢弃并计数。
        audio_ble_send(frame.samples.data(), SAMPLES_PER_FRAME, frame.flags);
#else
        rust_udp_send_audio(reinterpret_cast<const uint8_t *>(frame.samples.data()),
                            sizeof(frame.samples), frame.sequence, SAMPLE_RATE,
                            SAMPLES_PER_FRAME, frame.flags);
#endif
    }
}

void configure_keys() {
    gpio_config_t config{};
    config.pin_bit_mask = (1ULL << PIN_KEY_2) | (1ULL << PIN_KEY_3);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
}
}  // namespace

extern "C" esp_err_t audio_transmitter_start(void) {
    configure_keys();
#ifdef AUDIO_LINK_BLE
    // BLE 模式不开 Wi-Fi：两者共用射频，同时开会互相抢，丢帧原因就查不清了。
    // 状态查询改走 BLE 的 RX 特征（写 "STAT"）。
    if (!audio_ble_start()) {
        ESP_LOGW(TAG, "BLE start failed, falling back to Wi-Fi UDP");
        rust_audio_network_start();
    }
#else
    rust_audio_network_start();
#endif

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, nullptr, &rx_handle));

    i2s_std_config_t std_config = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_SCK,
            .ws = PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_I2S_SD,
            .invert_flags = {false, false, false},
        },
    };
    std_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    std_config.slot_cfg.bit_shift = true;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_config));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    // 音频前端：AEC → NS → VAD，三段的实现与参数都是实机验证过的。
    // esp_afe_sr 一体化方案在本机（esp-sr 2.4.7 + IDF 5.3.1）跑不通，详见 audio_afe.cc。
    audio_aec_start();  // 失败只告警，上行退化直通
    audio_ns_start();   // 同理：NS 起不来就只保留 AEC 输出
    audio_vad_start();  // 起不来则上行 flags 恒 0，PC 端退回能量阈值

    frame_queue = xQueueCreate(FRAME_QUEUE_LENGTH, sizeof(AudioFrame));
    if (frame_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(capture_task, "audio_capture", 4096, nullptr, 7, nullptr) != pdPASS ||
        xTaskCreate(send_task, "audio_send", 4096, nullptr, 6, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "INMP441 RX started: rate=%u pins=%d/%d/%d",
             static_cast<unsigned>(SAMPLE_RATE), static_cast<int>(PIN_I2S_SCK),
             static_cast<int>(PIN_I2S_WS), static_cast<int>(PIN_I2S_SD));
#ifdef AUDIO_LINK_BLE
    // BLE 模式没有网络栈（Wi-Fi 没起），speaker 的 UDP 下行监听会调 lwIP
    // 然后断言 "Invalid mbox" 并无限重启。下行 TTS 本轮不在范围内，直接不启动。
    ESP_LOGI(TAG, "BLE mode: speaker downlink disabled");
#else
    speaker_start();  // 失败仅告警，不影响上行
#endif
    return ESP_OK;
}
