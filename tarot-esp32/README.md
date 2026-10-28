# ESP32-S3 INMP441 UDP Transmitter

This `tarot-esp32` application is now configured as the first firmware slice for an ESP32-S3 DevKitM-1 with an external INMP441 module. The original Tarot UI sources remain in the repository history, but the active `main` component is the audio transmitter entry point.

## Current firmware path

- ESP32-S3 target with ESP-IDF and Rust/C++ FFI boundary.
- INMP441 I2S RX: GPIO4 (`SCK`), GPIO5 (`WS`), GPIO6 (`SD`), 16 kHz mono.
- PCM frames: 320 samples per frame, bounded queue, controlled drop counter.
- Wi-Fi STA and non-blocking UDP audio packets.
- GPIO7/8/9 are configured as active-low inputs with internal pull-ups; button actions are intentionally not assigned yet.
- UART0 recovery remains on the DevKitM-1 module pins GPIO43/GPIO44.

## Configuration

Set these at build time without committing credentials:

```powershell
$env:AUDIO_WIFI_SSID = "your-ssid"
$env:AUDIO_WIFI_PASSWORD = "your-password"
$env:AUDIO_UDP_TARGET = "192.168.1.10:5000"
```

The defaults are placeholders and will not connect to a real network.

## Build prerequisites

- ESP-IDF 5.3.x configured for ESP32-S3.
- `espup` Rust toolchain with `xtensa-esp32s3-espidf` installed.
- Rust/Cargo and the repository's existing CMake integration.

The build command is the normal project command:

```powershell
idf.py set-target esp32s3
idf.py build
```

Hardware capture and Wi-Fi throughput still require an ESP32-S3 + INMP441 connected for acceptance testing. The receiver firmware and audio output are out of scope for this slice.
