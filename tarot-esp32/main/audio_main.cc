#include <cstdio>

#include "audio_transmitter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI("audio_main", "ESP32-S3 INMP441 UDP transmitter starting");
    ESP_ERROR_CHECK(audio_transmitter_start());
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
