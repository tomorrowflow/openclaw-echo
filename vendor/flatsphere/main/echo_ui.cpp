/**
 * @file echo_ui.cpp
 * @brief OpenClaw Echo round-display UI implementation
 *
 * All widgets placed directly on the screen (no layer containers).
 * Hiding/showing individual widgets keeps dirty regions small,
 * reducing SPI DMA bounce buffer pressure from PSRAM LVGL buffers.
 */

#include "echo_ui.h"
#include <cstring>
#include <cstdio>
#include "esp_log.h"

static const char *TAG = "echo_ui";

/* ── Widgets (all on lv_scr_act, individually hidden/shown) ── */

/* Boot */
static lv_obj_t *boot_label    = nullptr;

/* Ready (brief OK after boot) */
static lv_obj_t *ready_label   = nullptr;
static lv_obj_t *ready_circle  = nullptr;
static lv_anim_t ready_anim;

/* Idle (status label reused for boot status messages) */
static lv_obj_t *idle_status   = nullptr;

/* Active */
static lv_obj_t *active_arc    = nullptr;
static lv_obj_t *active_label  = nullptr;

/* Response */
static lv_obj_t *resp_label    = nullptr;

/* Error */
static lv_obj_t *error_label   = nullptr;

/* ── Animation state ── */
static lv_anim_t arc_anim1;
static lv_anim_t arc_anim2;
static bool arc_anim_running   = false;

/* Error auto-dismiss */
static lv_timer_t *error_timer = nullptr;

/* Stopped auto-dismiss */
static lv_timer_t *stopped_timer = nullptr;

/* Current state */
static app_state_t current_state = STATE_INIT;

/* ── Helpers ── */

static void hide_widget(lv_obj_t *w)
{
    if (w) lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN);
}

static void show_widget(lv_obj_t *w)
{
    if (w) lv_obj_remove_flag(w, LV_OBJ_FLAG_HIDDEN);
}

static void hide_all_widgets(void)
{
    hide_widget(boot_label);
    hide_widget(ready_label);
    hide_widget(ready_circle);
    hide_widget(idle_status);
    hide_widget(active_arc);
    hide_widget(active_label);
    hide_widget(resp_label);
    hide_widget(error_label);
}

/* ── Arc animation callbacks ── */

static void arc_spin_start_cb(void *obj, int32_t v)
{
    lv_arc_set_start_angle((lv_obj_t *)obj, (uint32_t)v);
}

static void arc_spin_end_cb(void *obj, int32_t v)
{
    lv_arc_set_end_angle((lv_obj_t *)obj, (uint32_t)v);
}

static void arc_pulse_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_arc_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_INDICATOR);
}

static void stop_arc_anim(void)
{
    if (arc_anim_running) {
        lv_anim_delete(active_arc, arc_spin_start_cb);
        lv_anim_delete(active_arc, arc_spin_end_cb);
        lv_anim_delete(active_arc, arc_pulse_opa_cb);
        arc_anim_running = false;
    }
}

static void start_arc_spin(uint32_t color_hex)
{
    stop_arc_anim();

    lv_obj_set_style_arc_color(active_arc, lv_color_hex(color_hex), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(active_arc, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_anim_init(&arc_anim1);
    lv_anim_set_var(&arc_anim1, active_arc);
    lv_anim_set_exec_cb(&arc_anim1, arc_spin_start_cb);
    lv_anim_set_values(&arc_anim1, 0, 360);
    lv_anim_set_duration(&arc_anim1, 1500);
    lv_anim_set_repeat_count(&arc_anim1, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&arc_anim1);

    lv_anim_init(&arc_anim2);
    lv_anim_set_var(&arc_anim2, active_arc);
    lv_anim_set_exec_cb(&arc_anim2, arc_spin_end_cb);
    lv_anim_set_values(&arc_anim2, 90, 450);
    lv_anim_set_duration(&arc_anim2, 1500);
    lv_anim_set_repeat_count(&arc_anim2, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&arc_anim2);

    arc_anim_running = true;
}

static void start_arc_pulse(uint32_t color_hex)
{
    stop_arc_anim();

    lv_obj_set_style_arc_color(active_arc, lv_color_hex(color_hex), LV_PART_INDICATOR);
    lv_arc_set_start_angle(active_arc, 0);
    lv_arc_set_end_angle(active_arc, 360);

    lv_anim_init(&arc_anim1);
    lv_anim_set_var(&arc_anim1, active_arc);
    lv_anim_set_exec_cb(&arc_anim1, arc_pulse_opa_cb);
    lv_anim_set_values(&arc_anim1, 80, 255);
    lv_anim_set_duration(&arc_anim1, 800);
    lv_anim_set_playback_duration(&arc_anim1, 800);
    lv_anim_set_repeat_count(&arc_anim1, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&arc_anim1);

    arc_anim_running = true;
}

/* ── Ready circle expand animation ── */

static void ready_circle_size_cb(void *obj, int32_t v)
{
    lv_obj_set_size((lv_obj_t *)obj, v, v);
    lv_obj_center((lv_obj_t *)obj);
}

static void ready_circle_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

/* ── Error auto-dismiss ── */

static void error_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    hide_widget(error_label);
    error_timer = nullptr;
}

static void stopped_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    stop_arc_anim();
    hide_all_widgets();
    current_state = STATE_IDLE;
    stopped_timer = nullptr;
}

