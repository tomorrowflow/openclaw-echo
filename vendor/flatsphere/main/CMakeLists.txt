file(GLOB_RECURSE HAL_SRCS
    hal/*.cpp
)

file(GLOB_RECURSE SETTINGS_SRCS
    settings/*.cpp
)

file(GLOB_RECURSE UI_SRCS
    ui/*.c
)

idf_component_register(SRCS "main.cpp" "micro_wake.cpp" "stt_client.c" "tts_client.c" "openclaw_client.c"
    ${HAL_SRCS} ${SETTINGS_SRCS} ${UI_SRCS}
    INCLUDE_DIRS "." "include" "ui" "hal" "settings"
    REQUIRES spi_flash fatfs esp_wifi json esp_lcd driver nvs_flash esp_timer esp_adc usb usb_host_msc
        esp_http_client esp_websocket_client mbedtls esp-tflite-micro
    PRIV_REQUIRES ed25519_lib minimp3 microfrontend microlink
    EMBED_FILES assets/coin16.wav assets/error.wav assets/usb_connected.wav assets/usb_disconnected.wav
        assets/hey_snorri.tflite assets/vad.tflite
)


nvs_create_partition_image(nvs ../nvs.csv FLASH_IN_PROJECT)
