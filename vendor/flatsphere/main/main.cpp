/**
 * @file main.cpp
 * @brief OpenClaw Echo — Voice assistant pipeline
 *
 * Board: Waveshare ESP32-S3-Touch-LCD-1.85C V2
 * ES8311 (DAC/speaker) + ES7210 (ADC/mic) on shared I2S_NUM_0
 *
 * State machine: IDLE → RECORDING → TRANSCRIBING → CHATTING → SPEAKING → IDLE
 * Wake word ("Hey Jarvis") or BOOT button triggers recording.
 * Wake word detection + audio preprocessing runs on Core 0.
 * App logic + LVGL runs on Core 1.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include "hal/hal_185c.h"
#include "settings/settings.hpp"
#include <lvgl.h>
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/gpio.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"

extern "C" {
#include "stt_client.h"
#include "tts_client.h"
#include "openclaw_client.h"
}

#include "micro_wake.h"
#include "microlink.h"
#include "lwip/dns.h"

#include "secrets.h"

static const char* TAG = "echo";

/* ── Pin definitions ── */
#define AUDIO_MCLK_IO    2
#define AUDIO_BCLK_IO    48
#define AUDIO_WS_IO      38
#define AUDIO_DOUT_IO    47
#define AUDIO_DIN_IO     39
#define PA_EN_IO         15
#define BOOT_BTN_IO      0

#define ES7210_ADDR      ES7210_CODEC_DEFAULT_ADDR
#define ES8311_ADDR      ES8311_CODEC_DEFAULT_ADDR
#define SAMPLE_RATE      24000
#define STT_SAMPLE_RATE  16000

/* ── Audio recording config ── */
#define MAX_RECORD_SECONDS  10
#define RECORD_BUF_SAMPLES  (STT_SAMPLE_RATE * MAX_RECORD_SECONDS)  /* 16kHz mono */
#define SILENCE_TIMEOUT_MS  1500    /* Stop recording after 1.5s silence */

/* ── App state ── */
typedef enum {
    STATE_INIT = 0,
    STATE_IDLE,
    STATE_RECORDING,
    STATE_TRANSCRIBING,
    STATE_CHATTING,
    STATE_SPEAKING,
    STATE_ERROR,
} app_state_t;

using namespace HAL;
using namespace SETTINGS;

Settings settings;
Hal185C hal(&settings);

static i2s_chan_handle_t tx_chan = nullptr;
static i2s_chan_handle_t rx_chan = nullptr;
static esp_codec_dev_handle_t input_dev = nullptr;
static esp_codec_dev_handle_t output_dev = nullptr;

static app_state_t app_state = STATE_INIT;
static int16_t *record_buf = nullptr;       /* 16kHz mono in PSRAM */
static size_t record_samples = 0;
static microlink_t *ml_handle = nullptr;    /* Tailscale VPN handle */

/* UI labels */
static lv_obj_t *lbl_state = nullptr;
static lv_obj_t *lbl_info = nullptr;
static lv_obj_t *lbl_response = nullptr;

/* Event bits for cross-task signaling */
static EventGroupHandle_t app_events = nullptr;
#define EVT_BOOT_PRESS    BIT0
#define EVT_OC_CONNECTED  BIT1
#define EVT_CHAT_DONE     BIT2
#define EVT_WAKE_WORD     BIT3
#define EVT_VPN_READY     BIT4

/* OpenClaw response storage */
static char oc_response[2048] = "";
static bool oc_response_ready = false;

/* ── Forward declarations ── */
static esp_err_t setup_audio();
static void setup_wifi();
static void setup_ui();
static void update_ui_state(const char *state_text, uint32_t color);

