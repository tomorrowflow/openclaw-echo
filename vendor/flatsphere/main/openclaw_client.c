/*
 * OpenClaw Gateway WebSocket client implementation.
 * Ported from HeyClawy (MIT license), simplified for Echo MVP.
 *
 * Protocol: JSON text frames over WebSocket
 * - Server sends connect.challenge event with nonce
 * - Client sends connect req with device identity (ED25519 signed)
 * - Server responds hello-ok → client is connected
 * - Chat: {type:"req", method:"chat.send", params:{sessionKey, message}}
 * - Chat event: {type:"event", event:"chat", payload:{state:"delta"|"final", message:{content:[{text}]}}}
 */

#include "openclaw_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_tls.h"
#include "cJSON.h"
#include "ed25519.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <ctype.h>
#include "esp_heap_caps.h"
#include <sys/time.h>

static const char *TAG = "openclaw";

#define MAX_RESPONSE_LEN 4096
#define MAX_MSG_ID 99999

/* Base64url encoding (no padding) */
static const char b64url_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t base64url_encode(char *out, size_t out_max, const uint8_t *in, size_t in_len)
{
    size_t oi = 0, i = 0;
    while (i < in_len) {
        size_t rem = in_len - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i+1] << 8;
        if (rem > 2) v |= (uint32_t)in[i+2];

        if (oi < out_max) out[oi++] = b64url_table[(v >> 18) & 0x3F];
        if (oi < out_max) out[oi++] = b64url_table[(v >> 12) & 0x3F];
        if (rem > 1 && oi < out_max) out[oi++] = b64url_table[(v >> 6) & 0x3F];
        if (rem > 2 && oi < out_max) out[oi++] = b64url_table[v & 0x3F];
        i += 3;
    }
    if (oi < out_max) out[oi] = '\0';
    return oi;
}

static uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return 0;
}

static void hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        out[i] = (hex_nibble(hex[i*2]) << 4) | hex_nibble(hex[i*2+1]);
    }
}

static struct {
    esp_websocket_client_handle_t ws;
    openclaw_state_t state;
    openclaw_state_cb_t state_cb;
    openclaw_chat_cb_t chat_cb;
    char response_buf[MAX_RESPONSE_LEN];
    size_t response_len;
    char host[64];
    uint16_t port;
    char token[64];
    char nonce[48];
    int64_t chat_start_time;
    uint32_t msg_id;
    /* ED25519 device identity */
    uint8_t ed_seed[32];
    uint8_t ed_pubkey[32];
    uint8_t ed_privkey[64];
    char device_id[65];
    bool has_device_key;
    /* Fragmented message reassembly */
    char *frag_buf;
    size_t frag_len;
    size_t frag_total;
} s_oc;

static void set_state(openclaw_state_t st)
{
    s_oc.state = st;
    if (s_oc.state_cb) s_oc.state_cb(st);
}

static char *next_id(void)
{
    static char id_buf[16];
    s_oc.msg_id = (s_oc.msg_id + 1) % MAX_MSG_ID;
    snprintf(id_buf, sizeof(id_buf), "%" PRIu32, s_oc.msg_id);
    return id_buf;
}

