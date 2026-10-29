#include "audio_ble.h"

#include <cstdio>
#include <cstring>

#include "audio_ns.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_speaker.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace {
constexpr char TAG[] = "audio_ble";
constexpr size_t kFrameSamples = 320;   // 20 ms @16 kHz
constexpr size_t kAdpcmBytes = 160;     // 320 个 4 bit
constexpr size_t kHeader = 6;           // seq(2) flags(1) pred(2) index(1)
constexpr size_t kPacket = kHeader + kAdpcmBytes;

uint8_t s_own_addr_type = 0;
volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
uint16_t s_tx_handle = 0;
uint16_t s_rx_handle = 0;
volatile bool s_subscribed = false;
volatile uint16_t s_mtu = 23;
uint16_t s_seq = 0;
volatile uint32_t s_sent = 0;
// 两种丢弃分开计数：没订阅时的丢弃是正常的（板子在等 PC 连），
// 发送失败才是链路/缓冲问题。混在一起看会误判成严重丢包。
volatile uint32_t s_drop_nolink = 0;
volatile uint32_t s_drop_fail = 0;
volatile uint32_t s_key_presses = 0;   // BOOT 键按下次数，写进 STAT 供 PC 端核对
volatile uint32_t s_rx_frames = 0;   // 下行收到的 TTS 帧数



// IMA ADPCM 查表
const int8_t kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                -1, -1, -1, -1, 2, 4, 6, 8};
const int16_t kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,
    21,    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,
    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,
    173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,
    494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,
    1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,
    4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};

// 下行 ADPCM 解码：与 encode_frame 对称，按帧独立，不保留跨帧状态
void decode_frame(const uint8_t *in, int16_t *pcm) {
    int pred = static_cast<int16_t>(in[3] | (in[4] << 8));
    int index = in[5];
    if (index > 88) index = 88;
    pcm[0] = static_cast<int16_t>(pred);
    const uint8_t *d = in + kHeader;
    for (size_t n = 0; n + 1 < kFrameSamples; ++n) {
        const uint8_t byte = d[n >> 1];
        const uint8_t nib = ((n & 1) == 0) ? (byte >> 4) : (byte & 0x0F);
        const int step = kStepTable[index];
        int diff = step >> 3;
        if (nib & 4) diff += step;
        if (nib & 2) diff += step >> 1;
        if (nib & 1) diff += step >> 2;
        pred += (nib & 8) ? -diff : diff;
        if (pred > 32767) pred = 32767;
        if (pred < -32768) pred = -32768;
        index += kIndexTable[nib];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        pcm[n + 1] = static_cast<int16_t>(pred);
    }
}

// 一帧独立编码：预测器取本帧第一个采样，步长索引从 0 开始。
// 不做跨帧预测，丢一包只坏 20 ms。
void encode_frame(const int16_t *pcm, uint8_t flags, uint8_t *out) {
    int pred = pcm[0];
    // 初始步长索引按首帧斜率估：从 0 起步的话，一个有幅度的信号第一下就会把
    // 索引推到 88（最粗档），之后整帧都在粗量化里出不来，SNR 直接变负。
    int index = 0;
    const int slope = (pcm[1] > pcm[0]) ? (pcm[1] - pcm[0]) : (pcm[0] - pcm[1]);
    while (index < 88 && kStepTable[index] < slope) ++index;

    out[0] = static_cast<uint8_t>(s_seq & 0xFF);
    out[1] = static_cast<uint8_t>(s_seq >> 8);
    out[2] = flags;
    out[3] = static_cast<uint8_t>(pred & 0xFF);
    out[4] = static_cast<uint8_t>((pred >> 8) & 0xFF);
    out[5] = static_cast<uint8_t>(index);
    ++s_seq;

    uint8_t *p = out + kHeader;
    std::memset(p, 0, kAdpcmBytes);
    for (size_t i = 1; i < kFrameSamples; ++i) {
        const int step = kStepTable[index];
        int diff = pcm[i] - pred;
        const int sign = (diff < 0) ? 8 : 0;
        if (sign) diff = -diff;
        int delta = 0;
        int vpdiff = step >> 3;
        if (diff >= step) {
            delta |= 4;
            diff -= step;
            vpdiff += step;
        }
        const int step2 = step >> 1;
        if (diff >= step2) {
            delta |= 2;
            diff -= step2;
            vpdiff += step2;
        }
        const int step3 = step2 >> 1;
        if (diff >= step3) {
            delta |= 1;
            vpdiff += step3;
        }
        pred += sign ? -vpdiff : vpdiff;
        if (pred > 32767) pred = 32767;
        if (pred < -32768) pred = -32768;
        index += kIndexTable[delta & 0x0F];
        if (index < 0) index = 0;
        if (index > 88) index = 88;

        const size_t n = i - 1;   // 第 n 个 nibble
        // 符号位必须并进 nibble：漏了它解码端永远只加不减，会一路漂到满幅
        const uint8_t nib = static_cast<uint8_t>((delta | sign) & 0x0F);
        if ((n & 1) == 0) {
            p[n >> 1] = static_cast<uint8_t>(nib << 4);   // 先放高半字节
        } else {
            p[n >> 1] |= nib;
        }
    }
}