/* ── Audio setup (from loopback test) ── */
static esp_err_t setup_audio()
{
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << PA_EN_IO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level((gpio_num_t)PA_EN_IO, 0);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan), TAG, "new duplex channel");

    /* TX: STD mode for ES8311 speaker */
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = (gpio_num_t)AUDIO_MCLK_IO,
            .bclk = (gpio_num_t)AUDIO_BCLK_IO,
            .ws   = (gpio_num_t)AUDIO_WS_IO,
            .dout = (gpio_num_t)AUDIO_DOUT_IO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_chan, &std_cfg), TAG, "TX std init");

    /* RX: TDM mode for ES7210 mic (4 slots) */
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = (gpio_num_t)AUDIO_MCLK_IO,
            .bclk = (gpio_num_t)AUDIO_BCLK_IO,
            .ws   = (gpio_num_t)AUDIO_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)AUDIO_DIN_IO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(rx_chan, &tdm_cfg), TAG, "RX TDM init");

    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_chan), TAG, "TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx_chan), TAG, "RX enable");

    /* Shared data interface for codec dev */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_chan,
        .tx_handle = tx_chan,
    };
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) return ESP_FAIL;

    /* ES8311 output codec */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0,
        .addr = ES8311_ADDR,
        .bus_handle = hal.i2c()->get_bus_handle(),
    };
    const audio_codec_ctrl_if_t* out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!out_ctrl) return ESP_FAIL;

    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl;
    es8311_cfg.gpio_if = gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = (int16_t)PA_EN_IO;
    es8311_cfg.use_mclk = true;
    const audio_codec_if_t* es8311_if = es8311_codec_new(&es8311_cfg);
    if (!es8311_if) return ESP_FAIL;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_if,
        .data_if = data_if,
    };
    output_dev = esp_codec_dev_new(&dev_cfg);
    if (!output_dev) return ESP_FAIL;

    esp_codec_dev_sample_info_t out_sample = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE,
    };
    if (esp_codec_dev_open(output_dev, &out_sample) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    esp_codec_dev_set_out_vol(output_dev, 70);

    /* ES7210 input codec */
    i2c_cfg.addr = ES7210_ADDR;
    const audio_codec_ctrl_if_t* in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!in_ctrl) return ESP_FAIL;

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    const audio_codec_if_t* es7210_if = es7210_codec_new(&es7210_cfg);
    if (!es7210_if) return ESP_FAIL;

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = es7210_if;
    input_dev = esp_codec_dev_new(&dev_cfg);
    if (!input_dev) return ESP_FAIL;

    esp_codec_dev_sample_info_t in_sample = {
        .bits_per_sample = 16,
        .channel = 4,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE,
    };
    if (esp_codec_dev_open(input_dev, &in_sample) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    esp_codec_dev_set_in_gain(input_dev, 30.0f);

    ESP_LOGI(TAG, "Audio initialized (ES8311 + ES7210)");
    return ESP_OK;
}

/* ── WiFi setup ── */
static bool wifi_has_ip()
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return false;
    return ip_info.ip.addr != 0;
}

static void setup_wifi()
{
    /* Override settings with secrets.h values for WiFi connect */
    settings.setString("wifi", "ssid", SECRETS_WIFI_SSID);
    settings.setString("wifi", "pass", SECRETS_WIFI_PASSWORD);
    settings.setBool("wifi", "enabled", true);

    hal.wifi()->init();
    hal.wifi()->connect();

    /* Wait for WiFi association */
    int retries = 0;
    while (!hal.wifi()->is_connected() && retries < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retries++;
    }

    if (!hal.wifi()->is_connected()) {
        ESP_LOGW(TAG, "WiFi connection timeout — will retry in background");
        return;
    }

    /* Wait for IP address (DHCP) — is_connected fires before IP is assigned */
    ESP_LOGI(TAG, "WiFi associated, waiting for IP...");
    retries = 0;
    while (!wifi_has_ip() && retries < 150) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retries++;
    }

    if (wifi_has_ip()) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(netif, &ip_info);
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&ip_info.ip));
    } else {
        ESP_LOGW(TAG, "WiFi connected but no IP yet — DHCP may still be pending");
    }
}

/* ── TTS playback callback ── */
static void tts_play_callback(const int16_t *samples, size_t bytes)
{
    esp_codec_dev_write(output_dev, (void *)samples, bytes);
}

/* ── OpenClaw state callback ── */
static void oc_state_callback(openclaw_state_t state)
{
    ESP_LOGI(TAG, "OpenClaw state: %d", state);
    if (state == OPENCLAW_STATE_CONNECTED) {
        xEventGroupSetBits(app_events, EVT_OC_CONNECTED);
    }
}

