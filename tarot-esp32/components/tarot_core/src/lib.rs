use anyhow::{anyhow, Result};
use esp_idf_svc::eventloop::EspSystemEventLoop;
use esp_idf_svc::nvs::EspDefaultNvsPartition;
use esp_idf_svc::wifi::{ClientConfiguration, Configuration, EspWifi};
use log::{error, info, warn};
use std::net::UdpSocket;
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

const WIFI_SSID: &str = match option_env!("AUDIO_WIFI_SSID") {
    Some(value) => value,
    None => "CHANGE_ME_SSID",
};
const WIFI_PASSWORD: &str = match option_env!("AUDIO_WIFI_PASSWORD") {
    Some(value) => value,
    None => "CHANGE_ME_PASSWORD",
};
const UDP_TARGET: &str = match option_env!("AUDIO_UDP_TARGET") {
    Some(value) => value,
    None => "192.0.2.1:5000",
};
const AUDIO_MAGIC: &[u8; 4] = b"AUD0";
const AUDIO_VERSION: u8 = 1;

static UDP_SOCKET: OnceLock<Mutex<Option<UdpSocket>>> = OnceLock::new();

fn socket_slot() -> &'static Mutex<Option<UdpSocket>> {
    UDP_SOCKET.get_or_init(|| Mutex::new(None))
}

#[repr(C, packed)]
struct AudioHeader {
    magic: [u8; 4],
    version: u8,
    flags: u8,
    header_len: u16,
    sequence: u32,
    sample_rate: u32,
    sample_count: u16,
    payload_len: u16,
}

#[no_mangle]
pub extern "C" fn rust_audio_network_start() {
    esp_idf_svc::log::EspLogger::initialize_default();
    if let Err(error) = start_network() {
        error!("audio network start failed: {error:?}");
    }
}

fn start_network() -> Result<()> {
    if WIFI_SSID == "CHANGE_ME_SSID" || WIFI_PASSWORD == "CHANGE_ME_PASSWORD" {
        warn!("Wi-Fi credentials are placeholders; set AUDIO_WIFI_SSID/AUDIO_WIFI_PASSWORD");
    }
    let peripherals = esp_idf_hal::peripherals::Peripherals::take()
        .map_err(|_| anyhow!("ESP peripherals already taken"))?;
    let sys_loop = EspSystemEventLoop::take()?;
    let nvs = EspDefaultNvsPartition::take()?;
    let mut wifi = EspWifi::new(peripherals.modem, sys_loop, Some(nvs))?;
    wifi.set_configuration(&Configuration::Client(ClientConfiguration {
        ssid: WIFI_SSID.try_into().map_err(|_| anyhow!("SSID too long"))?,
        password: WIFI_PASSWORD
            .try_into()
            .map_err(|_| anyhow!("password too long"))?,
        ..Default::default()
    }))?;
    wifi.start()?;
    wifi.connect()?;
    for _ in 0..100 {
        if wifi.is_connected()? {
            break;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    if !wifi.is_connected()? {
        return Err(anyhow!("Wi-Fi connection timeout"));
    }
    let socket = UdpSocket::bind("0.0.0.0:0")?;
    socket.set_nonblocking(true)?;
    info!("Wi-Fi connected; UDP target={UDP_TARGET}");
    *socket_slot()
        .lock()
        .map_err(|_| anyhow!("UDP mutex poisoned"))? = Some(socket);
    Box::leak(Box::new(wifi));
    Ok(())
}

#[no_mangle]
pub extern "C" fn rust_udp_send_audio(
    data: *const u8,
    len: usize,
    sequence: u32,
    sample_rate: u32,
    sample_count: u16,
) {
    if data.is_null() || len > u16::MAX as usize {
        return;
    }
    let payload = unsafe { std::slice::from_raw_parts(data, len) };
    let header = AudioHeader {
        magic: *AUDIO_MAGIC,
        version: AUDIO_VERSION,
        flags: 0,
        header_len: std::mem::size_of::<AudioHeader>() as u16,
        sequence,
        sample_rate,
        sample_count,
        payload_len: len as u16,
    };
    let header_bytes = unsafe {
        std::slice::from_raw_parts(
            (&header as *const AudioHeader).cast::<u8>(),
            std::mem::size_of::<AudioHeader>(),
        )
    };
    let mut packet = Vec::with_capacity(header_bytes.len() + payload.len());
    packet.extend_from_slice(header_bytes);
    packet.extend_from_slice(payload);
    let Ok(guard) = socket_slot().lock() else {
        return;
    };
    let Some(socket) = guard.as_ref() else { return };
    if let Err(error) = socket.send_to(&packet, UDP_TARGET) {
        if error.kind() != std::io::ErrorKind::WouldBlock {
            warn!("UDP audio send failed: {error}");
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_get_version() -> *const u8 {
    b"ESP32S3_INMP441_UDP_v1\0".as_ptr()
}

#[cfg(test)]
mod tests;