/* ── Public API ── */

void echo_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Boot label ── */
    boot_label = lv_label_create(scr);
    lv_obj_set_style_text_color(boot_label, lv_color_hex(0x00AAFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(boot_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_align(boot_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(boot_label);
    lv_label_set_text(boot_label, "OpenClaw\nEcho");

    /* ── Ready label (brief OK after boot) ── */
    ready_label = lv_label_create(scr);
    lv_obj_set_style_text_color(ready_label, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(ready_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_align(ready_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(ready_label);
    lv_label_set_text(ready_label, "OK");
    hide_widget(ready_label);

    /* ── Ready expanding circle (border-only, no fill) ── */
    ready_circle = lv_obj_create(scr);
    lv_obj_remove_style_all(ready_circle);
    lv_obj_set_size(ready_circle, 120, 120);
    lv_obj_center(ready_circle);
    lv_obj_set_style_radius(ready_circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ready_circle, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ready_circle, 8, LV_PART_MAIN);
    lv_obj_set_style_border_color(ready_circle, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_border_opa(ready_circle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(ready_circle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ready_circle, LV_OBJ_FLAG_SCROLLABLE);
    hide_widget(ready_circle);

    /* ── Idle widgets ── */
    idle_status = lv_label_create(scr);
    lv_obj_set_style_text_color(idle_status, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(idle_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(idle_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(idle_status, 240);
    lv_label_set_long_mode(idle_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(idle_status, LV_ALIGN_CENTER, 0, -10);
    lv_label_set_text(idle_status, "Ready");
    hide_widget(idle_status);

    /* ── Active widgets (recording / transcribing / chatting) ── */
    active_arc = lv_arc_create(scr);
    lv_obj_set_size(active_arc, 340, 340);
    lv_obj_center(active_arc);
    lv_arc_set_rotation(active_arc, 270);
    lv_arc_set_range(active_arc, 0, 360);
    lv_arc_set_value(active_arc, 0);
    lv_arc_set_bg_angles(active_arc, 0, 360);
    lv_obj_remove_style(active_arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(active_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(active_arc, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_arc_width(active_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(active_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(active_arc, true, LV_PART_INDICATOR);
    hide_widget(active_arc);

    active_label = lv_label_create(scr);
    lv_obj_set_style_text_color(active_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(active_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(active_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(active_label, 240);
    lv_label_set_long_mode(active_label, LV_LABEL_LONG_WRAP);
    lv_obj_center(active_label);
    lv_label_set_text(active_label, "");
    hide_widget(active_label);

    /* ── Response label ── */
    resp_label = lv_label_create(scr);
    lv_obj_set_style_text_color(resp_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(resp_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(resp_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(resp_label, 240);
    lv_label_set_long_mode(resp_label, LV_LABEL_LONG_WRAP);
    lv_obj_center(resp_label);
    lv_label_set_text(resp_label, "");
    hide_widget(resp_label);

    /* ── Error label ── */
    error_label = lv_label_create(scr);
    lv_obj_set_style_text_color(error_label, lv_color_hex(0xFF4444), LV_PART_MAIN);
    lv_obj_set_style_text_font(error_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(error_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(error_label, 260);
    lv_label_set_long_mode(error_label, LV_LABEL_LONG_WRAP);
    lv_obj_center(error_label);
    lv_label_set_text(error_label, "");
    hide_widget(error_label);

    ESP_LOGI(TAG, "UI initialized");
}

app_state_t echo_ui_get_state(void)
{
    return current_state;
}

void echo_ui_set_state(app_state_t state)
{
    current_state = state;

    stop_arc_anim();

    if (error_timer && state != STATE_ERROR) {
        lv_timer_delete(error_timer);
        error_timer = nullptr;
    }
    if (stopped_timer && state != STATE_STOPPED) {
        lv_timer_delete(stopped_timer);
        stopped_timer = nullptr;
    }

    hide_all_widgets();

    switch (state) {
    case STATE_INIT:
        show_widget(boot_label);
        break;

    case STATE_READY:
        show_widget(ready_label);
        show_widget(ready_circle);
        /* Reset circle to wrap around "OK" text */
        lv_obj_set_size(ready_circle, 120, 120);
        lv_obj_center(ready_circle);
        lv_obj_set_style_border_opa(ready_circle, LV_OPA_COVER, LV_PART_MAIN);
        /* Expand circle from 120px to 380px over 1.2s */
        lv_anim_init(&ready_anim);
        lv_anim_set_var(&ready_anim, ready_circle);
        lv_anim_set_exec_cb(&ready_anim, ready_circle_size_cb);
        lv_anim_set_values(&ready_anim, 120, 380);
        lv_anim_set_duration(&ready_anim, 1200);
        lv_anim_set_path_cb(&ready_anim, lv_anim_path_ease_out);
        lv_anim_start(&ready_anim);
        /* Fade out circle border in last 400ms */
        {
            lv_anim_t fade;
            lv_anim_init(&fade);
            lv_anim_set_var(&fade, ready_circle);
            lv_anim_set_exec_cb(&fade, ready_circle_opa_cb);
            lv_anim_set_values(&fade, 255, 0);
            lv_anim_set_duration(&fade, 400);
            lv_anim_set_delay(&fade, 800);
            lv_anim_start(&fade);
        }
        break;

    case STATE_IDLE:
        /* Dark screen — all widgets hidden, no animations.
         * Backlight is controlled by main.cpp (faded to 0). */
        break;

    case STATE_RECORDING:
        show_widget(active_arc);
        show_widget(active_label);
        lv_label_set_text(active_label, "Listening...");
        start_arc_pulse(0x00FF00);
        break;

    case STATE_TRANSCRIBING:
    case STATE_CHATTING:
        show_widget(active_arc);
        show_widget(active_label);
        lv_label_set_text(active_label, "Thinking...");
        start_arc_spin(0x00AAFF);
        break;

    case STATE_SPEAKING:
        show_widget(active_arc);
        show_widget(resp_label);
        /* Static amber ring — no animation needed (LVGL can't update during TTS) */
        lv_obj_set_style_arc_color(active_arc, lv_color_hex(0xFF8800), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(active_arc, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_arc_set_start_angle(active_arc, 0);
        lv_arc_set_end_angle(active_arc, 360);
        break;

    case STATE_STOPPED:
        show_widget(active_arc);
        show_widget(active_label);
        lv_label_set_text(active_label, "Stopped");
        /* Static red ring */
        lv_obj_set_style_arc_color(active_arc, lv_color_hex(0xFF4444), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(active_arc, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_arc_set_start_angle(active_arc, 0);
        lv_arc_set_end_angle(active_arc, 360);
        /* Auto-dismiss after 1.5s */
        stopped_timer = lv_timer_create(stopped_timer_cb, 1500, nullptr);
        lv_timer_set_repeat_count(stopped_timer, 1);
        break;

    case STATE_ERROR:
        show_widget(error_label);
        break;
    }
}

void echo_ui_set_status(const char *text)
{
    if (!text) return;

    if (current_state == STATE_INIT) {
        lv_label_set_text(boot_label, text);
    } else {
        lv_label_set_text(idle_status, text);
        if (current_state != STATE_IDLE) {
            hide_all_widgets();
            show_widget(idle_status);
        }
    }
}

/* Replace non-ASCII UTF-8 sequences (emoji, etc.) with a space.
 * Montserrat only covers ASCII — unsupported codepoints show as squares. */
static void sanitize_ascii(char *dst, const char *src, size_t dst_size)
{
    size_t d = 0;
    size_t s = 0;
    while (src[s] && d < dst_size - 1) {
        uint8_t c = (uint8_t)src[s];
        if (c < 0x80) {
            /* Plain ASCII — keep as-is */
            dst[d++] = src[s++];
        } else {
            /* Multi-byte UTF-8 sequence — skip all continuation bytes, emit one space */
            s++;
            while (src[s] && ((uint8_t)src[s] & 0xC0) == 0x80) s++;
            /* Only add space if previous char wasn't already a space */
            if (d > 0 && dst[d - 1] != ' ') {
                dst[d++] = ' ';
            }
        }
    }
    dst[d] = '\0';
}

void echo_ui_set_transcript(const char *text)
{
    if (!text || !text[0]) return;

    char clean[256];
    sanitize_ascii(clean, text, sizeof(clean));

    /* ~120 chars fits inside the arc (240px wide, Montserrat 20, ~6 lines) */
    char truncated[124];
    if (strlen(clean) > 120) {
        snprintf(truncated, sizeof(truncated), "%.117s...", clean);
    } else {
        strncpy(truncated, clean, sizeof(truncated) - 1);
        truncated[sizeof(truncated) - 1] = '\0';
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Thinking...\n\n\"%s\"", truncated);
    lv_label_set_text(active_label, buf);
}

void echo_ui_set_response(const char *text)
{
    if (!text) return;

    char clean[2048];
    sanitize_ascii(clean, text, sizeof(clean));

    /* ~120 chars fits inside the arc (240px wide, Montserrat 20, ~6 lines) */
    if (strlen(clean) > 120) {
        char truncated[124];
        snprintf(truncated, sizeof(truncated), "%.117s...", clean);
        lv_label_set_text(resp_label, truncated);
    } else {
        lv_label_set_text(resp_label, clean);
    }
}

void echo_ui_set_error(const char *text)
{
    if (!text) return;
    lv_label_set_text(error_label, text);
    show_widget(error_label);

    if (error_timer) {
        lv_timer_delete(error_timer);
    }
    error_timer = lv_timer_create(error_timer_cb, 3000, nullptr);
    lv_timer_set_repeat_count(error_timer, 1);
}

void echo_ui_tick(void)
{
}