static void send_connect(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "connect");

    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddNumberToObject(params, "minProtocol", 3);
    cJSON_AddNumberToObject(params, "maxProtocol", 3);

    cJSON *client = cJSON_AddObjectToObject(params, "client");
    cJSON_AddStringToObject(client, "id", "gateway-client");
    cJSON_AddStringToObject(client, "displayName", "OpenClaw Echo");
    cJSON_AddStringToObject(client, "version", "0.1.0");
    cJSON_AddStringToObject(client, "platform", "esp32s3");
    cJSON_AddStringToObject(client, "mode", "cli");

    cJSON_AddStringToObject(params, "role", "operator");

    cJSON *scopes = cJSON_AddArrayToObject(params, "scopes");
    cJSON_AddItemToArray(scopes, cJSON_CreateString("operator.read"));
    cJSON_AddItemToArray(scopes, cJSON_CreateString("operator.write"));
    cJSON_AddItemToArray(scopes, cJSON_CreateString("operator.admin"));

    if (strlen(s_oc.token) > 0) {
        cJSON *auth = cJSON_AddObjectToObject(params, "auth");
        cJSON_AddStringToObject(auth, "token", s_oc.token);
    }

    /* Add device identity block with ED25519 signature */
    if (s_oc.has_device_key) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t epoch_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

        char auth_payload[512];
        snprintf(auth_payload, sizeof(auth_payload),
                 "v2|%s|gateway-client|cli|operator|operator.read,operator.write,operator.admin|%" PRId64 "|%s|%s",
                 s_oc.device_id, epoch_ms, s_oc.token, s_oc.nonce);

        uint8_t signature[64];
        ed25519_sign(signature, (const uint8_t *)auth_payload, strlen(auth_payload),
                     s_oc.ed_pubkey, s_oc.ed_privkey);

        char pub_b64[64];
        base64url_encode(pub_b64, sizeof(pub_b64), s_oc.ed_pubkey, 32);
        char sig_b64[128];
        base64url_encode(sig_b64, sizeof(sig_b64), signature, 64);

        cJSON *device = cJSON_AddObjectToObject(params, "device");
        cJSON_AddStringToObject(device, "id", s_oc.device_id);
        cJSON_AddStringToObject(device, "publicKey", pub_b64);
        cJSON_AddStringToObject(device, "signature", sig_b64);
        cJSON_AddNumberToObject(device, "signedAt", (double)epoch_ms);
        cJSON_AddStringToObject(device, "nonce", s_oc.nonce);

        ESP_LOGI(TAG, "Device auth: id=%s", s_oc.device_id);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        int jlen = strlen(json_str);
        ESP_LOGI(TAG, "Connect req (%d bytes)", jlen);
        esp_websocket_client_send_text(s_oc.ws, json_str, jlen, pdMS_TO_TICKS(5000));
        free(json_str);
    }
    cJSON_Delete(root);

    set_state(OPENCLAW_STATE_AUTHENTICATING);
}

