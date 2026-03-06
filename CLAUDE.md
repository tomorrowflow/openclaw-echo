# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

OpenClaw Echo is an ESP32-S3 voice assistant (Amazon Echo-like) built for the **Waveshare ESP32-S3-Touch-LCD-1.85C** board, integrated with an existing OpenClaw instance. The device handles wake word detection on-device, streams audio to server-side STT (faster-whisper) and TTS (Kokoro), and communicates with OpenClaw over WebSocket.

**Current status:** Planning phase — see `OPENCLAW_ESP32_VOICE_ASSISTANT_PLAN.md` for the full implementation roadmap.

## Build System

ESP-IDF v5.5+ with CMake. Target: ESP32-S3.

```bash
# Environment setup
. $IDF_PATH/export.sh
idf.py set-target esp32s3

# Build, flash, and monitor
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor

# Configuration (partition scheme, PSRAM, WiFi, etc.)
idf.py menuconfig
```

Credentials go in `main/include/secrets.h` (copied from `secrets.h.example`). Never commit this file.

## Architecture

**State machine:** Idle → Listening → Processing → Speaking → Idle

**Three layers:**
- `main/hal/` — Hardware abstraction (ported from [flatsphere](https://github.com/d4rkmen/flatsphere), Apache 2.0): display (ST77916 QSPI), touch (CST816S), I2S mic/speaker, RTC, GPIO expander, battery ADC, SD card, WiFi
- `main/openclaw/` + `main/voice/` — Communication layer (ported from [HeyClawy](https://github.com/omeriko9/HeyClawy)): OpenClaw WebSocket client (`ws://host:18789`), STT streaming to faster-whisper (`host:5051`), TTS from Kokoro (`host:5050`)
- `main/ui/` — LVGL 9 round-display UI with animated state transitions (referencing [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) patterns)

**Core allocation:** AFE/wake word on Core 0, app logic + LVGL on Core 1.

## Key Hardware Constraints

- **PSRAM required** — 8MB PSRAM in octal mode at 80MHz. Use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for large allocations (LVGL frame buffers, audio buffers, WakeNet models).
- **QSPI display** — ST77916 uses QSPI, not standard SPI. Only flatsphere's driver and Waveshare BSP support this.
- **Shared I2S bus** — Single I2S bus for both mic and speaker. Flatsphere's HAL handles the multiplexing.
- **Audio format:** 16kHz, mono, 16-bit signed PCM everywhere (WakeNet, Whisper STT, I2S config).
- **Partition scheme:** 16MB flash = 3MB APP / 7MB SPIFFS / 2.9MB MODEL (for WakeNet).

## Dependencies

| Component | Version |
|-----------|---------|
| ESP-IDF | >= 5.5.1 |
| LVGL | ^9.4.0 |
| ESP-SR (WakeNet/AFE) | Latest via ESP component registry |
| ESP-ADF | Latest |
