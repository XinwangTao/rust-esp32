// Wi-Fi 拉取式 OTA 实现（图：docs/archify/esp32s3-ota_workflow.*）
// 触发：BLE 控制帧 "OTA"；地址来自编译期 OTA_URL（audio.env）。
// 安全性：下载/校验失败只回错误，不切换分区；新固件自检通过前处于 PENDING_VERIFY，
// 崩溃或自检失败会在下次启动被 bootloader 回滚。
#include "audio_ota.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio_ble.h"
#include "audio_transmitter.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace {
constexpr const char *TAG = "audio_ota";
constexpr int kWifiOkBit = BIT0;
constexpr int kWifiFailBit = BIT1;

volatile bool s_running = false;
EventGroupHandle_t s_evt = nullptr;
int s_retry = 0;

void notify(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", buf);
#ifdef AUDIO_LINK_BLE
    audio_ble_send_text(buf);
#endif
}

void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++s_retry <= 5) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_evt, kWifiFailBit);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_evt, kWifiOkBit);
    }
}

// UDP 模式下 Rust 已经起了 Wi-Fi，直接复用；BLE 模式按需启动 STA（与 BLE 共存）
bool ensure_wifi() {
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != nullptr) {
        ESP_LOGI(TAG, "Wi-Fi already initialized (UDP mode), reuse");
        return true;
    }
    ESP_LOGI(TAG, "starting Wi-Fi STA for OTA");
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    esp_event_loop_create_default();   // 已存在时返回 ESP_ERR_INVALID_STATE，忽略
    esp_netif_create_default_wifi_sta();  // 已存在会返回已有句柄
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    s_evt = xEventGroupCreate();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr);

    wifi_config_t wc = {};
    const char *ssid = AUDIO_WIFI_SSID;
    const char *pass = AUDIO_WIFI_PASSWORD;
    strncpy(reinterpret_cast<char *>(wc.sta.ssid), ssid, sizeof(wc.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(wc.sta.password), pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    if (esp_wifi_start() != ESP_OK) return false;

    EventBits_t bits = xEventGroupWaitBits(s_evt, kWifiOkBit | kWifiFailBit, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(20000));
    return (bits & kWifiOkBit) != 0;
}

void ota_task(void *arg) {
    char *url = static_cast<char *>(arg);
    audio_tx_set_paused(true);
    notify("OTA start\n");

    esp_http_client_handle_t client = nullptr;
    esp_ota_handle_t ota = 0;
    bool ok = false;
    const esp_partition_t *update = esp_ota_get_next_update_partition(nullptr);

    do {
        if (update == nullptr) {
            notify("OTA fail:image\n");
            break;
        }
        if (!ensure_wifi()) {
            notify("OTA fail:wifi\n");
            break;
        }
        esp_http_client_config_t http = {};
        http.url = url;
        http.timeout_ms = 30000;
        http.buffer_size = 4096;
        http.keep_alive_enable = false;
        client = esp_http_client_init(&http);
        if (client == nullptr || esp_http_client_open(client, 0) != ESP_OK) {
            notify("OTA fail:http\n");
            break;
        }
        int total = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP status=%d content_length=%d", status, total);
        if (status != 200) {
            notify("OTA fail:http%d\n", status);
            break;
        }
        if (esp_ota_begin(update, total > 0 ? static_cast<size_t>(total) : OTA_SIZE_UNKNOWN, &ota) != ESP_OK) {
            notify("OTA fail:begin\n");
            break;
        }
        char buf[4096];
        int got = 0;
        int pct_last = -1;
        bool write_fail = false;
        while (true) {
            int r = esp_http_client_read(client, buf, sizeof(buf));
            if (r < 0) {
                write_fail = true;   // 网络中断
                notify("OTA fail:http\n");
                break;
            }
            if (r == 0) break;       // 读完
            if (esp_ota_write(ota, buf, r) != ESP_OK) {
                write_fail = true;
                notify("OTA fail:flash\n");
                break;
            }
            got += r;
            if (total > 0) {
                int pct = got * 100 / total;
                if (pct / 10 != pct_last / 10) {
                    pct_last = pct;
                    notify("OTA %d%%\n", pct);
                }
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = nullptr;
        if (write_fail) break;
        if (esp_ota_end(ota) != ESP_OK) {
            notify("OTA fail:verify\n");
            break;
        }
        if (esp_ota_set_boot_partition(update) != ESP_OK) {
            notify("OTA fail:setboot\n");
            break;
        }
        notify("OTA ok rebooting\n");
        ok = true;
    } while (false);

    if (!ok) {
        if (client) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }
        audio_tx_set_paused(false);
        s_running = false;
        free(url);
        vTaskDelete(nullptr);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    free(url);
    vTaskDelete(nullptr);
}
}  // namespace

extern "C" bool audio_ota_start(const char *url) {
    if (s_running) return false;
    s_running = true;
    char *copy = strdup(url != nullptr ? url : OTA_URL);
    if (copy == nullptr) {
        s_running = false;
        return false;
    }
    if (xTaskCreate(ota_task, "audio_ota", 8192, copy, 5, nullptr) != pdPASS) {
        free(copy);
        s_running = false;
        return false;
    }
    return true;
}
