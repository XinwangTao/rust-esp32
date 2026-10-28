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
)

add_custom_target(tarot_core_target ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${RUST_LIB_NAME})
add_dependencies(${COMPONENT_LIB} tarot_core_target)
