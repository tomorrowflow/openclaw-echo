# OpenClaw Echo

An ESP32-S3 voice assistant (Amazon Echo-like) built for the **Waveshare ESP32-S3-Touch-LCD-1.85C V2** board, integrated with [OpenClaw](https://github.com/openclaw).

Say "Hey Jarvis" to activate, speak your request, and hear the AI response through the speaker.

## Status

**Phases 1-4 complete** (hardware foundation, voice pipeline, wake word, Tailscale VPN). Phase 5 (display UI rework) is next.

### What works

- **Wake word detection** — "Hey Jarvis" via microWakeWord TFLite models running on Core 0
- **Voice activity detection** — VAD model for automatic end-of-speech silence detection
- **Speech-to-text** — Records 16kHz mono audio, sends WAV to faster-whisper HTTP endpoint
- **OpenClaw chat** — WebSocket connection with ED25519 device authentication, auto-reconnect with retry
- **Text-to-speech** — Kokoro TTS via OpenAI-compatible API, MP3 decode (minimp3), 24kHz speaker output
- **Tailscale VPN** — MicroLink (WireGuard + DERP relay) for secure connectivity to OpenClaw server
- **BOOT button** — Fallback trigger when wake word isn't desired

### Known issues

- **Display SPI errors** — Intermittent `panel_io_spi_tx_color` failures during state transitions due to low internal DMA RAM. Non-blocking (display recovers on next flush). Will be addressed in Phase 5 UI rework.
- **OpenClaw empty responses** — Occasional empty "final" events from OpenClaw (no message in payload). Handled gracefully (skips TTS).

## Hardware

| Component | Chip | Interface |
|-----------|------|-----------|
| Display | ST77916 360x360 round LCD | QSPI (80MHz) |
| Touch | CST816S capacitive | I2C |
| Mic ADC | ES7210 (4-ch, MIC1 active) | I2S TDM (24kHz) |
| Speaker DAC | ES8311 | I2S STD (24kHz) |
| RTC | PCF85063 | I2C |
| GPIO Expander | TCA9554PWR | I2C |
| VPN | MicroLink (WireGuard + Tailscale) | WiFi |

## Architecture

```
Core 0: Wake word task (always running)
  I2S mic 24kHz 4ch TDM -> extract MIC1 -> downsample 16kHz
  -> mel spectrogram -> hey_jarvis.tflite -> wake detection
  -> vad.tflite -> voice activity detection
  -> copies audio to recording buffer when active

Core 1: App task (state machine)
  IDLE -> RECORDING -> TRANSCRIBING -> CHATTING -> SPEAKING -> IDLE
  + MicroLink VPN maintenance
  + LVGL display updates
```

## Building

### Prerequisites

- ESP-IDF v5.5.1+
- Waveshare ESP32-S3-Touch-LCD-1.85C V2 board

### Setup

```bash
source ~/esp/esp-idf/export.sh

# Copy and edit credentials
cp vendor/flatsphere/main/include/secrets.h.example vendor/flatsphere/main/include/secrets.h
# Edit secrets.h with your WiFi, OpenClaw, STT, TTS, and Tailscale credentials

# Build and flash
cd vendor/flatsphere
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

**Important:** Do NOT run `idf.py set-target` — the shipped `sdkconfig` already targets ESP32-S3 with required LVGL font settings.

### Binary size

~1.9MB (52% free in 4MB app partition). No partition table changes needed.

## Project Structure

```
vendor/flatsphere/
  main/
    main.cpp                  # Voice pipeline state machine (Core 1)
    micro_wake.cpp            # Wake word detection (Core 0)
    openclaw_client.c         # OpenClaw WebSocket client + ED25519 auth
    stt_client.c              # STT HTTP client (faster-whisper)
    tts_client.c              # TTS HTTP client (Kokoro) + MP3 decoder
    include/
      micro_wake.h            # Wake word API
      openclaw_client.h       # OpenClaw API
      stt_client.h            # STT API
      tts_client.h            # TTS API
      secrets.h.example       # Credentials template
    assets/
      hey_jarvis.tflite       # Wake word model (52KB)
      vad.tflite              # Voice activity model (34KB)
    hal/                      # Hardware abstraction (from flatsphere)
  components/
    ed25519_lib/              # ED25519 crypto (from HeyClawy)
    minimp3/                  # MP3 decoder (header-only)
    microfrontend/            # TFLite audio preprocessing
    microlink/                # Tailscale VPN for ESP32
```

## Dependencies

| Component | Source |
|-----------|--------|
| ESP-IDF | >= 5.5.1 |
| LVGL | ^9.4.0 (via ESP component registry) |
| esp-tflite-micro | ^1.0.0 (via ESP component registry) |
| esp_codec_dev | ^1.4.0 (via ESP component registry) |
| esp_websocket_client | via ESP component registry |
| MicroLink | v3.0.0 (vendored, MIT) |
| flatsphere HAL | Apache 2.0 (vendored) |
| HeyClawy components | MIT (ported) |
| microWakeWord models | esphome/micro-wake-word-models v2 |

## Custom Wake Word Training

Train a custom wake word (e.g., "Hey Snorri") using [TaterTotterson's microWakeWord NVIDIA Docker trainer](https://github.com/TaterTotterson/microWakeWord-Trainer-Nvidia-Docker). Requires a machine with an NVIDIA GPU.

### Setup (on GPU machine)

```bash
git clone https://github.com/tomorrowflow/openclaw-echo.git
cd openclaw-echo
./training/setup.sh
```

### Train

```bash
cd training/microWakeWord-Trainer-Nvidia-Docker
docker run --rm -it --gpus all -p 8888:8888 -v $(pwd)/data:/data microwakeword-trainer
```

Open http://localhost:8888 in your browser:

1. Enter your wake word (e.g., `hey_snorri`)
2. Click **Test TTS** — if pronunciation is off, try `hey_snorree` or `hey_snorrey`
3. (Optional) Record your voice — set speakers/takes, speak naturally. Improves real-world accuracy.
4. Click **Train** — first run downloads ~5GB of datasets (cached), then trains (~30 min on a 3090)

Output (`.tflite` + `.json`) appears in `training/microWakeWord-Trainer-Nvidia-Docker/data/output/`.

### Flash to device

Copy the trained model and manifest, then run the swap script:

```bash
cp data/output/<timestamp>/hey_snorri.tflite ../../training/models/
cp data/output/<timestamp>/hey_snorri.json ../../training/models/

# From project root:
./training/swap_model.sh training/models/hey_snorri.tflite training/models/hey_snorri.json
```

This updates `CMakeLists.txt`, `micro_wake.cpp` (asm symbols + config) automatically. Then build and flash:

```bash
source ~/esp/esp-idf/export.sh
cd vendor/flatsphere
idf.py build && idf.py -p /dev/cu.usbmodem1101 flash monitor
```

### Tuning (no retraining needed)

Edit `vendor/flatsphere/main/micro_wake.cpp`:
- **WAKE_CUTOFF_U8** — lower (e.g., 230) = more sensitive, higher (e.g., 250) = fewer false positives
- **WAKE_SLIDING_WINDOW** — larger = slower detection but fewer false triggers

## Credits

- [flatsphere](https://github.com/d4rkmen/flatsphere) by d4rkmen — HAL for Waveshare 1.85C (Apache 2.0)
- [HeyClawy](https://github.com/omeriko9/HeyClawy) by omeriko9 — OpenClaw voice integration (MIT)
- [MicroLink](https://github.com/CamM2325/microlink) — Tailscale VPN for ESP32 (MIT)
- [microWakeWord](https://github.com/kahrendt/microWakeWord) — TFLite wake word models
- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — Round display UI patterns reference