void send_flat(const uint8_t *buf, size_t len) {
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        ++s_drop_nolink;
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
    if (om == nullptr) {
        ++s_drop_fail;
        return;
    }
    const int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
    if (rc != 0) {
        ++s_drop_fail;   // om 已被 notify_custom 释放
        return;
    }
    ++s_sent;
}

void reply_status() {
    char text[192];
    const int n = snprintf(text, sizeof(text),
                           "STAT ble=on mtu=%u conn=%d sub=%d sent=%u drop_nolink=%u "
                           "drop_fail=%u rx=%u key=%u\n",
                           static_cast<unsigned>(s_mtu), s_conn == BLE_HS_CONN_HANDLE_NONE ? 0 : 1,
                           s_subscribed ? 1 : 0, static_cast<unsigned>(s_sent),
                           static_cast<unsigned>(s_drop_nolink), static_cast<unsigned>(s_drop_fail),
                           static_cast<unsigned>(s_rx_frames), static_cast<unsigned>(s_key_presses));
    if (n > 0) send_flat(reinterpret_cast<const uint8_t *>(text), static_cast<size_t>(n));
}

// 板载 BOOT 键（GPIO0）当实体开关：按下并稳定 40 ms 就通过 BLE 发一条 VI 帧，
// PC 端收到后切换语音输入。运行期按 BOOT 不会进下载模式，只有复位瞬间按住才会。
constexpr gpio_num_t kBootKey = GPIO_NUM_0;

void send_vi() {
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) return;
    static const uint8_t kMsg[3] = {'V', 'I', 10};   // 三个字节：V I 换行
    send_flat(kMsg, sizeof(kMsg));
}

