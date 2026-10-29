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

idf_component_register(
    SRCS "audio_main.cc" "audio_transmitter.cc" "audio_speaker.cc" "audio_aec.cc" "audio_ns.cc" "audio_vad.cc" "audio_ble.cc"
    INCLUDE_DIRS "." "ffi"
    REQUIRES tarot_core
    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif driver esp_driver_i2s esp_psram esp-sr bt
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
)
