// 这些变量通过 option_env! 烧进固件，改动后必须触发重新编译，
// 否则改了 audio.env 也不会重新构建，固件里留的还是旧凭据/旧目标。
fn main() {
    for key in ["AUDIO_WIFI_SSID", "AUDIO_WIFI_PASSWORD", "AUDIO_UDP_TARGET"] {
        println!("cargo:rerun-if-env-changed={key}");
    }
}