void boot_key_task(void *) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << static_cast<int>(kBootKey);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    int stable = 1;    // 上拉，未按下 = 1
    int cnt = 0;
    while (true) {
        const int lv = gpio_get_level(kBootKey);
        if (lv != stable) {
            if (++cnt >= 2) {          // 2 x 20 ms 去抖
                stable = lv;
                cnt = 0;
                if (lv == 0) {
                    ++s_key_presses;
                    ESP_LOGI(TAG, "boot key pressed (total %u)", static_cast<unsigned>(s_key_presses));
                    send_vi();
                }
            }
        } else {
            cnt = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

int gatt_access(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    const uint16_t total = OS_MBUF_PKTLEN(ctxt->om);

    // 下行音频：166 字节 = 6 字节头 + 160 字节 ADPCM，解出 320 采样直接送扬声器。
    // 播放同时 tx_write 会把 PCM 喂给 AEC 当参考，所以全双工下行不会被麦克风采回去。
    if (total == kPacket) {
        uint8_t frame[kPacket];
        if (os_mbuf_copydata(ctxt->om, 0, kPacket, frame) != 0) return BLE_ATT_ERR_UNLIKELY;
        int16_t pcm[kFrameSamples];
        decode_frame(frame, pcm);
        speaker_write(pcm, kFrameSamples);
        ++s_rx_frames;
        return 0;
    }

    uint8_t buf[32];
    uint16_t len = total;
    if (len > sizeof(buf)) len = sizeof(buf);
    if (os_mbuf_copydata(ctxt->om, 0, len, buf) != 0) return BLE_ATT_ERR_UNLIKELY;

    if (len >= 4 && std::memcmp(buf, "STAT", 4) == 0) {
        reply_status();
    } else if (len >= 4 && std::memcmp(buf, "TONE", 4) == 0) {
        // 与 UDP 侧同名控制帧：重播自检音，用来确认扬声器链路而不接串口
        speaker_play_tone();
    } else if (len >= 5 && std::memcmp(buf, "NS:", 3) == 0) {
        // 与 UDP 侧同名控制帧：运行期旁路设备端降噪，做 A/B 对照
        const bool on = (buf[3] == 'o' && buf[4] == 'n');
        audio_ns_set_enabled(on);
        uint8_t ack[16];
        const int n = snprintf(reinterpret_cast<char *>(ack), sizeof(ack), "NS %s", on ? "on" : "off");
        if (n > 0) send_flat(ack, static_cast<size_t>(n));
    } else if (len >= 5 && std::memcmp(buf, "GPIO?", 5) == 0) {
        // 诊断用：把候选引脚按「输入 + 上拉」读一遍，用来确认板子上到底有没有
        // 实体按键、接在哪只脚。只读，不改任何已用引脚的功能；确认后这条分支可以留着。
        static bool configured = false;
        static constexpr gpio_num_t kPins[] = {
            GPIO_NUM_0,  GPIO_NUM_1,  GPIO_NUM_2,  GPIO_NUM_3,  GPIO_NUM_8,  GPIO_NUM_9,
            GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_17,
            GPIO_NUM_18, GPIO_NUM_21,
        };
        if (!configured) {
            for (gpio_num_t pin : kPins) {
                gpio_config_t io = {};
                io.pin_bit_mask = 1ULL << static_cast<int>(pin);
                io.mode = GPIO_MODE_INPUT;
                io.pull_up_en = GPIO_PULLUP_ENABLE;
                io.pull_down_en = GPIO_PULLDOWN_DISABLE;
                io.intr_type = GPIO_INTR_DISABLE;
                gpio_config(&io);
            }
            configured = true;
        }
        char text[160];
        int n = snprintf(text, sizeof(text), "GPIO");
        for (gpio_num_t pin : kPins) {
            if (n <= 0 || n >= static_cast<int>(sizeof(text)) - 10) break;
            n += snprintf(text + n, sizeof(text) - n, " %d=%d", static_cast<int>(pin),
                          gpio_get_level(pin));
        }
        if (n > 0) {
            n += snprintf(text + n, sizeof(text) - n, "\n");
            send_flat(reinterpret_cast<const uint8_t *>(text), static_cast<size_t>(n));
        }
    } else if (len >= 4 && std::memcmp(buf, "PING", 4) == 0) {
        // 只回显 token（跳过 "PING " 前缀）。注意 buf 不是以 0 结尾的，
        // 直接 %s 会读到越界垃圾，所以先拷进带终止符的缓冲。
        char token[24] = {0};
        const uint16_t off = (len > 5) ? 5 : 4;
        uint16_t tl = (len > off) ? static_cast<uint16_t>(len - off) : 0;
        if (tl > sizeof(token) - 1) tl = sizeof(token) - 1;
        if (tl > 0) std::memcpy(token, buf + off, tl);
        uint8_t text[48];
        const int n = snprintf(reinterpret_cast<char *>(text), sizeof(text), "PONG %s rx=%u",
                               token, static_cast<unsigned>(s_rx_frames));
        if (n > 0) send_flat(text, static_cast<size_t>(n));
    }
    return 0;
}

// 128 位 UUID 用 Nordic UART Service，通用工具（nRF Connect 等）能直接认出来
const ble_uuid128_t kSvcUuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
                     0x01, 0x00, 0x40, 0x6e);
const ble_uuid128_t kRxUuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
                     0x02, 0x00, 0x40, 0x6e);
const ble_uuid128_t kTxUuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
                     0x03, 0x00, 0x40, 0x6e);

