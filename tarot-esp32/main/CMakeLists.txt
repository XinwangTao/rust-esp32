idf_component_register(
    SRCS "audio_main.cc" "audio_transmitter.cc"
    INCLUDE_DIRS "." "ffi"
    REQUIRES tarot_core
    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif
)
