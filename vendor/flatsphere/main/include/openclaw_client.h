/*
 * OpenClaw Gateway WebSocket client.
 * Ported from HeyClawy (MIT license).
 * Simplified for OpenClaw Echo MVP — token auth + chat.send.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENCLAW_STATE_DISCONNECTED = 0,
    OPENCLAW_STATE_CONNECTING,
    OPENCLAW_STATE_AUTHENTICATING,
    OPENCLAW_STATE_CONNECTED,
    OPENCLAW_STATE_CHAT_SENDING,
    OPENCLAW_STATE_CHAT_THINKING,
    OPENCLAW_STATE_CHAT_STREAMING,
    OPENCLAW_STATE_ERROR,
    OPENCLAW_STATE_NOT_PAIRED,
} openclaw_state_t;

typedef struct {
    const char *host;
    uint16_t port;
    const char *token;
    const char *device_key_hex; /* 64-char hex string: ED25519 seed (32 bytes), or NULL */
} openclaw_config_t;

/* Callback for chat response chunks */
typedef void (*openclaw_chat_cb_t)(const char *text, bool is_final);

/* Callback for state changes */
typedef void (*openclaw_state_cb_t)(openclaw_state_t state);

esp_err_t openclaw_init(const openclaw_config_t *config, openclaw_state_cb_t state_cb);
esp_err_t openclaw_connect(void);
esp_err_t openclaw_disconnect(void);
openclaw_state_t openclaw_get_state(void);

/* Send a text chat message and receive streaming response via callback.
 * session_key: if non-NULL, reuses an existing conversation session;
 *              if NULL, generates a unique session key (new conversation). */
esp_err_t openclaw_chat_send(const char *message, const char *session_key, openclaw_chat_cb_t response_cb);

/* Get the session key used by the last chat.send (valid after openclaw_chat_send).
 * Caller should copy if needed — pointer is to internal static buffer. */
const char *openclaw_get_session_key(void);

/* Cancel a pending chat (clears callback, resets state to CONNECTED) */
void openclaw_chat_cancel(void);

/* Get the accumulated full response text (valid after final callback) */
const char *openclaw_get_last_response(void);

/* Get elapsed time since chat was sent (ms) */
uint32_t openclaw_get_thinking_time_ms(void);

/* Get device ID (64-char hex, or empty if no device key). For pairing UI. */
const char *openclaw_get_device_id(void);

#ifdef __cplusplus
}
#endif