static void handle_message(const char *data, int len)
{
    ESP_LOGD(TAG, "WS msg (%d bytes): %.200s%s", len, data, len > 200 ? "..." : "");

    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON message");
        return;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (!type) goto cleanup;

    if (strcmp(type, "event") == 0) {
        const char *event = cJSON_GetStringValue(cJSON_GetObjectItem(root, "event"));
        if (!event) goto cleanup;

        if (strcmp(event, "connect.challenge") == 0) {
            ESP_LOGI(TAG, "Got connect.challenge");
            cJSON *payload = cJSON_GetObjectItem(root, "payload");
            if (payload) {
                const char *nonce = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "nonce"));
                if (nonce) {
                    strncpy(s_oc.nonce, nonce, sizeof(s_oc.nonce) - 1);
                }
            }
            send_connect();
        } else if (strcmp(event, "chat") == 0) {
            cJSON *payload = cJSON_GetObjectItem(root, "payload");
            if (!payload) goto cleanup;

            const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "state"));
            if (!state) goto cleanup;

            /* Only process if we initiated a chat */
            if (!s_oc.chat_cb) goto cleanup;

            if (strcmp(state, "delta") == 0) {
                set_state(OPENCLAW_STATE_CHAT_STREAMING);
                cJSON *message = cJSON_GetObjectItem(payload, "message");
                if (message) {
                    cJSON *content = cJSON_GetObjectItem(message, "content");
                    if (content && cJSON_IsArray(content)) {
                        cJSON *item;
                        cJSON_ArrayForEach(item, content) {
                            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "text"));
                            if (text) {
                                size_t tlen = strlen(text);
                                if (s_oc.response_len + tlen < MAX_RESPONSE_LEN - 1) {
                                    memcpy(s_oc.response_buf + s_oc.response_len, text, tlen);
                                    s_oc.response_len += tlen;
                                    s_oc.response_buf[s_oc.response_len] = '\0';
                                }
                                if (s_oc.chat_cb) s_oc.chat_cb(text, false);
                            }
                        }
                    }
                }
            } else if (strcmp(state, "final") == 0) {
                /* Final event: authoritative, replaces streaming deltas */
                cJSON *message = cJSON_GetObjectItem(payload, "message");
                if (message) {
                    cJSON *content = cJSON_GetObjectItem(message, "content");
                    if (content && cJSON_IsArray(content)) {
                        s_oc.response_buf[0] = '\0';
                        s_oc.response_len = 0;
                        cJSON *item;
                        cJSON_ArrayForEach(item, content) {
                            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "text"));
                            if (text) {
                                size_t tlen = strlen(text);
                                if (s_oc.response_len + tlen < MAX_RESPONSE_LEN - 1) {
                                    memcpy(s_oc.response_buf + s_oc.response_len, text, tlen);
                                    s_oc.response_len += tlen;
                                    s_oc.response_buf[s_oc.response_len] = '\0';
                                }
                            }
                        }
                        ESP_LOGI(TAG, "Final: content items=%d, response_len=%d",
                                 cJSON_GetArraySize(content), (int)s_oc.response_len);
                    } else {
                        ESP_LOGW(TAG, "Final: no content array in message");
                    }
                } else {
                    char *raw = cJSON_PrintUnformatted(payload);
                    ESP_LOGW(TAG, "Final: no message in payload: %.300s", raw ? raw : "null");
                    free(raw);
                }
                /* If final response is empty but we accumulated deltas, keep them */
                if (s_oc.response_len == 0 && s_oc.response_buf[0] == '\0') {
                    ESP_LOGW(TAG, "Final: response empty, check payload structure");
                }
                if (s_oc.chat_cb) s_oc.chat_cb(s_oc.response_buf, true);
                s_oc.chat_cb = NULL;
                set_state(OPENCLAW_STATE_CONNECTED);
            } else if (strcmp(state, "error") == 0) {
                const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "errorMessage"));
                ESP_LOGE(TAG, "Chat error: %s", err ? err : "unknown");
                if (s_oc.chat_cb) s_oc.chat_cb(err ? err : "Error", true);
                s_oc.chat_cb = NULL;
                set_state(OPENCLAW_STATE_CONNECTED);
            } else if (strcmp(state, "aborted") == 0) {
                ESP_LOGW(TAG, "Chat aborted");
                if (s_oc.chat_cb) s_oc.chat_cb("Aborted", true);
                s_oc.chat_cb = NULL;
                set_state(OPENCLAW_STATE_CONNECTED);
            }
        }
    } else if (strcmp(type, "res") == 0) {
        cJSON *error = cJSON_GetObjectItem(root, "error");
        if (error) {
            const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(error, "message"));
            ESP_LOGE(TAG, "Request error: %s", msg ? msg : "unknown");
            if (s_oc.state == OPENCLAW_STATE_AUTHENTICATING) {
                set_state(OPENCLAW_STATE_ERROR);
            }
        } else {
            /* Success response — if authenticating, we're now connected */
            if (s_oc.state == OPENCLAW_STATE_AUTHENTICATING) {
                cJSON *payload = cJSON_GetObjectItem(root, "payload");
                if (payload) {
                    cJSON *server = cJSON_GetObjectItem(payload, "server");
                    if (server) {
                        const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(server, "version"));
                        if (ver) ESP_LOGI(TAG, "Server version: %s", ver);
                    }
                }
                set_state(OPENCLAW_STATE_CONNECTED);
                /* Relax WS ping from 1s to 10s (must be < network_timeout_ms) */
                esp_websocket_client_set_ping_interval_sec(s_oc.ws, 10);
                ESP_LOGI(TAG, "Connected to OpenClaw gateway");
            }
        }
    }

