/*
 * TTS client — EdgeTTS/Kokoro (OpenAI-compatible) text-to-speech via HTTP.
 * Ported from HeyClawy (MIT license).
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *host;
    uint16_t port;
    const char *api_key;
    const char *voice;
    const char *model;
} tts_config_t;

/* Callback to play decoded PCM audio. Caller provides stereo 24kHz 16-bit samples.
 * samples = interleaved L/R, count = number of stereo frames. */
typedef void (*tts_play_cb_t)(const int16_t *samples, size_t bytes);

/* Initialize the TTS client */
esp_err_t tts_init(const tts_config_t *config);

/* Set the audio playback callback (must be set before tts_speak) */
void tts_set_play_cb(tts_play_cb_t cb);

/* Speak text: fetches MP3 from TTS server, decodes, resamples, plays via callback.
 * Blocking call. target_sample_rate is the output rate (e.g. 24000). */
esp_err_t tts_speak(const char *text, int target_sample_rate);

/* Stop any current playback */
void tts_stop(void);

/* Check if TTS is currently playing */
bool tts_is_playing(void);

#ifdef __cplusplus
}
#endif
