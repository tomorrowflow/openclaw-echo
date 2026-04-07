/**
 * @file echo_ui.h
 * @brief OpenClaw Echo round-display UI
 *
 * Layer-based UI for 360x360 round display. Each state shows/hides
 * a layer of widgets to minimize dirty regions (reduces SPI DMA pressure).
 */
#pragma once

#include <lvgl.h>

/* Forward-declare app_state_t from main.cpp */
typedef enum {
    STATE_INIT = 0,
    STATE_READY,       /* Brief "OK" after boot, before going dark */
    STATE_IDLE,
    STATE_RECORDING,
    STATE_TRANSCRIBING,
    STATE_CHATTING,
    STATE_SPEAKING,
    STATE_STOPPED,     /* Brief red ring after touch-cancel, auto-dismisses */
    STATE_ERROR,
} app_state_t;

#ifdef __cplusplus
extern "C" {
#endif

/** Create all layers and widgets on the active screen. */
void echo_ui_init(void);

/** Transition UI to match app state (shows/hides layers, starts/stops anims). */
void echo_ui_set_state(app_state_t state);

/** Get the current UI display state (may differ from app state, e.g. STATE_STOPPED). */
app_state_t echo_ui_get_state(void);

/** Update the small status text (e.g. "Connecting WiFi..."). */
void echo_ui_set_status(const char *text);

/** Show the user's transcript text. */
void echo_ui_set_transcript(const char *text);

/** Show the AI response text. */
void echo_ui_set_response(const char *text);

/** Show an error message that auto-dismisses after 3s. */
void echo_ui_set_error(const char *text);

/** Called each main loop iteration (drives animations). */
void echo_ui_tick(void);

#ifdef __cplusplus
}
#endif