cleanup:
    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    esp_websocket_event_data_t *ws_data = (esp_websocket_event_data_t *)data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_oc.msg_id = 0;
        free(s_oc.frag_buf);
        s_oc.frag_buf = NULL;
        s_oc.frag_len = 0;
        s_oc.frag_total = 0;
        esp_websocket_client_set_ping_interval_sec(s_oc.ws, 1);
        set_state(OPENCLAW_STATE_CONNECTING);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected (state was %d)", s_oc.state);
        set_state(OPENCLAW_STATE_DISCONNECTED);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ws_data->op_code == 0x08) {
            uint16_t close_code = 0;
            if (ws_data->data_len >= 2 && ws_data->data_ptr) {
                close_code = ((uint8_t)ws_data->data_ptr[0] << 8) | (uint8_t)ws_data->data_ptr[1];
            }
            ESP_LOGW(TAG, "WS CLOSE frame: code=%d", close_code);
            set_state(OPENCLAW_STATE_DISCONNECTED);
            break;
        }
        if (ws_data->op_code == 0x0a) break; /* Pong */

        if (ws_data->op_code == 0x01 || ws_data->op_code == 0x00) {
            /* Handle fragmented messages */
            if (ws_data->payload_len > ws_data->data_len) {
                if (ws_data->payload_offset == 0) {
                    free(s_oc.frag_buf);
                    s_oc.frag_buf = heap_caps_malloc(ws_data->payload_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    s_oc.frag_total = ws_data->payload_len;
                    s_oc.frag_len = 0;
                }
                if (s_oc.frag_buf && s_oc.frag_len + ws_data->data_len <= s_oc.frag_total) {
                    memcpy(s_oc.frag_buf + s_oc.frag_len, ws_data->data_ptr, ws_data->data_len);
                    s_oc.frag_len += ws_data->data_len;
                }
                if (s_oc.frag_buf && s_oc.frag_len >= s_oc.frag_total) {
                    s_oc.frag_buf[s_oc.frag_len] = '\0';
                    handle_message(s_oc.frag_buf, s_oc.frag_len);
                    free(s_oc.frag_buf);
                    s_oc.frag_buf = NULL;
                    s_oc.frag_len = 0;
                }
            } else {
                handle_message((const char *)ws_data->data_ptr, ws_data->data_len);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        set_state(OPENCLAW_STATE_ERROR);
        break;

    default:
        break;
    }
}

esp_err_t openclaw_init(const openclaw_config_t *config, openclaw_state_cb_t state_cb)
{
    memset(&s_oc, 0, sizeof(s_oc));
    s_oc.state_cb = state_cb;
    strncpy(s_oc.host, config->host, sizeof(s_oc.host) - 1);
    s_oc.port = config->port;
    if (config->token) {
        strncpy(s_oc.token, config->token, sizeof(s_oc.token) - 1);
    }

    /* Initialize ED25519 device identity from hex seed */
    if (config->device_key_hex && strlen(config->device_key_hex) == 64) {
        hex_to_bytes(config->device_key_hex, s_oc.ed_seed, 32);
        ed25519_create_keypair(s_oc.ed_pubkey, s_oc.ed_privkey, s_oc.ed_seed);

        /* Device ID = SHA-256(raw_public_key).hex() */
        uint8_t hash[32];
        mbedtls_sha256(s_oc.ed_pubkey, 32, hash, 0);
        for (int i = 0; i < 32; i++) {
            sprintf(s_oc.device_id + i*2, "%02x", hash[i]);
        }
        s_oc.has_device_key = true;
        ESP_LOGI(TAG, "Device identity: %s", s_oc.device_id);
    }

    ESP_LOGI(TAG, "OpenClaw client initialized (host=%s, port=%d)", s_oc.host, s_oc.port);
    return ESP_OK;
}

esp_err_t openclaw_connect(void)
{
    char uri[128];
    bool use_tls = (s_oc.port == 443);
    const char *scheme = use_tls ? "wss" : "ws";
    snprintf(uri, sizeof(uri), "%s://%s:%d", scheme, s_oc.host, s_oc.port);

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .buffer_size = 16384,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 60000,
        .task_stack = 8192,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 0,
        .disable_pingpong_discon = true,  /* Tailscale proxy doesn't forward WS pongs */
    };

    /* For wss:// connections, build a custom SSL transport with TLS 1.3 forced.
     * The default esp_websocket_client doesn't expose tls_version, so we create
     * the transport chain ourselves and pass it via ext_transport. */
    if (use_tls) {
        esp_transport_handle_t ssl = esp_transport_ssl_init();
        if (!ssl) {
            ESP_LOGE(TAG, "Failed to create SSL transport");
            return ESP_FAIL;
        }

        /* Use cert bundle for CA verification (Let's Encrypt for Tailscale).
         * Do NOT skip common name check — that also disables SNI, which
         * Go's crypto/tls requires to select the right certificate. */
        esp_transport_ssl_crt_bundle_attach(ssl, esp_crt_bundle_attach);

        /* ALPN is required by Tailscale's Go-based HTTPS proxy */
        static const char *alpn_protos[] = {"http/1.1", NULL};
        esp_transport_ssl_set_alpn_protocol(ssl, alpn_protos);

        esp_transport_handle_t wss = esp_transport_ws_init(ssl);
        if (!wss) {
            ESP_LOGE(TAG, "Failed to create WS transport");
            return ESP_FAIL;
        }
        esp_transport_set_default_port(wss, 443);

        ws_cfg.ext_transport = wss;
        ESP_LOGI(TAG, "Using custom TLS transport with ALPN");
    } else {
        ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_oc.ws = esp_websocket_client_init(&ws_cfg);
    if (!s_oc.ws) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_oc.ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    set_state(OPENCLAW_STATE_CONNECTING);
    ESP_LOGI(TAG, "Connecting to %s", uri);
    return esp_websocket_client_start(s_oc.ws);
}

esp_err_t openclaw_disconnect(void)
{
    if (s_oc.ws) {
        esp_websocket_client_stop(s_oc.ws);
        esp_websocket_client_destroy(s_oc.ws);
        s_oc.ws = NULL;
    }
    set_state(OPENCLAW_STATE_DISCONNECTED);
    return ESP_OK;
}

openclaw_state_t openclaw_get_state(void)
{
    return s_oc.state;
}

esp_err_t openclaw_chat_send(const char *message, openclaw_chat_cb_t response_cb)
{
    if (s_oc.state != OPENCLAW_STATE_CONNECTED) {
        ESP_LOGE(TAG, "Not connected to OpenClaw (state=%d)", s_oc.state);
        return ESP_ERR_INVALID_STATE;
    }

    s_oc.chat_cb = response_cb;
    s_oc.response_buf[0] = '\0';
    s_oc.response_len = 0;
    s_oc.chat_start_time = esp_timer_get_time();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", next_id());
    cJSON_AddStringToObject(root, "method", "chat.send");

    cJSON *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "sessionKey", "default");
    cJSON_AddStringToObject(params, "message", message);

    /* Idempotency key */
    char idem_key[32];
    snprintf(idem_key, sizeof(idem_key), "echo-%" PRIu64 "-%" PRIu32,
             (uint64_t)(esp_timer_get_time() / 1000), s_oc.msg_id);
    cJSON_AddStringToObject(params, "idempotencyKey", idem_key);

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    set_state(OPENCLAW_STATE_CHAT_SENDING);
    int sent = esp_websocket_client_send_text(s_oc.ws, json_str, strlen(json_str), pdMS_TO_TICKS(10000));
    free(json_str);
    cJSON_Delete(root);

    if (sent < 0) {
        ESP_LOGE(TAG, "Chat send failed (ws lock timeout or disconnected)");
        s_oc.chat_cb = NULL;
        set_state(OPENCLAW_STATE_CONNECTED);
        return ESP_FAIL;
    }

    set_state(OPENCLAW_STATE_CHAT_THINKING);
    ESP_LOGI(TAG, "Chat sent: %.80s%s", message, strlen(message) > 80 ? "..." : "");
    return ESP_OK;
}

const char *openclaw_get_last_response(void)
{
    return s_oc.response_buf;
}

uint32_t openclaw_get_thinking_time_ms(void)
{
    if (s_oc.chat_start_time == 0) return 0;
    return (uint32_t)((esp_timer_get_time() - s_oc.chat_start_time) / 1000);
}
