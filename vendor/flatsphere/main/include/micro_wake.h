#pragma once

#include "esp_err.h"
#include "driver/i2s_types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*micro_wake_cb_t)(void);

/**
 * Initialize TFLite models and start wake word detection task on Core 0.
 * @param rx_chan I2S RX channel handle (TDM, 24kHz, 4-ch)
 */
esp_err_t micro_wake_start(i2s_chan_handle_t rx_chan);
void micro_wake_stop(void);

/** Set callback invoked (from Core 0 task) when wake word detected. */
void micro_wake_set_callback(micro_wake_cb_t cb);

/**
 * Start copying 16kHz mono audio into the provided buffer.
 * Called from app task when transitioning to RECORDING state.
 */
void micro_wake_start_recording(int16_t *buf, size_t max_samples);

/** Stop recording and return number of samples written. */
size_t micro_wake_stop_recording(void);

/** Returns true if VAD model detects voice activity. */
bool micro_wake_vad_active(void);

/** Enable/disable wake detection (disable during TTS playback). */
void micro_wake_enable(bool enable);

#ifdef __cplusplus
}
#endif