/* ── OpenClaw chat response callback ── */
static void oc_chat_callback(const char *text, bool is_final)
{
    if (is_final) {
        strncpy(oc_response, text, sizeof(oc_response) - 1);
        oc_response[sizeof(oc_response) - 1] = '\0';
        oc_response_ready = true;
        xEventGroupSetBits(app_events, EVT_CHAT_DONE);
        ESP_LOGI(TAG, "OpenClaw response: %.100s%s", text, strlen(text) > 100 ? "..." : "");
    }
}

/* ── UI setup ── */
static void setup_ui()
{
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);

    lbl_state = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_state, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(lbl_state, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(lbl_state, "Initializing...");

    lbl_info = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_info, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(lbl_info, 300);
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl_info, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(lbl_info, "");

    lbl_response = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_response, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_response, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_width(lbl_response, 300);
    lv_label_set_long_mode(lbl_response, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_response, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl_response, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_label_set_text(lbl_response, "Say \"Hey Jarvis\" or press BOOT");
}

static void update_ui_state(const char *state_text, uint32_t color)
{
    lv_obj_set_style_text_color(lbl_state, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(lbl_state, state_text);
}

/* ── Wake word callback (called from Core 0) ── */
static void wake_word_callback(void)
{
    xEventGroupSetBits(app_events, EVT_WAKE_WORD);
}

/* ── App task (Core 1) ── */
static void app_task(void *arg);

/* ── Main application ── */
extern "C" void app_main(void)
{
    if (!settings.init()) return;
    hal.init();
    hal.display()->set_backlight(settings.getNumber("system", "brightness"));

    app_events = xEventGroupCreate();

    /* Setup UI */
    setup_ui();
    hal.display()->lvgl_timer_handler();

    /* Setup audio */
    esp_err_t ret = setup_audio();
    if (ret != ESP_OK) {
        update_ui_state("AUDIO FAILED", 0xFF0000);
        lv_label_set_text(lbl_info, esp_err_to_name(ret));
        while (1) {
            hal.display()->lvgl_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    /* Allocate recording buffer in PSRAM */
    record_buf = (int16_t *)heap_caps_malloc(RECORD_BUF_SAMPLES * sizeof(int16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!record_buf) {
        update_ui_state("PSRAM OOM", 0xFF0000);
        while (1) {
            hal.display()->lvgl_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    /* Setup WiFi */
    update_ui_state("Connecting WiFi...", 0xFFAA00);
    hal.display()->lvgl_timer_handler();
    setup_wifi();

    if (!hal.wifi()->is_connected()) {
        update_ui_state("WiFi Failed", 0xFF0000);
        lv_label_set_text(lbl_info, "Check SSID/password in secrets.h");
        while (1) {
            hal.display()->lvgl_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    /* Start Tailscale VPN first (non-blocking, connects in background) */
    if (strlen(SECRETS_TAILSCALE_AUTH_KEY) > 0) {
        microlink_config_t ml_cfg;
        microlink_get_default_config(&ml_cfg);
        ml_cfg.auth_key = SECRETS_TAILSCALE_AUTH_KEY;
        ml_cfg.device_name = SECRETS_TAILSCALE_DEVICE;
        ml_cfg.enable_derp = true;
        ml_cfg.enable_disco = false;  /* DERP relay only — disco probes all peers */
        ml_cfg.enable_stun = false;   /* Not needed without direct P2P */
        /* Only establish WG session with minisforum790 (OpenClaw server)
         * to reduce DERP traffic — 10 peers all doing handshakes/keepalives
         * overwhelms the DERP queue and starves the WebSocket connection. */
        ml_cfg.target_vpn_ip = (100u << 24) | (122u << 16) | (29u << 8) | 8u;
        ml_cfg.on_connected = []() {
            ESP_LOGI(TAG, "Tailscale VPN connected!");
            char ip_str[16];
            if (ml_handle) {
                uint32_t vpn_ip = microlink_get_vpn_ip(ml_handle);
                ESP_LOGI(TAG, "Tailscale IP: %s", microlink_vpn_ip_to_str(vpn_ip, ip_str));

                /* Populate lwIP local DNS hostlist from MicroLink peer map.
                 * MagicDNS (100.100.100.100) doesn't work without a local tailscaled,
                 * so we resolve .ts.net hostnames ourselves from the peer table. */
                const microlink_peer_t *peers = nullptr;
                uint8_t peer_count = 0;
                if (microlink_get_peers(ml_handle, &peers, &peer_count) == ESP_OK && peers) {
                    for (uint8_t i = 0; i < peer_count; i++) {
                        if (peers[i].hostname[0] && peers[i].vpn_ip) {
                            ip_addr_t addr;
                            IP_ADDR4(&addr,
                                     (peers[i].vpn_ip >> 24) & 0xFF,
                                     (peers[i].vpn_ip >> 16) & 0xFF,
                                     (peers[i].vpn_ip >> 8) & 0xFF,
                                     peers[i].vpn_ip & 0xFF);
                            if (dns_local_addhost(peers[i].hostname, &addr) == ERR_OK) {
                                ESP_LOGI(TAG, "DNS: %s -> %d.%d.%d.%d",
                                         peers[i].hostname,
                                         (peers[i].vpn_ip >> 24) & 0xFF,
                                         (peers[i].vpn_ip >> 16) & 0xFF,
                                         (peers[i].vpn_ip >> 8) & 0xFF,
                                         peers[i].vpn_ip & 0xFF);
                            }
                        }
                    }
                }

                /* Signal that VPN tunnel is ready for OpenClaw connection */
                if (app_events) {
                    xEventGroupSetBits(app_events, EVT_VPN_READY);
                }
            }
        };
        ml_cfg.on_state_change = [](microlink_state_t old_s, microlink_state_t new_s) {
            ESP_LOGI(TAG, "Tailscale: %s -> %s",
                     microlink_state_to_str(old_s), microlink_state_to_str(new_s));
        };

        update_ui_state("Tailscale...", 0xFFAA00);
        hal.display()->lvgl_timer_handler();

        /* Reduce MicroLink verbosity — keep WARN+ for routine ops,
         * state transitions and errors still visible. */
        esp_log_level_set("ml_derp",  ESP_LOG_WARN);
        esp_log_level_set("ml_disco", ESP_LOG_WARN);
        esp_log_level_set("ml_coord", ESP_LOG_WARN);
        esp_log_level_set("ml_wg",    ESP_LOG_WARN);
        esp_log_level_set("ml_stun",  ESP_LOG_WARN);
        esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);

        ml_handle = microlink_init(&ml_cfg);
        if (ml_handle) {
            microlink_connect(ml_handle);
            ESP_LOGI(TAG, "Tailscale connecting in background...");
        } else {
            ESP_LOGW(TAG, "Tailscale init failed");
        }
    }

    /* Initialize service clients */
    stt_init(SECRETS_STT_HOST, SECRETS_STT_PORT);

    tts_config_t tts_cfg = {
        .host = SECRETS_TTS_HOST,
        .port = SECRETS_TTS_PORT,
        .api_key = nullptr,
        .voice = SECRETS_TTS_VOICE,
        .model = SECRETS_TTS_MODEL,
    };
    tts_init(&tts_cfg);
    tts_set_play_cb(tts_play_callback);

    openclaw_config_t oc_cfg = {
        .host = SECRETS_OPENCLAW_HOST,
        .port = SECRETS_OPENCLAW_PORT,
        .token = SECRETS_OPENCLAW_TOKEN,
        .device_key_hex = (strlen(SECRETS_DEVICE_KEY_HEX) == 64) ? SECRETS_DEVICE_KEY_HEX : nullptr,
    };
    openclaw_init(&oc_cfg, oc_state_callback);

    /* Wait for VPN tunnel before connecting OpenClaw (host is on Tailscale network) */
    if (ml_handle) {
        update_ui_state("Waiting VPN...", 0xFFAA00);
        hal.display()->lvgl_timer_handler();

        TickType_t vpn_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
        bool vpn_ready = false;
        while (xTaskGetTickCount() < vpn_deadline) {
            microlink_update(ml_handle);
            EventBits_t bits = xEventGroupWaitBits(app_events, EVT_VPN_READY,
                                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
            if (bits & EVT_VPN_READY) { vpn_ready = true; break; }
            hal.display()->lvgl_timer_handler();
        }
        if (!vpn_ready) {
            ESP_LOGW(TAG, "VPN not ready after 60s, connecting OpenClaw anyway");
        } else {
            ESP_LOGI(TAG, "VPN ready, connecting OpenClaw...");
            /* Brief delay for WG handshake with target peer to complete */
            for (int i = 0; i < 30; i++) {
                microlink_update(ml_handle);
                hal.display()->lvgl_timer_handler();
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }

    /* Connect to OpenClaw */
    update_ui_state("Connecting OC...", 0xFFAA00);
    hal.display()->lvgl_timer_handler();
    openclaw_connect();

    /* Wait for OpenClaw connection, while pumping MicroLink state machine */
    {
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
        bool oc_connected = false;
        while (xTaskGetTickCount() < deadline) {
            if (ml_handle) microlink_update(ml_handle);
            EventBits_t bits = xEventGroupWaitBits(app_events, EVT_OC_CONNECTED,
                                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
            if (bits & EVT_OC_CONNECTED) { oc_connected = true; break; }
            hal.display()->lvgl_timer_handler();
        }
        if (!oc_connected) {
            update_ui_state("OC Timeout", 0xFF4400);
            lv_label_set_text(lbl_info, "OpenClaw not reachable");
            /* Continue anyway — might connect later via auto-reconnect */
        }
    }

    /* Start wake word detection on Core 0 */
    update_ui_state("Loading wake...", 0xFFAA00);
    hal.display()->lvgl_timer_handler();

    micro_wake_set_callback(wake_word_callback);
    ret = micro_wake_start(rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wake word init failed: %s — BOOT button only", esp_err_to_name(ret));
    }

    /* Ready! */
    app_state = STATE_IDLE;
    update_ui_state("Ready", 0x00FF00);
    lv_label_set_text(lbl_response, "Say \"Hey Jarvis\" or press BOOT");
    /* Don't flush here — micro_wake just started on Core 0 and SPI
     * queue conflicts. The app_task flushes on its first iteration. */

    /* Launch app task on Core 1 — use PSRAM for stack (internal RAM exhausted) */
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
        app_task, "app_task", 16384, nullptr, 5, nullptr, 1, MALLOC_CAP_SPIRAM);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create app_task! ret=%d", task_ret);
    } else {
        ESP_LOGI(TAG, "app_task created on Core 1 (PSRAM stack)");
    }

    /* app_main returns — FreeRTOS scheduler runs the tasks */
}

/* ── App task (Core 1) — main state machine loop ── */
static void app_task(void *arg)
{
    (void)arg;

    /* BOOT button GPIO */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BTN_IO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    bool btn_prev = true;

    /* Let micro_wake task settle before first LVGL flush — avoids SPI queue race */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* ── Main loop ── */
    while (1) {
        hal.display()->lvgl_timer_handler();

        /* Check BOOT button (active low) */
        bool btn_now = gpio_get_level((gpio_num_t)BOOT_BTN_IO);
        bool btn_pressed = (btn_prev && !btn_now);
        btn_prev = btn_now;

        if (btn_pressed) {
            vTaskDelay(pdMS_TO_TICKS(50)); /* debounce */
        }

        /* Check for wake word event */
        EventBits_t wake_bits = xEventGroupGetBits(app_events);
        bool wake_triggered = (wake_bits & EVT_WAKE_WORD) != 0;
        if (wake_triggered) {
            xEventGroupClearBits(app_events, EVT_WAKE_WORD);
            ESP_LOGI(TAG, "Wake event received (app_state=%d)", app_state);
        }

        switch (app_state) {

        case STATE_IDLE:
            if (btn_pressed || wake_triggered) {
                if (wake_triggered) {
                    ESP_LOGI(TAG, "Wake word triggered! Starting recording...");
                }
                app_state = STATE_RECORDING;
                update_ui_state("Listening...", 0xFF0000);
                lv_label_set_text(lbl_info, "");
                lv_label_set_text(lbl_response, "");
                hal.display()->lvgl_timer_handler();

                /* Tell wake task to start copying audio to record buffer */
                micro_wake_start_recording(record_buf, RECORD_BUF_SAMPLES);

                /* Wait for silence (VAD-based) or button cancel */
                uint32_t silence_start = 0;
                bool got_voice = false;
                TickType_t rec_start_tick = xTaskGetTickCount();

                while (app_state == STATE_RECORDING) {
                    /* Check for cancel (button press during recording) */
                    bool b = gpio_get_level((gpio_num_t)BOOT_BTN_IO);
                    if (!b && btn_prev) {
                        ESP_LOGI(TAG, "Recording stopped by button");
                        break;
                    }
                    btn_prev = b;

                    /* VAD-based silence detection */
                    bool vad = micro_wake_vad_active();
                    if (vad) {
                        got_voice = true;
                        silence_start = 0;
                    } else if (got_voice) {
                        if (silence_start == 0) {
                            silence_start = xTaskGetTickCount();
                        } else if ((xTaskGetTickCount() - silence_start) * portTICK_PERIOD_MS > SILENCE_TIMEOUT_MS) {
                            ESP_LOGI(TAG, "VAD silence detected, stopping recording");
                            break;
                        }
                    }

                    /* Max duration check */
                    if ((xTaskGetTickCount() - rec_start_tick) * portTICK_PERIOD_MS > MAX_RECORD_SECONDS * 1000) {
                        ESP_LOGI(TAG, "Max recording duration reached");
                        break;
                    }

                    /* Keep VPN alive while recording */
                    if (ml_handle) microlink_update(ml_handle);

                    /* Update UI periodically */
                    hal.display()->lvgl_timer_handler();
                    vTaskDelay(pdMS_TO_TICKS(50));
                }

                /* Stop recording and get sample count */
                record_samples = micro_wake_stop_recording();

                ESP_LOGI(TAG, "Recorded %u samples (%.1fs at %dHz)",
                         (unsigned)record_samples,
                         (float)record_samples / STT_SAMPLE_RATE,
                         STT_SAMPLE_RATE);

                if (record_samples < STT_SAMPLE_RATE / 4) {
                    ESP_LOGW(TAG, "Recording too short, ignoring");
                    app_state = STATE_IDLE;
                    update_ui_state("Ready", 0x00FF00);
                    lv_label_set_text(lbl_response, "Say \"Hey Jarvis\" or press BOOT");
                } else {
                    app_state = STATE_TRANSCRIBING;
                }
            }
            break;

        case STATE_TRANSCRIBING: {
            update_ui_state("Transcribing...", 0xFFAA00);
            hal.display()->lvgl_timer_handler();

            char transcript[512] = "";
            esp_err_t err = stt_transcribe(record_buf, record_samples,
                                           STT_SAMPLE_RATE, transcript, sizeof(transcript));

            if (err == ESP_OK && transcript[0]) {
                ESP_LOGI(TAG, "Transcript: %s", transcript);
                lv_label_set_text(lbl_info, transcript);

                if (openclaw_get_state() != OPENCLAW_STATE_CONNECTED) {
                    ESP_LOGW(TAG, "OpenClaw not connected, skipping chat");
                    update_ui_state("OC Disconnected", 0xFF4400);
                    lv_label_set_text(lbl_response, "OpenClaw not available");
                    app_state = STATE_IDLE;
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    update_ui_state("Ready", 0x00FF00);
                    lv_label_set_text(lbl_response, "Say \"Hey Jarvis\" or press BOOT");
                } else {
                    app_state = STATE_CHATTING;
                }
            } else {
                ESP_LOGW(TAG, "STT failed or empty: %s", esp_err_to_name(err));
                update_ui_state("STT Failed", 0xFF4400);
                lv_label_set_text(lbl_info, err == ESP_ERR_NOT_FOUND ? "No speech detected" : esp_err_to_name(err));
                app_state = STATE_IDLE;
                vTaskDelay(pdMS_TO_TICKS(2000));
                update_ui_state("Ready", 0x00FF00);
                lv_label_set_text(lbl_response, "Say \"Hey Jarvis\" or press BOOT");
            }
            break;
        }

        case STATE_CHATTING: {
            update_ui_state("Thinking...", 0x00AAFF);
            hal.display()->lvgl_timer_handler();

            const char *transcript = lv_label_get_text(lbl_info);
            oc_response_ready = false;
            bool chat_sent = false;
            int send_attempts = 0;
            const int max_send_attempts = 3;

            /* Send (or re-send after reconnect) */
            while (send_attempts < max_send_attempts && !oc_response_ready) {
                /* Wait for OpenClaw to be connected before sending */
                TickType_t connect_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
                while (openclaw_get_state() != OPENCLAW_STATE_CONNECTED &&
                       xTaskGetTickCount() < connect_deadline) {
                    if (ml_handle) microlink_update(ml_handle);
                    hal.display()->lvgl_timer_handler();
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                if (openclaw_get_state() != OPENCLAW_STATE_CONNECTED) {
                    ESP_LOGW(TAG, "OpenClaw not connected after waiting");
                    break;
                }

                esp_err_t err = openclaw_chat_send(transcript, oc_chat_callback);
                send_attempts++;
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Chat send failed: %s (attempt %d)", esp_err_to_name(err), send_attempts);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                chat_sent = true;

                /* Poll for response, watching for disconnection */
                EventBits_t bits = 0;
                TickType_t chat_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
                while (xTaskGetTickCount() < chat_deadline) {
                    bits = xEventGroupWaitBits(app_events, EVT_CHAT_DONE,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
                    if (bits & EVT_CHAT_DONE) break;

                    /* If connection dropped mid-chat, retry on reconnect */
                    if (openclaw_get_state() == OPENCLAW_STATE_DISCONNECTED ||
                        openclaw_get_state() == OPENCLAW_STATE_ERROR) {
                        ESP_LOGW(TAG, "OpenClaw disconnected during chat, will re-send (attempt %d)", send_attempts);
                        chat_sent = false;
                        break;
                    }

                    if (ml_handle) microlink_update(ml_handle);
                    hal.display()->lvgl_timer_handler();
                }

                if (oc_response_ready) break;
                if (!chat_sent) continue;  /* Disconnected — retry loop */

                /* Timed out without disconnect — give up */
                break;
            }

            if (!oc_response_ready) {
                ESP_LOGW(TAG, "Chat response timeout after %d attempts", send_attempts);
                update_ui_state("Timeout", 0xFF4400);
                app_state = STATE_IDLE;
                vTaskDelay(pdMS_TO_TICKS(2000));
                update_ui_state("Ready", 0x00FF00);
                lv_label_set_text(lbl_response, "Say \"Hey Jarvis\" or press BOOT");
                break;
            }

            lv_label_set_text(lbl_response, oc_response);
            ESP_LOGI(TAG, "Response: %.200s", oc_response);

            if (oc_response[0] == '\0') {
                ESP_LOGW(TAG, "Empty response from OpenClaw, skipping TTS");
                app_state = STATE_IDLE;
                update_ui_state("Ready", 0x00FF00);
                lv_label_set_text(lbl_response, "Say \"Hey Jarvis\" or press BOOT");
                break;
            }

            app_state = STATE_SPEAKING;
            break;
        }

        case STATE_SPEAKING: {
            update_ui_state("Speaking...", 0x00FF00);
            hal.display()->lvgl_timer_handler();

            /* Disable wake detection during playback to prevent false triggers */
            micro_wake_enable(false);

            gpio_set_level((gpio_num_t)PA_EN_IO, 1);

            esp_err_t err = tts_speak(oc_response, SAMPLE_RATE);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "TTS failed: %s", esp_err_to_name(err));
            }

            gpio_set_level((gpio_num_t)PA_EN_IO, 0);

            /* Re-enable wake detection */
            micro_wake_enable(true);

            app_state = STATE_IDLE;
            update_ui_state("Ready", 0x00FF00);
            lv_label_set_text(lbl_response, "Say \"Hey Jarvis\" or press BOOT");
            break;
        }

        default:
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }

        /* Update Tailscale state machine */
        if (ml_handle) {
            microlink_update(ml_handle);
        }

        if (app_state == STATE_IDLE) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
