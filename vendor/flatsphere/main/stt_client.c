/*
 * STT client — sends WAV audio to faster-whisper HTTP server for transcription.
 * Ported from HeyClawy (MIT license).
 */

#include "stt_client.h"
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "stt";

static char s_host[64] = "";
static int  s_port = 5051;

void stt_init(const char *host, int port)
{
    strncpy(s_host, host, sizeof(s_host) - 1);
    s_port = port;
    ESP_LOGI(TAG, "STT init: %s:%d", s_host, s_port);
}

/* Build a WAV file in PSRAM from 16-bit mono PCM */
static uint8_t *build_wav(const int16_t *pcm, size_t num_samples, int sample_rate, size_t *out_len)
{
    uint32_t data_size = (uint32_t)(num_samples * 2);
    uint32_t file_size = 44 + data_size;
    uint8_t *buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return NULL;

    memcpy(buf, "RIFF", 4);
    uint32_t chunk_size = file_size - 8;
    memcpy(buf + 4, &chunk_size, 4);
    memcpy(buf + 8, "WAVE", 4);

    memcpy(buf + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(buf + 16, &fmt_size, 4);
    uint16_t audio_fmt = 1;
    memcpy(buf + 20, &audio_fmt, 2);
    uint16_t channels = 1;
    memcpy(buf + 22, &channels, 2);
    uint32_t sr = (uint32_t)sample_rate;
    memcpy(buf + 24, &sr, 4);
    uint32_t byte_rate = sr * 2;
    memcpy(buf + 28, &byte_rate, 4);
    uint16_t block_align = 2;
    memcpy(buf + 32, &block_align, 2);
    uint16_t bits = 16;
    memcpy(buf + 34, &bits, 2);

    memcpy(buf + 36, "data", 4);
    memcpy(buf + 40, &data_size, 4);
    memcpy(buf + 44, pcm, data_size);

    *out_len = file_size;
    return buf;
}

/* HTTP response handler — accumulate body into user_data buffer */
typedef struct {
    char  *buf;
    size_t buf_len;
    size_t pos;
} http_resp_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_ctx_t *ctx = (http_resp_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && evt->data_len > 0) {
        size_t avail = ctx->buf_len - ctx->pos - 1;
        size_t copy = (evt->data_len < avail) ? evt->data_len : avail;
        memcpy(ctx->buf + ctx->pos, evt->data, copy);
        ctx->pos += copy;
        ctx->buf[ctx->pos] = '\0';
    }
    return ESP_OK;
}

esp_err_t stt_transcribe(const int16_t *pcm, size_t num_samples,
                         int sample_rate, char *out_text, size_t out_len)
{
    if (!s_host[0]) {
        ESP_LOGE(TAG, "STT not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Build WAV in PSRAM */
    size_t wav_len = 0;
    uint8_t *wav = build_wav(pcm, num_samples, sample_rate, &wav_len);
    if (!wav) {
        ESP_LOGE(TAG, "WAV build OOM");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WAV built: %u bytes (%u samples @ %dHz)",
             (unsigned)wav_len, (unsigned)num_samples, sample_rate);

    /* Prepare URL — whisper-asr-webservice uses /asr with multipart form upload */
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%d/asr?task=transcribe&language=en&output=json&encode=false", s_host, s_port);

    /* Build multipart/form-data body in PSRAM.
     * Format:
     *   --BOUNDARY\r\n
     *   Content-Disposition: form-data; name="audio_file"; filename="audio.wav"\r\n
     *   Content-Type: audio/wav\r\n
     *   \r\n
     *   <WAV data>
     *   \r\n--BOUNDARY--\r\n
     */
    static const char BOUNDARY[] = "----EchoBoundary9876";
    static const char PART_HDR[] =
        "------EchoBoundary9876\r\n"
        "Content-Disposition: form-data; name=\"audio_file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n"
        "\r\n";
    static const char PART_END[] = "\r\n------EchoBoundary9876--\r\n";

    size_t hdr_len = sizeof(PART_HDR) - 1;
    size_t end_len = sizeof(PART_END) - 1;
    size_t body_len = hdr_len + wav_len + end_len;

    char *body = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        free(wav);
        return ESP_ERR_NO_MEM;
    }
    memcpy(body, PART_HDR, hdr_len);
    memcpy(body + hdr_len, wav, wav_len);
    memcpy(body + hdr_len + wav_len, PART_END, end_len);
    free(wav);

    /* Response buffer in PSRAM */
    char *resp_buf = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp_buf) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    http_resp_ctx_t resp_ctx = { .buf = resp_buf, .buf_len = 4096, .pos = 0 };

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .event_handler = http_event_handler,
        .user_data = &resp_ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        free(resp_buf);
        return ESP_FAIL;
    }

    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", BOUNDARY);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, body, body_len);

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp_buf);
        return err;
    }

    ESP_LOGI(TAG, "STT response: status=%d len=%d time=%ldms", status, (int)resp_ctx.pos, (long)elapsed_ms);

    if (status != 200) {
        ESP_LOGE(TAG, "STT error: %s", resp_buf);
        free(resp_buf);
        return ESP_FAIL;
    }

    /* Response is JSON: {"text": "..."} */
    cJSON *json = cJSON_Parse(resp_buf);
    free(resp_buf);
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse STT JSON response");
        return ESP_FAIL;
    }

    cJSON *text_item = cJSON_GetObjectItem(json, "text");
    if (text_item && cJSON_IsString(text_item) && text_item->valuestring[0]) {
        strncpy(out_text, text_item->valuestring, out_len - 1);
        out_text[out_len - 1] = '\0';
        ESP_LOGI(TAG, "Transcribed (%ldms): %s", (long)elapsed_ms, out_text);
    } else {
        ESP_LOGW(TAG, "STT returned empty text");
        strncpy(out_text, "", out_len - 1);
        cJSON_Delete(json);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON_Delete(json);
    return ESP_OK;
}
