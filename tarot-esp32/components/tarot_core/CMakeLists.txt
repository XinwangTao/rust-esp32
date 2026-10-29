set(RUST_LIB_NAME "libtarot_core_rust.a")
set(RUST_TARGET "xtensa-esp32s3-espidf")
set(RUST_TARGET_DIR "C:/t/esp32s3")

idf_component_register(
    SRCS "dummy.c"
    REQUIRES esp_wifi nvs_flash esp_netif esp_event
)

target_link_options(${COMPONENT_LIB} PUBLIC "-Wl,--allow-multiple-definition")

add_library(rust_core STATIC IMPORTED GLOBAL)
set_property(TARGET rust_core PROPERTY IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/${RUST_LIB_NAME})
set_property(TARGET rust_core PROPERTY INTERFACE_LINK_LIBRARIES
    idf::esp_wifi
    idf::nvs_flash
    idf::esp_netif
    idf::esp_event
    idf::lwip
    idf::newlib
)
target_link_libraries(${COMPONENT_LIB} PUBLIC rust_core)

# option_env! 注入的凭据/目标变了必须重编 Rust，否则音频环境改了固件里还是旧值。
# 签名 = audio.env 内容 + 当前环境里的三个变量（构建脚本可用环境变量覆盖）。
set(AUDIO_ENV_FILE "${CMAKE_CURRENT_LIST_DIR}/../../audio.env")
if(EXISTS "${AUDIO_ENV_FILE}")
    file(MD5 "${AUDIO_ENV_FILE}" _audio_env_md5)
else()
    set(_audio_env_md5 "missing")
endif()
set(_audio_sig "${_audio_env_md5}|$ENV{AUDIO_UDP_TARGET}|$ENV{AUDIO_WIFI_SSID}")
set(_audio_stamp "${CMAKE_CURRENT_BINARY_DIR}/audio_env.stamp")
if(EXISTS "${_audio_stamp}")
    file(READ "${_audio_stamp}" _audio_old)
else()
    set(_audio_old "")
endif()
if(NOT _audio_old STREQUAL _audio_sig)
    file(WRITE "${_audio_stamp}" "${_audio_sig}")
endif()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${RUST_LIB_NAME}
    COMMAND ${CMAKE_COMMAND} -E env
        MCU=esp32s3
        CARGO_TARGET_DIR=${RUST_TARGET_DIR}
        cargo +esp build --release --target ${RUST_TARGET}
    COMMAND ${CMAKE_COMMAND} -E copy
        ${RUST_TARGET_DIR}/${RUST_TARGET}/release/libtarot_core.a
        ${CMAKE_CURRENT_BINARY_DIR}/${RUST_LIB_NAME}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs
           ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
           ${CMAKE_CURRENT_SOURCE_DIR}/build.rs
           ${_audio_stamp}
)

add_custom_target(tarot_core_target ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${RUST_LIB_NAME})
add_dependencies(${COMPONENT_LIB} tarot_core_target)
