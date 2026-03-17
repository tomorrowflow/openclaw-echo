/**
 * @file micro_wake.cpp
 * @brief microWakeWord wake word detection using TFLite Micro
 *
 * Runs on Core 0. Reads I2S TDM mic data, extracts MIC1 mono,
 * downsamples 24kHz→16kHz, computes mel spectrogram features via
 * the TFLite microfrontend, and feeds them to hey_snorri.tflite
 * and vad.tflite models for wake word and voice activity detection.
 */

#include "micro_wake.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_common.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern "C" {
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
}

#include <cstring>
#include <cmath>
#include <algorithm>

static const char *TAG = "micro_wake";

/* ── Model data (embedded via EMBED_FILES) ── */
extern const uint8_t hey_snorri_tflite_start[] asm("_binary_hey_snorri_tflite_start");
extern const uint8_t hey_snorri_tflite_end[]   asm("_binary_hey_snorri_tflite_end");
extern const uint8_t vad_tflite_start[]        asm("_binary_vad_tflite_start");
extern const uint8_t vad_tflite_end[]          asm("_binary_vad_tflite_end");

/* ── Model config (from model JSON files) ── */
#define WAKE_SLIDING_WINDOW      10
#define WAKE_TENSOR_ARENA_SIZE   46000
/* probability_cutoff 0.97 -> in uint8 scale: 0.97 * 255 ≈ 247 */
#define WAKE_CUTOFF_U8           247

#define VAD_SLIDING_WINDOW       5
#define VAD_TENSOR_ARENA_SIZE    36000
/* probability_cutoff 0.5 → in uint8 scale: 0.5 * 255 ≈ 128 */
#define VAD_CUTOFF_U8            128

/* ── Audio constants ── */
#define MIC_SAMPLE_RATE     24000
#define TARGET_SAMPLE_RATE  16000
#define NUM_MEL_BINS        40
#define WINDOW_MS           30
#define STEP_MS             10
#define STEP_SAMPLES        (TARGET_SAMPLE_RATE * STEP_MS / 1000)    /* 160 */
#define TDM_CHANNELS        4
#define TDM_FRAMES_PER_READ 256

/* Quantization: uint16 frontend output → int8 model input (matching ESPHome) */
#define QUANT_SCALE  256
#define QUANT_DIV    666

/* ── State ── */
static TaskHandle_t wake_task_handle = nullptr;
static i2s_chan_handle_t s_rx_chan = nullptr;
static volatile micro_wake_cb_t s_wake_cb = nullptr;
static volatile bool s_enabled = true;
static volatile bool s_running = false;

/* Recording state (written by Core 0 task, read by Core 1 app) */
static volatile int16_t *s_rec_buf = nullptr;
static volatile size_t s_rec_max = 0;
static volatile size_t s_rec_pos = 0;
static volatile bool s_recording = false;

/* VAD state */
static volatile bool s_vad_active = false;

/* Sliding window probability buffers */
static uint8_t wake_probs[WAKE_SLIDING_WINDOW];
static int wake_prob_idx = 0;
static uint8_t vad_probs[VAD_SLIDING_WINDOW];
static int vad_prob_idx = 0;
static int wake_cooldown = 0;
#define WAKE_COOLDOWN_WINDOWS 20

/* Microfrontend state */
static struct FrontendState s_frontend;
static bool s_frontend_init = false;

/* TFLite arenas (PSRAM) */
static uint8_t *wake_arena = nullptr;
static uint8_t *vad_arena = nullptr;

/* ── Forward declarations ── */
static void wake_task(void *arg);

/* ── Extract MIC1 from 4-ch TDM and downsample 24kHz→16kHz ── */
static size_t extract_and_downsample(const int16_t *tdm_buf, size_t tdm_frames,
                                      int16_t *out_buf, size_t out_max)
{
    size_t mono_count = tdm_frames;
    size_t out_count = (mono_count * 2) / 3;
    if (out_count > out_max) out_count = out_max;

    for (size_t i = 0; i < out_count; i++) {
        float src_pos = (float)i * 3.0f / 2.0f;
        size_t idx = (size_t)src_pos;
        float frac = src_pos - (float)idx;
        int16_t s0 = (idx < mono_count) ? tdm_buf[idx * TDM_CHANNELS] : 0;
        int16_t s1 = (idx + 1 < mono_count) ? tdm_buf[(idx + 1) * TDM_CHANNELS] : s0;
        out_buf[i] = (int16_t)(s0 * (1.0f - frac) + s1 * frac);
    }
    return out_count;
}

