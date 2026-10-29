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

idf_component_register(
    SRCS "audio_main.cc" "audio_transmitter.cc" "audio_speaker.cc" "audio_aec.cc" "audio_ns.cc" "audio_vad.cc"
    INCLUDE_DIRS "." "ffi"
    REQUIRES tarot_core
    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif driver esp_driver_i2s esp_psram esp-sr
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SPEAKER_BCLK=${SPEAKER_BCLK}
    SPEAKER_WS=${SPEAKER_WS}
    SPEAKER_DOUT=${SPEAKER_DOUT}
    SPEAKER_EN=${SPEAKER_EN}
    SPEAKER_UDP_PORT=${SPEAKER_UDP_PORT}
)
