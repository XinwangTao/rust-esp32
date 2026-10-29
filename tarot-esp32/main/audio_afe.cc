#include "audio_afe.h"

#include <cstring>

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vad.h"

namespace {
constexpr char TAG[] = "audio_afe";
constexpr int kSampleRate = 16000;
constexpr size_t kUplinkFrame = 320;   // 上行一帧 20 ms，与协议一致
constexpr size_t kOutFifo = 4096;      // fetch 输出暂存，容纳若干 512 块
constexpr size_t kRefRing = 8192;      // 播放参考环形缓冲（samples）
constexpr size_t kMicStage = 2048;     // 麦克风攒块缓冲

const esp_afe_sr_iface_t *s_afe = nullptr;
esp_afe_sr_data_t *s_data = nullptr;
int s_feed_chunk = 0;                  // get_feed_chunksize()，S3 上通常是 512（32 ms）
int s_fetch_chunk = 0;                 // get_fetch_chunksize()

int16_t *s_in = nullptr;               // 交错输入：mic / ref 交替
int16_t *s_mic_stage = nullptr;        // 攒够 feed_chunk 再送
size_t s_mic_len = 0;
int16_t *s_out = nullptr;              // fetch 输出 FIFO
size_t s_out_len = 0;

int16_t *s_ref_ring = nullptr;         // tx_write 生产、采集线程消费
volatile size_t s_ref_w = 0;
volatile size_t s_ref_r = 0;
volatile size_t s_ref_fill = 0;

volatile bool s_speech = false;
volatile uint32_t s_blocks = 0;
volatile uint32_t s_speech_blocks = 0;
volatile uint32_t s_failed = 0;
volatile uint32_t s_ref_under = 0;
volatile uint64_t s_us_total = 0;
volatile uint32_t s_us_max = 0;
// 诊断：最近一次 feed/fetch 的返回码，以及连续失败计数
volatile int s_last_feed = 0;
volatile int s_last_ret = 0;
volatile uint32_t s_consec_fail = 0;
volatile bool s_disabled = false;   // 连续失败太多次就整条停掉，别把采集拖死
constexpr uint32_t kMaxConsecFail = 10;

// 8 字节对齐的小缓冲放内部 RAM：喂/取都在采集任务里同步跑，走 PSRAM 只会拖慢
void *alloc_internal(size_t bytes) {
    return heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bool fill_config(afe_config_t *cfg) {
    std::memset(cfg, 0, sizeof(*cfg));
    if (!afe_parse_input_format("MR", &cfg->pcm_config)) {   // 1 麦 + 1 参考
        ESP_LOGE(TAG, "input format MR rejected");
        return false;
    }
    // AEC：低开销模式，滤波长度与老链路保持一致，NLP 用默认的激进档
    cfg->aec_init = true;
    cfg->aec_mode = AEC_MODE_SR_LOW_COST;
    cfg->aec_filter_length = 4;
    cfg->aec_nlp_level = AEC_NLP_LEVEL_AGGR;
    // 单麦没有阵列，空间增强一律关掉
    cfg->se_init = false;
    // 降噪沿用 WebRTC NS（和老链路同源，便于对照）
    cfg->ns_init = true;
    cfg->ns_model_name = nullptr;
    cfg->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    // VAD：不挂神经网络模型，用 WebRTC VAD；起止门限与 PC 端老参数对齐
    cfg->vad_init = true;
    cfg->vad_mode = VAD_MODE_1;
    cfg->vad_model_name = nullptr;
    cfg->vad_min_speech_ms = 128;
    cfg->vad_min_noise_ms = 800;
    cfg->vad_delay_ms = 128;
    cfg->vad_mute_playback = false;   // 全双工：播放期间也要判语音
    cfg->vad_enable_channel_trigger = false;
    // 唤醒词本轮不做
    cfg->wakenet_init = false;
    // AGC：把 2–3 米的小声抬起来，比一味压低能量阈值干净
    cfg->agc_init = true;
    cfg->agc_mode = AFE_AGC_MODE_WEBRTC;
    cfg->agc_compression_gain_db = 9;
    cfg->agc_target_level_dbfs = 3;
    cfg->afe_mode = AFE_MODE_LOW_COST;
    cfg->afe_type = AFE_TYPE_VC;      // 16 kHz 语音通信，含非线性降噪
    // 注：AFE_TYPE_FD 在本机（esp-sr 2.4.7 + IDF 5.3.1）上 fetch 恒定 ret=-1，
    // 配置项（块 512、16 kHz、2 通道）与 feed 返回值都正常，只有输出侧拿不到数据。
    // se 任务优先级必须高于采集任务（采集是 7）。低于它就会被饿死：se 任务拿不到
    // CPU 就写不出 rb_out，fetch 只能一直等到超时（实测 ret=-1、每次卡 1.5~2 s）。
    cfg->afe_perferred_core = 0;
    cfg->afe_perferred_priority = 9;
    cfg->afe_ringbuf_size = 5;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_linear_gain = 1.0f;
    cfg->debug_init = false;
    cfg->fixed_first_channel = true;
    cfg->fixed_output_channel = true;
    cfg->output_playback_channel = false;
    return true;
}

// 跑一次 feed+fetch，把结果追加进输出 FIFO
void run_block() {
    const int chunk = s_feed_chunk;
    for (int i = 0; i < chunk; ++i) {
        s_in[2 * i] = s_mic_stage[i];
        int16_t ref = 0;
        if (s_ref_fill > 0) {
            ref = s_ref_ring[s_ref_r];
            s_ref_r = (s_ref_r + 1) % kRefRing;
            --s_ref_fill;
        } else {
            ++s_ref_under;
        }
        s_in[2 * i + 1] = ref;
    }

    const int64_t t0 = esp_timer_get_time();
    const int feed_ret = s_afe->feed(s_data, s_in);
    afe_fetch_result_t *res = s_afe->fetch(s_data);
    const int64_t elapsed = esp_timer_get_time() - t0;
    s_us_total += static_cast<uint64_t>(elapsed);
    if (static_cast<uint32_t>(elapsed) > s_us_max) s_us_max = static_cast<uint32_t>(elapsed);
    s_last_feed = feed_ret;
    s_last_ret = (res == nullptr) ? -999 : res->ret_value;

    if (res == nullptr || res->ret_value != 0 || res->data == nullptr || res->data_size <= 0) {
        ++s_failed;
        if (++s_consec_fail >= kMaxConsecFail) {
            // 连续失败说明这条链跑不起来。停掉它，让采集直接走直通，
            // 至少保住 50 包/秒的上行——卡在 7 包/秒比没有降噪糟糕得多。
            s_disabled = true;
            ESP_LOGE(TAG, "AFE disabled after %u failures (feed=%d ret=%d)",
                     static_cast<unsigned>(s_failed), s_last_feed, s_last_ret);
        }
        return;
    }
    s_consec_fail = 0;
    s_speech = (res->vad_state == VAD_SPEECH);
    if (s_speech) ++s_speech_blocks;
    ++s_blocks;

    const size_t n = static_cast<size_t>(res->data_size) / sizeof(int16_t);
    if (s_out_len + n > kOutFifo) s_out_len = 0;   // 溢出丢掉旧数据，宁可短暂断一下也不越界
    std::memcpy(s_out + s_out_len, res->data, n * sizeof(int16_t));
    s_out_len += n;
}

// 启动时先喂几块静音把输出 FIFO 垫起来，否则第一秒会来回欠载
void prime() {
    std::memset(s_mic_stage, 0, static_cast<size_t>(s_feed_chunk) * sizeof(int16_t));
    s_mic_len = static_cast<size_t>(s_feed_chunk);
    for (int k = 0; k < 3; ++k) {
        run_block();
        s_mic_len = static_cast<size_t>(s_feed_chunk);
    }
    s_mic_len = 0;
}
}  // namespace

extern "C" bool audio_afe_start(void) {
    if (s_data != nullptr) return true;

    static afe_config_t cfg;
    if (!fill_config(&cfg)) return false;
    afe_config_check(&cfg);   // 让库修掉互相冲突的配置项（比如 FD 与 se/agc 的搭配）

    const esp_afe_sr_iface_t *iface = esp_afe_handle_from_config(&cfg);
    if (iface == nullptr) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config returned null");
        return false;
    }
    s_afe = iface;