/* ── Initialize microfrontend ── */
static bool init_frontend(void)
{
    struct FrontendConfig config;
    config.window.size_ms = WINDOW_MS;
    config.window.step_size_ms = STEP_MS;
    config.filterbank.num_channels = NUM_MEL_BINS;
    config.filterbank.lower_band_limit = 125.0f;
    config.filterbank.upper_band_limit = 7500.0f;
    config.noise_reduction.smoothing_bits = 10;
    config.noise_reduction.even_smoothing = 0.025f;
    config.noise_reduction.odd_smoothing = 0.06f;
    config.noise_reduction.min_signal_remaining = 0.05f;
    config.pcan_gain_control.enable_pcan = 1;
    config.pcan_gain_control.strength = 0.95f;
    config.pcan_gain_control.offset = 80.0f;
    config.pcan_gain_control.gain_bits = 21;
    config.log_scale.enable_log = 1;
    config.log_scale.scale_shift = 6;

    if (!FrontendPopulateState(&config, &s_frontend, TARGET_SAMPLE_RATE)) {
        ESP_LOGE(TAG, "FrontendPopulateState failed");
        return false;
    }
    s_frontend_init = true;
    return true;
}

/* ── Quantize frontend uint16 output → int8 model input ── */
static inline int8_t quantize_feature(uint16_t val)
{
    int32_t v = ((int32_t)val * QUANT_SCALE + (QUANT_DIV / 2)) / QUANT_DIV;
    v += INT8_MIN;
    return (int8_t)std::clamp(v, (int32_t)INT8_MIN, (int32_t)INT8_MAX);
}

/* ── Sliding window detection ── */
static bool check_wake_detection(uint8_t new_prob)
{
    wake_probs[wake_prob_idx] = new_prob;
    wake_prob_idx = (wake_prob_idx + 1) % WAKE_SLIDING_WINDOW;

    if (wake_cooldown > 0) {
        wake_cooldown--;
        return false;
    }

    /* Average probability over window */
    uint32_t sum = 0;
    for (int i = 0; i < WAKE_SLIDING_WINDOW; i++) sum += wake_probs[i];
    return (sum / WAKE_SLIDING_WINDOW) >= WAKE_CUTOFF_U8;
}

static bool check_vad(uint8_t new_prob)
{
    vad_probs[vad_prob_idx] = new_prob;
    vad_prob_idx = (vad_prob_idx + 1) % VAD_SLIDING_WINDOW;

    /* Max probability over window (ESPHome uses max for VAD) */
    uint8_t max_p = 0;
    for (int i = 0; i < VAD_SLIDING_WINDOW; i++) {
        if (vad_probs[i] > max_p) max_p = vad_probs[i];
    }
    return max_p >= VAD_CUTOFF_U8;
}

