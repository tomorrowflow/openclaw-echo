/*
 * TTS client — fetches MP3 from Kokoro/EdgeTTS, decodes with minimp3,
 * resamples to board sample rate, and plays through callback.
 * Ported from HeyClawy (MIT license).
 */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"

#include "tts_client.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "tts";

#define MP3_BUF_SIZE  (16 * 1024)

static struct {
    char host[64];
    uint16_t port;
    char api_key[64];
    char voice[32];
    char model[32];
    bool playing;
    bool stop_requested;
    tts_play_cb_t play_cb;
} s_tts;

esp_err_t tts_init(const tts_config_t *config)
{
    tts_play_cb_t saved_cb = s_tts.play_cb;
    memset(&s_tts, 0, sizeof(s_tts));
    s_tts.play_cb = saved_cb;
    strncpy(s_tts.host, config->host, sizeof(s_tts.host) - 1);
    s_tts.port = config->port;
    if (config->api_key) strncpy(s_tts.api_key, config->api_key, sizeof(s_tts.api_key) - 1);
    strncpy(s_tts.voice, config->voice ? config->voice : "alloy", sizeof(s_tts.voice) - 1);
    strncpy(s_tts.model, config->model ? config->model : "tts-1", sizeof(s_tts.model) - 1);

    ESP_LOGI(TAG, "TTS init: %s:%d voice=%s model=%s", s_tts.host, s_tts.port, s_tts.voice, s_tts.model);
    return ESP_OK;
}

void tts_set_play_cb(tts_play_cb_t cb)
{
    s_tts.play_cb = cb;
}

