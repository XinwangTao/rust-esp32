#include "audio_transmitter.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

extern "C" void rust_udp_send_audio(const uint8_t *data, size_t len, uint32_t sequence,
                                     uint32_t sample_rate, uint16_t sample_count);
extern "C" void rust_audio_network_start(void);

namespace {
constexpr char TAG[] = "audio_tx";
constexpr gpio_num_t PIN_I2S_SCK = GPIO_NUM_4;
constexpr gpio_num_t PIN_I2S_WS = GPIO_NUM_5;
constexpr gpio_num_t PIN_I2S_SD = GPIO_NUM_6;
constexpr gpio_num_t PIN_KEY_1 = GPIO_NUM_7;
constexpr gpio_num_t PIN_KEY_2 = GPIO_NUM_8;
constexpr gpio_num_t PIN_KEY_3 = GPIO_NUM_9;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t SAMPLES_PER_FRAME = 320;
constexpr size_t FRAME_QUEUE_LENGTH = 4;

struct AudioFrame {
    uint32_t sequence;
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
        rust_udp_send_audio(reinterpret_cast<const uint8_t *>(frame.samples.data()),
                            sizeof(frame.samples), frame.sequence, SAMPLE_RATE,
                            SAMPLES_PER_FRAME);
    }
}

void configure_keys() {
    gpio_config_t config{};
    config.pin_bit_mask = (1ULL << PIN_KEY_1) | (1ULL << PIN_KEY_2) | (1ULL << PIN_KEY_3);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
}
}  // namespace

extern "C" esp_err_t audio_transmitter_start(void) {
    configure_keys();
    rust_audio_network_start();

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
    return ESP_OK;
}
