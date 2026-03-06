/*
 * STT (Speech-to-Text) client — sends WAV audio to HTTP STT server,
 * returns transcribed text.
 * Ported from HeyClawy (MIT license).
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize STT client with server endpoint.
 * @param host STT server hostname/IP
 * @param port STT server port
 */
void stt_init(const char *host, int port);

/**
 * @brief Transcribe PCM audio via the STT server.
 *
 * Builds a WAV file from raw PCM, sends it to the STT HTTP endpoint,
 * and returns the transcribed text.
 *
 * @param pcm         Raw 16-bit mono PCM samples
 * @param num_samples Number of samples
 * @param sample_rate Sample rate in Hz (e.g. 16000)
 * @param out_text    Output buffer for transcribed text
 * @param out_len     Size of output buffer
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t stt_transcribe(const int16_t *pcm, size_t num_samples,
                         int sample_rate, char *out_text, size_t out_len);

#ifdef __cplusplus
}
#endif