/* ── Public C API ── */
extern "C" {

esp_err_t micro_wake_start(i2s_chan_handle_t rx_chan)
{
    if (wake_task_handle) return ESP_ERR_INVALID_STATE;

    s_rx_chan = rx_chan;
    s_running = true;
    s_enabled = true;

    memset(wake_probs, 0, sizeof(wake_probs));
    memset(vad_probs, 0, sizeof(vad_probs));
    wake_prob_idx = 0;
    vad_prob_idx = 0;
    wake_cooldown = 0;

    /* Allocate tensor arenas in PSRAM */
    wake_arena = (uint8_t *)heap_caps_malloc(WAKE_TENSOR_ARENA_SIZE,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    vad_arena = (uint8_t *)heap_caps_malloc(VAD_TENSOR_ARENA_SIZE,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wake_arena || !vad_arena) {
        ESP_LOGE(TAG, "Failed to allocate tensor arenas in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    if (!init_frontend()) {
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        wake_task, "wake_word", 8192, nullptr, 5, &wake_task_handle, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wake word task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wake word detection started on Core 0");
    return ESP_OK;
}

void micro_wake_stop(void)
{
    s_running = false;
    if (wake_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
        vTaskDelete(wake_task_handle);
        wake_task_handle = nullptr;
    }
    if (wake_arena) { free(wake_arena); wake_arena = nullptr; }
    if (vad_arena) { free(vad_arena); vad_arena = nullptr; }
    if (s_frontend_init) {
        FrontendFreeStateContents(&s_frontend);
        s_frontend_init = false;
    }
}

void micro_wake_set_callback(micro_wake_cb_t cb)
{
    s_wake_cb = cb;
}

void micro_wake_start_recording(int16_t *buf, size_t max_samples)
{
    s_rec_buf = buf;
    s_rec_max = max_samples;
    s_rec_pos = 0;
    s_recording = true;
}

size_t micro_wake_stop_recording(void)
{
    s_recording = false;
    size_t pos = s_rec_pos;
    s_rec_buf = nullptr;
    s_rec_max = 0;
    s_rec_pos = 0;
    return pos;
}

bool micro_wake_vad_active(void)
{
    return s_vad_active;
}

void micro_wake_enable(bool enable)
{
    s_enabled = enable;
    if (enable) {
        memset(wake_probs, 0, sizeof(wake_probs));
        wake_prob_idx = 0;
        wake_cooldown = 0;
    }
}

} /* extern "C" */

/* ── Wake word detection task (Core 0) ── */
static void wake_task(void *arg)
{
    (void)arg;

    /* Load TFLite models */
    const tflite::Model *wake_model = tflite::GetModel(hey_snorri_tflite_start);
    const tflite::Model *vad_model = tflite::GetModel(vad_tflite_start);

    if (!wake_model || !vad_model) {
        ESP_LOGE(TAG, "Failed to load TFLite models");
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Models loaded: wake=%u bytes, vad=%u bytes",
             (unsigned)(hey_snorri_tflite_end - hey_snorri_tflite_start),
             (unsigned)(vad_tflite_end - vad_tflite_start));

    /* Create op resolver — microWakeWord v2 streaming models need these 20 ops */
    static tflite::MicroMutableOpResolver<20> resolver;
    resolver.AddCallOnce();
    resolver.AddVarHandle();
    resolver.AddReshape();
    resolver.AddReadVariable();
    resolver.AddStridedSlice();
    resolver.AddConcatenation();
    resolver.AddAssignVariable();
    resolver.AddConv2D();
    resolver.AddMul();
    resolver.AddAdd();
    resolver.AddMean();
    resolver.AddFullyConnected();
    resolver.AddLogistic();
    resolver.AddQuantize();
    resolver.AddDepthwiseConv2D();
    resolver.AddAveragePool2D();
    resolver.AddMaxPool2D();
    resolver.AddPad();
    resolver.AddPack();
    resolver.AddSplitV();

    /* Create allocators and resource variables for streaming models */
    tflite::MicroAllocator *wake_alloc = tflite::MicroAllocator::Create(
        wake_arena, WAKE_TENSOR_ARENA_SIZE);
    tflite::MicroAllocator *vad_alloc = tflite::MicroAllocator::Create(
        vad_arena, VAD_TENSOR_ARENA_SIZE);

    if (!wake_alloc || !vad_alloc) {
        ESP_LOGE(TAG, "Failed to create MicroAllocators");
        vTaskDelete(nullptr);
        return;
    }

    /* Streaming models use VAR_HANDLE/READ_VARIABLE/ASSIGN_VARIABLE —
     * they need MicroResourceVariables for stateful inference */
    tflite::MicroResourceVariables *wake_rv =
        tflite::MicroResourceVariables::Create(wake_alloc, 20);
    tflite::MicroResourceVariables *vad_rv =
        tflite::MicroResourceVariables::Create(vad_alloc, 20);

    if (!wake_rv || !vad_rv) {
        ESP_LOGE(TAG, "Failed to create MicroResourceVariables");
        vTaskDelete(nullptr);
        return;
    }

    /* Create interpreters with resource variables */
    tflite::MicroInterpreter wake_interp(wake_model, resolver,
                                          wake_alloc, wake_rv);
    tflite::MicroInterpreter vad_interp(vad_model, resolver,
                                         vad_alloc, vad_rv);

    if (wake_interp.AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "Wake model AllocateTensors failed");
        vTaskDelete(nullptr);
        return;
    }
    if (vad_interp.AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "VAD model AllocateTensors failed");
        vTaskDelete(nullptr);
        return;
    }

    /* Get input tensor info */
    TfLiteTensor *wake_input = wake_interp.input(0);
    TfLiteTensor *vad_input = vad_interp.input(0);

    int wake_stride = wake_input->dims->data[1];  /* Typically 3 for v2 */
    int vad_stride = vad_input->dims->data[1];

    ESP_LOGI(TAG, "Wake input: [%d, %d, %d], stride=%d",
             wake_input->dims->data[0], wake_input->dims->data[1],
             wake_input->dims->data[2], wake_stride);
    ESP_LOGI(TAG, "VAD input: [%d, %d, %d], stride=%d",
             vad_input->dims->data[0], vad_input->dims->data[1],
             vad_input->dims->data[2], vad_stride);

    /* Audio buffers */
    int16_t tdm_buf[TDM_FRAMES_PER_READ * TDM_CHANNELS];
    int16_t mono_buf[TDM_FRAMES_PER_READ];  /* 16kHz mono after downsample */

    int wake_stride_pos = 0;
    int vad_stride_pos = 0;
    uint32_t feature_count = 0;
    uint32_t invoke_count = 0;

    ESP_LOGI(TAG, "Wake word task running");

    while (s_running) {
        /* Read I2S TDM data */
        size_t bytes_read = 0;
        esp_err_t r = i2s_channel_read(s_rx_chan, tdm_buf, sizeof(tdm_buf),
                                        &bytes_read, pdMS_TO_TICKS(500));
        if (r != ESP_OK || bytes_read == 0) continue;

        size_t tdm_frames = bytes_read / (TDM_CHANNELS * sizeof(int16_t));

        /* Extract MIC1 + downsample to 16kHz */
        size_t mono_count = extract_and_downsample(tdm_buf, tdm_frames,
                                                    mono_buf, TDM_FRAMES_PER_READ);

        /* If recording, copy 16kHz mono to record buffer */
        if (s_recording && s_rec_buf) {
            size_t space = s_rec_max - s_rec_pos;
            size_t to_copy = (mono_count < space) ? mono_count : space;
            memcpy((int16_t *)s_rec_buf + s_rec_pos, mono_buf, to_copy * sizeof(int16_t));
            s_rec_pos += to_copy;
        }

        if (!s_enabled) continue;

        /* Feed samples to frontend one step at a time */
        size_t mono_offset = 0;
        while (mono_offset < mono_count) {
            size_t num_read = 0;
            struct FrontendOutput frontend_out = FrontendProcessSamples(
                &s_frontend, mono_buf + mono_offset,
                mono_count - mono_offset, &num_read);

            if (num_read == 0) break;
            mono_offset += num_read;

            if (frontend_out.size != NUM_MEL_BINS) {
                continue;
            }

            feature_count++;

            /* Quantize features to int8 */
            int8_t features[NUM_MEL_BINS];
            for (int i = 0; i < NUM_MEL_BINS; i++) {
                features[i] = quantize_feature(frontend_out.values[i]);
            }

            /* Feed to wake word model (stride accumulation) */
            int8_t *wake_data = wake_input->data.int8;
            memcpy(wake_data + wake_stride_pos * NUM_MEL_BINS,
                   features, NUM_MEL_BINS);
            wake_stride_pos++;

            if (wake_stride_pos >= wake_stride) {
                wake_stride_pos = 0;
                if (wake_interp.Invoke() == kTfLiteOk) {
                    TfLiteTensor *out = wake_interp.output(0);
                    uint8_t prob = out->data.uint8[0];
                    invoke_count++;

                    /* Log when probability is notable */
                    if (prob > 100) {
                        ESP_LOGI(TAG, "wake prob=%u (invoke #%lu)",
                                 prob, (unsigned long)invoke_count);
                    }

                    if (check_wake_detection(prob)) {
                        ESP_LOGI(TAG, "*** WAKE WORD DETECTED (prob=%u) ***", prob);
                        wake_cooldown = WAKE_COOLDOWN_WINDOWS;
                        memset(wake_probs, 0, sizeof(wake_probs));
                        if (s_wake_cb) {
                            s_wake_cb();
                        }
                    }
                }
            }

            /* Feed to VAD model (stride accumulation) */
            int8_t *vad_data = vad_input->data.int8;
            memcpy(vad_data + vad_stride_pos * NUM_MEL_BINS,
                   features, NUM_MEL_BINS);
            vad_stride_pos++;

            if (vad_stride_pos >= vad_stride) {
                vad_stride_pos = 0;
                if (vad_interp.Invoke() == kTfLiteOk) {
                    TfLiteTensor *out = vad_interp.output(0);
                    uint8_t prob = out->data.uint8[0];
                    s_vad_active = check_vad(prob);
                }
            }
        }  /* while (mono_offset < mono_count) */
    }  /* while (s_running) */

    ESP_LOGI(TAG, "Wake word task exiting");
    vTaskDelete(nullptr);
}
