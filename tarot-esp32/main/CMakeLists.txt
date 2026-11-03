# 扬声器引脚与下行端口（可用环境变量覆盖，如 SPEAKER_BCLK）
# 2026-10-28 实测：板载功放 BCLK=15 / LRCLK=16 / DIN=7，无独立使能脚
set(SPEAKER_BCLK 15)
set(SPEAKER_WS 16)
set(SPEAKER_DOUT 7)
set(SPEAKER_EN -1)
set(SPEAKER_UDP_PORT 5001)
foreach(v SPEAKER_BCLK SPEAKER_WS SPEAKER_DOUT SPEAKER_EN SPEAKER_UDP_PORT)
  if(DEFINED ENV{${v}})
    set(${v} $ENV{${v}})
  endif()
endforeach()

# 音频上行链路二选一：udp（Wi-Fi）或 ble。两者共用射频，同时开会互相抢。
set(AUDIO_LINK "udp" CACHE STRING "audio uplink link: udp or ble")
if(DEFINED ENV{AUDIO_LINK})
  set(AUDIO_LINK $ENV{AUDIO_LINK})
endif()

# 固件标记（STAT 回包 ver=...，OTA 验收用）与坏镜像测试开关
set(FW_TAG "ota-base")
if(DEFINED ENV{FW_TAG})
  set(FW_TAG $ENV{FW_TAG})
endif()

# OTA 下载地址（BLE "OTA" 命令触发时使用），可用环境变量 OTA_URL 覆盖
set(OTA_URL "http://192.168.237.7:8000/tarot-esp32.bin")
if(DEFINED ENV{OTA_URL})
  set(OTA_URL $ENV{OTA_URL})
endif()

# Wi-Fi 凭据（与 Rust 侧同一套 audio.env 变量）
set(WIFI_SSID "")
set(WIFI_PASSWORD "")
if(DEFINED ENV{AUDIO_WIFI_SSID})
  set(WIFI_SSID $ENV{AUDIO_WIFI_SSID})
endif()
if(DEFINED ENV{AUDIO_WIFI_PASSWORD})
  set(WIFI_PASSWORD $ENV{AUDIO_WIFI_PASSWORD})
endif()

idf_component_register(
    SRCS "audio_main.cc" "audio_transmitter.cc" "audio_speaker.cc" "audio_aec.cc" "audio_ns.cc" "audio_vad.cc" "audio_ble.cc" "audio_ota.cc"
    INCLUDE_DIRS "." "ffi"
    REQUIRES tarot_core
    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif driver esp_driver_i2s esp_psram esp-sr bt esp_http_client app_update
)

if(AUDIO_LINK STREQUAL "ble")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE AUDIO_LINK_BLE=1)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SPEAKER_BCLK=${SPEAKER_BCLK}
    SPEAKER_WS=${SPEAKER_WS}
    SPEAKER_DOUT=${SPEAKER_DOUT}
    SPEAKER_EN=${SPEAKER_EN}
    SPEAKER_UDP_PORT=${SPEAKER_UDP_PORT}
    OTA_URL="${OTA_URL}"
    FW_TAG="${FW_TAG}"
    AUDIO_WIFI_SSID="${WIFI_SSID}"
    AUDIO_WIFI_PASSWORD="${WIFI_PASSWORD}"
)

if(DEFINED ENV{FW_CRASH})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_CRASH=1)
endif()
