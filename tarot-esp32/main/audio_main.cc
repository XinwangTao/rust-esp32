#include <cstdio>

#include "audio_ble.h"
#include "audio_transmitter.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "audio_main";

// OTA 开机自检：新固件处于 PENDING_VERIFY 时，确认 BLE 链路、麦克风首帧、PSRAM 都正常
// 才标记 valid；任一项 10 秒内没就绪则不标记，bootloader 下次启动自动回滚旧分区。
static void ota_boot_verify_task(void *) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "running partition=%s state=%d", running->label, static_cast<int>(state));
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        vTaskDelete(nullptr);
        return;
    }

    bool ok = false;
    for (int i = 0; i < 50; ++i) {   // 10 s
        const bool psram = esp_psram_is_initialized();
        const bool mic = audio_tx_sent_count() > 0;
#ifdef AUDIO_LINK_BLE
        const bool link = audio_ble_link_up();
#else
        const bool link = true;
#endif
        if (psram && mic && link) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ok) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA self-test passed, image marked valid");
    } else {
        ESP_LOGW(TAG, "OTA self-test not passed (psram/mic/ble), left PENDING_VERIFY");
    }
    vTaskDelete(nullptr);
}

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP32-S3 INMP441 audio transmitter starting");
    ESP_ERROR_CHECK(audio_transmitter_start());
#ifdef FW_CRASH
    // 回滚验收专用：故意不跑自检任务，启动后崩溃，让 bootloader 回滚
    ESP_LOGW(TAG, "FW_CRASH build: simulating crash in 3s");
    vTaskDelay(pdMS_TO_TICKS(3000));
    volatile int *p = nullptr;
    *p = 1;
#else
    xTaskCreate(ota_boot_verify_task, "ota_verify", 4096, nullptr, 3, nullptr);
#endif
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