ble_gatt_chr_def kChars[] = {
    {
        .uuid = &kTxUuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_tx_handle,
    },
    {
        .uuid = &kRxUuid.u,
        .access_cb = gatt_access,
        .flags = static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP),
        .val_handle = &s_rx_handle,
    },
    {0},
};

ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = kChars,
    },
    {0},
};

void start_advertising() {
    // 广播包只有 31 字节：128 位 UUID 占 18、名字占 17，放一起会超长，
    // ble_gap_adv_set_fields 直接返回 EMSGSIZE 且不广播。名字必须拆到扫描响应里。
    struct ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids128 = &kSvcUuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp{};
    rsp.name = reinterpret_cast<const uint8_t *>(ble_svc_gap_device_name());
    rsp.name_len = static_cast<uint8_t>(std::strlen(reinterpret_cast<const char *>(rsp.name)));
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // 15 ms 连接间隔：50 包/秒下约占 76% 的连接事件，比 7.5 ms 省电、比 30 ms 有余量。
    // 这里是「建议值」，实际以 PC 侧请求为准。
    rc = ble_gap_adv_start(s_own_addr_type, nullptr, BLE_HS_FOREVER, &params, [](struct ble_gap_event *event,
                                                                                 void *) -> int {
        switch (event->type) {
            case BLE_GAP_EVENT_CONNECT:
                if (event->connect.status == 0) {
                    s_conn = event->connect.conn_handle;
                    s_subscribed = false;
                    ESP_LOGI(TAG, "connected handle=%d", s_conn);
                } else {
                    start_advertising();
                }
                return 0;
            case BLE_GAP_EVENT_DISCONNECT:
                ESP_LOGW(TAG, "disconnected reason=%d", event->disconnect.reason);
                s_conn = BLE_HS_CONN_HANDLE_NONE;
                s_subscribed = false;
                start_advertising();
                return 0;
            case BLE_GAP_EVENT_SUBSCRIBE:
                if (event->subscribe.attr_handle == s_tx_handle) {
                    s_subscribed = event->subscribe.cur_notify != 0;
                    ESP_LOGI(TAG, "notify %s", s_subscribed ? "on" : "off");
                }
                return 0;
            case BLE_GAP_EVENT_MTU:
                s_mtu = event->mtu.value;
                ESP_LOGI(TAG, "mtu=%u", s_mtu);
                return 0;
            default:
                return 0;
        }
    },
                          nullptr);
    if (rc != 0) ESP_LOGE(TAG, "adv_start failed rc=%d", rc);
}

void on_sync() {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);

    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, nullptr);
    char name[24];
    std::snprintf(name, sizeof(name), "MicDesktop-%02X%02X", addr[1], addr[0]);
    ble_svc_gap_device_name_set(name);

    start_advertising();
    ESP_LOGI(TAG, "advertising as %s", name);
}

void on_reset(int reason) { ESP_LOGW(TAG, "host reset reason=%d", reason); }

void host_task(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}
}  // namespace

extern "C" bool audio_ble_start(void) {
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) return true;

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(kSvcs) != 0 || ble_gatts_add_svcs(kSvcs) != 0) {
        ESP_LOGE(TAG, "gatt service registration failed");
        return false;
    }

    nimble_port_freertos_init(host_task);
    xTaskCreate(boot_key_task, "boot_key", 3072, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "nimble started");
    return true;
}

extern "C" bool audio_ble_ready(void) { return true; }

extern "C" bool audio_ble_connected(void) { return s_subscribed; }

extern "C" void audio_ble_send(const int16_t *samples, size_t count, uint8_t flags) {
    if (samples == nullptr || count < kFrameSamples) return;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        ++s_drop_nolink;
        return;
    }
    uint8_t packet[kPacket];
    encode_frame(samples, flags, packet);
    send_flat(packet, sizeof(packet));
}

extern "C" void audio_ble_stats(uint32_t *sent, uint32_t *dropped, uint16_t *mtu) {
    if (sent != nullptr) *sent = s_sent;
    if (dropped != nullptr) *dropped = s_drop_nolink + s_drop_fail;
    if (mtu != nullptr) *mtu = s_mtu;
}