    esp_afe_sr_data_t *data = s_afe->create_from_config(&cfg);
    if (data == nullptr) {
        ESP_LOGE(TAG, "create_from_config failed; internal_free=%u psram_free=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        s_afe = nullptr;
        return false;
    }
    s_data = data;

    s_feed_chunk = s_afe->get_feed_chunksize(s_data);
    s_fetch_chunk = s_afe->get_fetch_chunksize(s_data);
    if (s_feed_chunk <= 0 || s_fetch_chunk <= 0) {
        ESP_LOGE(TAG, "bad chunk size feed=%d fetch=%d", s_feed_chunk, s_fetch_chunk);
        s_afe->destroy(s_data);
        s_data = nullptr;
        s_afe = nullptr;
        return false;
    }

    s_in = static_cast<int16_t *>(alloc_internal(static_cast<size_t>(s_feed_chunk) * 2 * sizeof(int16_t)));
    s_mic_stage = static_cast<int16_t *>(alloc_internal(kMicStage * sizeof(int16_t)));
    s_out = static_cast<int16_t *>(alloc_internal(kOutFifo * sizeof(int16_t)));
    s_ref_ring = static_cast<int16_t *>(alloc_internal(kRefRing * sizeof(int16_t)));
    if (s_in == nullptr || s_mic_stage == nullptr || s_out == nullptr || s_ref_ring == nullptr) {
        ESP_LOGE(TAG, "buffer alloc failed; internal_free=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        s_afe->destroy(s_data);
        s_data = nullptr;
        s_afe = nullptr;
        return false;
    }
    std::memset(s_ref_ring, 0, kRefRing * sizeof(int16_t));

    s_mic_len = 0;
    s_out_len = 0;
    prime();

    ESP_LOGI(TAG, "AFE ready: %d Hz feed=%d fetch=%d ch=%d fd+ns+vad+agc",
             s_afe->get_samp_rate(s_data), s_feed_chunk, s_fetch_chunk,
             s_afe->get_channel_num(s_data));
    return true;
}

extern "C" bool audio_afe_ready(void) { return s_data != nullptr; }

extern "C" void audio_afe_feed_ref(const int16_t *pcm, size_t count) {
    if (s_ref_ring == nullptr || pcm == nullptr) return;
    for (size_t i = 0; i < count; ++i) {
        if (s_ref_fill >= kRefRing) {          // 满了就挤掉最旧的，参考宁可丢也不能卡住播放
            s_ref_r = (s_ref_r + 1) % kRefRing;
            --s_ref_fill;
        }
        s_ref_ring[s_ref_w] = pcm[i];
        s_ref_w = (s_ref_w + 1) % kRefRing;
        ++s_ref_fill;
    }
}

extern "C" bool audio_afe_process(int16_t *samples, size_t count, bool *speech) {
    if (s_data == nullptr || s_disabled || samples == nullptr || count == 0) return false;

    // 先收下这一帧麦克风
    if (s_mic_len + count > kMicStage) s_mic_len = 0;   // 不该发生，保险
    std::memcpy(s_mic_stage + s_mic_len, samples, count * sizeof(int16_t));
    s_mic_len += count;

    // 够一块就处理
    while (s_mic_len >= static_cast<size_t>(s_feed_chunk)) {
        run_block();
        std::memmove(s_mic_stage, s_mic_stage + s_feed_chunk,
                     (s_mic_len - static_cast<size_t>(s_feed_chunk)) * sizeof(int16_t));
        s_mic_len -= static_cast<size_t>(s_feed_chunk);
    }

    if (s_out_len < kUplinkFrame) {
        // 启动初期或 fetch 连续失败：让调用方回退，别把静音当音频发出去
        return false;
    }
    std::memcpy(samples, s_out, kUplinkFrame * sizeof(int16_t));
    std::memmove(s_out, s_out + kUplinkFrame, (s_out_len - kUplinkFrame) * sizeof(int16_t));
    s_out_len -= kUplinkFrame;
    if (speech != nullptr) *speech = s_speech;
    return true;
}

extern "C" void audio_afe_stats(uint32_t *blocks, uint32_t *speech_blocks, uint32_t *failed,
                                uint32_t *ref_underflow) {
    if (blocks != nullptr) *blocks = s_blocks;
    if (speech_blocks != nullptr) *speech_blocks = s_speech_blocks;
    if (failed != nullptr) *failed = s_failed;
    if (ref_underflow != nullptr) *ref_underflow = s_ref_under;
}

extern "C" void audio_afe_timing(uint32_t *avg_us, uint32_t *max_us) {
    if (avg_us != nullptr) *avg_us = s_blocks ? static_cast<uint32_t>(s_us_total / s_blocks) : 0;
    if (max_us != nullptr) *max_us = s_us_max;
}

extern "C" int audio_afe_feed_chunk(void) { return s_feed_chunk; }

extern "C" void audio_afe_diag(int *feed_ret, int *fetch_ret, int *disabled, int *feed_chunk,
                               int *fetch_chunk, int *rate, int *channels) {
    if (feed_ret != nullptr) *feed_ret = s_last_feed;
    if (fetch_ret != nullptr) *fetch_ret = s_last_ret;
    if (disabled != nullptr) *disabled = s_disabled ? 1 : 0;
    if (feed_chunk != nullptr) *feed_chunk = s_feed_chunk;
    if (fetch_chunk != nullptr) *fetch_chunk = s_fetch_chunk;
    if (rate != nullptr) *rate = (s_data != nullptr) ? s_afe->get_samp_rate(s_data) : 0;
    if (channels != nullptr) *channels = (s_data != nullptr) ? s_afe->get_channel_num(s_data) : 0;
}