esp_err_t tts_speak(const char *text, int target_sample_rate)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    if (!s_tts.play_cb) {
        ESP_LOGE(TAG, "No play callback set");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tts.playing) {
        ESP_LOGW(TAG, "Already playing, stopping first");
        tts_stop();
    }

    s_tts.playing = true;
    s_tts.stop_requested = false;

    /* Sanitize text: replace problematic Unicode with ASCII equivalents */
    size_t text_len = strlen(text);
    char *clean = malloc(text_len + 1);
    if (!clean) { s_tts.playing = false; return ESP_ERR_NO_MEM; }
    size_t ci = 0;
    for (size_t i = 0; i < text_len; ) {
        uint8_t c = (uint8_t)text[i];
        if (c == 0xE2 && i + 2 < text_len) {
            uint8_t b1 = (uint8_t)text[i+1], b2 = (uint8_t)text[i+2];
            if (b1 == 0x80 && b2 == 0x94) { clean[ci++] = '-'; i += 3; continue; }
            if (b1 == 0x80 && b2 == 0x93) { clean[ci++] = '-'; i += 3; continue; }
            if (b1 == 0x80 && b2 == 0xA6) { clean[ci++] = '.'; clean[ci++] = '.'; clean[ci++] = '.'; i += 3; continue; }
            if (b1 == 0x80 && (b2 == 0x98 || b2 == 0x99)) { clean[ci++] = '\''; i += 3; continue; }
            if (b1 == 0x80 && (b2 == 0x9C || b2 == 0x9D)) { clean[ci++] = '"'; i += 3; continue; }
        }
        if (c == 0xC2 && i + 1 < text_len && (uint8_t)text[i+1] == 0xA0) {
            clean[ci++] = ' '; i += 2; continue;
        }
        clean[ci++] = text[i]; i++;
    }
    clean[ci] = '\0';
    text_len = ci;

    /* Build JSON body */
    size_t json_size = text_len + 256;
    char *json_body = malloc(json_size);
    if (!json_body) {
        free(clean);
        s_tts.playing = false;
        return ESP_ERR_NO_MEM;
    }

    /* Escape special chars for JSON string */
    char *escaped = malloc(text_len * 2 + 1);
    if (!escaped) {
        free(json_body);
        free(clean);
        s_tts.playing = false;
        return ESP_ERR_NO_MEM;
    }
    size_t ei = 0;
    for (size_t i = 0; i < text_len && ei < text_len * 2 - 2; i++) {
        char ch = clean[i];
        if (ch == '"' || ch == '\\') {
            escaped[ei++] = '\\'; escaped[ei++] = ch;
        } else if (ch == '\n') {
            escaped[ei++] = '\\'; escaped[ei++] = 'n';
        } else if (ch == '\r') {
            escaped[ei++] = '\\'; escaped[ei++] = 'r';
        } else if (ch == '\t') {
            escaped[ei++] = '\\'; escaped[ei++] = 't';
        } else if ((uint8_t)ch < 0x20) {
            /* Skip other control chars */
        } else {
            escaped[ei++] = ch;
        }
    }
    escaped[ei] = '\0';
    free(clean);

    snprintf(json_body, json_size,
             "{\"model\":\"%s\",\"input\":\"%s\",\"voice\":\"%s\",\"response_format\":\"mp3\"}",
             s_tts.model, escaped, s_tts.voice);
    free(escaped);

    /* Build URL */
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/v1/audio/speech", s_tts.host, s_tts.port);

    ESP_LOGI(TAG, "TTS request: voice=%s text_len=%d", s_tts.voice, (int)text_len);

    /* Configure HTTP client */
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = (int)(strlen(json_body) + 64),
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        free(json_body);
        s_tts.playing = false;
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (s_tts.api_key[0]) {
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", s_tts.api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    /* Open connection */
    esp_err_t err = esp_http_client_open(client, strlen(json_body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    /* Write request body */
    int wlen = esp_http_client_write(client, json_body, strlen(json_body));
    if (wlen < 0) {
        ESP_LOGE(TAG, "HTTP write failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    /* Download entire MP3 into PSRAM, then decode + play */
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "TTS response: status=%d content_length=%d", status, content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "TTS server error: HTTP %d", status);
        char err_buf[256];
        int rd = esp_http_client_read(client, err_buf, sizeof(err_buf) - 1);
        if (rd > 0) { err_buf[rd] = '\0'; ESP_LOGE(TAG, "Error: %s", err_buf); }
        err = ESP_FAIL;
        goto cleanup;
    }

    /* Allocate MP3 buffer in PSRAM — start with known size or 64KB for chunked */
    size_t mp3_alloc = (content_length > 0) ? (size_t)content_length + 256 : (64 * 1024);
    uint8_t *mp3_buf = heap_caps_malloc(mp3_alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mp3_buf) {
        ESP_LOGE(TAG, "Failed to alloc MP3 buffer (%u bytes)", (unsigned)mp3_alloc);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* Read entire MP3 response, growing buffer if needed for chunked transfers */
    size_t mp3_len = 0;
    while (!s_tts.stop_requested) {
        if (mp3_len + 4096 > mp3_alloc) {
            size_t new_alloc = mp3_alloc * 2;
            uint8_t *new_buf = heap_caps_realloc(mp3_buf, new_alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!new_buf) {
                ESP_LOGW(TAG, "MP3 buffer realloc failed at %u bytes", (unsigned)mp3_alloc);
                break;
            }
            mp3_buf = new_buf;
            mp3_alloc = new_alloc;
        }
        int rd = esp_http_client_read(client, (char *)mp3_buf + mp3_len,
                                      mp3_alloc - mp3_len);
        if (rd <= 0) break;
        mp3_len += rd;
    }
    ESP_LOGI(TAG, "MP3 downloaded: %u bytes (alloc=%u)", (unsigned)mp3_len, (unsigned)mp3_alloc);

    /* Decode MP3 frames and play */
    mp3dec_t *dec = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dec) {
        ESP_LOGE(TAG, "Failed to alloc MP3 decoder");
        free(mp3_buf);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    mp3dec_init(dec);

    /* PCM output buffer: minimp3 outputs max 1152 samples per frame x 2 channels */
    int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        ESP_LOGE(TAG, "Failed to alloc PCM buffer");
        free(dec);
        free(mp3_buf);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* Resample buffer (mono) */
    int16_t *resamp = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resamp) {
        free(pcm);
        free(dec);
        free(mp3_buf);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* Stereo output buffer (separate to avoid overlap with resamp) */
    int16_t *stereo_buf = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stereo_buf) {
        free(resamp);
        free(pcm);
        free(dec);
        free(mp3_buf);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    size_t offset = 0;
    size_t total_pcm_samples = 0;
    bool first_frame = true;
    int src_rate = 24000;

    while (offset < mp3_len && !s_tts.stop_requested) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, mp3_buf + offset,
                                          mp3_len - offset, pcm, &info);
        if (info.frame_bytes == 0) break;
        offset += info.frame_bytes;

        if (samples <= 0) continue;

        if (first_frame) {
            src_rate = info.hz;
            ESP_LOGI(TAG, "MP3: %dHz %dch %dkbps", info.hz, info.channels, info.bitrate_kbps);
            first_frame = false;
        }

        /* Convert stereo to mono if needed */
        int mono_samples = samples;
        if (info.channels == 2) {
            for (int i = 0; i < samples; i++) {
                pcm[i] = (int16_t)(((int32_t)pcm[i*2] + pcm[i*2+1]) / 2);
            }
        }

        /* Resample from src_rate to target_sample_rate, then duplicate mono → stereo */
        int out_mono;
        int16_t *mono_buf;
        if (src_rate != target_sample_rate) {
            out_mono = (int)((int64_t)mono_samples * target_sample_rate / src_rate);
            for (int i = 0; i < out_mono; i++) {
                float pos = (float)i * src_rate / target_sample_rate;
                int idx = (int)pos;
                float frac = pos - idx;
                if (idx + 1 < mono_samples) {
                    resamp[i] = (int16_t)(pcm[idx] * (1.0f - frac) + pcm[idx+1] * frac);
                } else if (idx < mono_samples) {
                    resamp[i] = pcm[idx];
                }
            }
            mono_buf = resamp;
        } else {
            out_mono = mono_samples;
            mono_buf = pcm;
        }

        /* Duplicate mono → stereo for ES8311 output */
        for (int i = 0; i < out_mono; i++) {
            stereo_buf[i * 2 + 0] = mono_buf[i];
            stereo_buf[i * 2 + 1] = mono_buf[i];
        }

        /* Play through callback */
        size_t out_bytes = out_mono * 2 * sizeof(int16_t);
        s_tts.play_cb(stereo_buf, out_bytes);
        total_pcm_samples += out_mono;
    }

    ESP_LOGI(TAG, "TTS playback complete: %u MP3 bytes -> %u PCM samples",
             (unsigned)mp3_len, (unsigned)total_pcm_samples);

    free(dec);
    free(stereo_buf);
    free(resamp);
    free(pcm);
    free(mp3_buf);

cleanup:
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(json_body);
    s_tts.playing = false;
    return err;
}

void tts_stop(void)
{
    s_tts.stop_requested = true;
    int timeout = 50;
    while (s_tts.playing && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool tts_is_playing(void)
{
    return s_tts.playing;
}
