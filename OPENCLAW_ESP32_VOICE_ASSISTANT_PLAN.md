# OpenClaw Voice Assistant — ESP32-S3-Touch-LCD-1.85C Implementation Plan

## Project Goal

Build an Amazon Echo-like desk voice assistant using a **Waveshare ESP32-S3-Touch-LCD-1.85C** board, integrated with an existing **OpenClaw** instance. The device should support:

- **Wake word detection** (on-device, initially "Hey Jarvis", custom wake word later)
- **Voice input** via onboard microphone → streamed to server-side Whisper STT
- **LLM responses** via OpenClaw gateway over WebSocket
- **Voice output** via server-side Kokoro TTS → streamed to onboard speaker
- **Visual feedback** on the 1.85" round 360×360 touch LCD (listening/thinking/speaking states, response text, status info)

## Infrastructure

| Component | Details |
|-----------|---------|
| **ESP32 board** | Waveshare ESP32-S3-Touch-LCD-1.85C (ESP32-S3, 16MB Flash, 8MB PSRAM, 360×360 round LCD, capacitive touch, I2S mic + speaker, RTC, SD card, battery mgmt) |
| **OpenClaw server** | Linux server, already running |
| **STT** | Whisper (faster-whisper), already running on the Linux server |
| **TTS** | Kokoro TTS, already running on the Linux server |
| **GPU machine** | Separate machine on same network, available for wake word model training |
| **Framework** | ESP-IDF (v5.5+), C/C++ |

## Key Hardware Components (ESP32-S3-Touch-LCD-1.85C)

| Chip | Function | Interface |
|------|----------|-----------|
| ST77916 | QSPI LCD Panel Controller | QSPI |
| CST816S | Capacitive Touch Controller | I2C |
| PCF85063 | Real-Time Clock | I2C |
| TCA9554PWR | 8-bit GPIO Expander | I2C |
| I2S codec | Speaker output + Microphone input | I2S |
| ADC | Battery voltage monitoring | ADC |
| SDMMC | SD Card interface | SDMMC |

## Reference Repositories

These repos have been reviewed and contain code to be used or referenced:

### 1. `d4rkmen/flatsphere` — Hardware Abstraction Layer (PRIMARY for hardware)
- **URL:** https://github.com/d4rkmen/flatsphere
- **License:** Apache 2.0
- **Why:** Complete, tested HAL for the exact 1.85C board. Contains drivers for ALL hardware components: display (ST77916), touch (CST816S), RTC (PCF85063), GPIO expander (TCA9554PWR), I2S speaker/mic, battery ADC, SD card, USB, WiFi.
- **Framework:** ESP-IDF v5.5+, LVGL 9, PicoTTS
- **Use:** Extract the entire `main/hal/` directory as the hardware foundation. Also use LVGL setup and display initialization code.
- **Key structure:**
  ```
  main/
  ├── hal/
  │   ├── bat/battery.*        # ADC battery monitoring
  │   ├── button/button.*      # Debounced button handler
  │   ├── display/display_panel.*  # ST77916 QSPI LCD
  │   ├── display/display_touch.*  # CST816S touch
  │   ├── exio/exio.*          # TCA9554PWR GPIO expander
  │   ├── i2c/i2c_master.*     # I2C bus
  │   ├── mic/mic.*            # I2S microphone input
  │   ├── rtc/rtc.*            # PCF85063 RTC
  │   ├── sdcard/sdcard.*      # SDMMC card
  │   ├── speaker/speaker.*    # I2S audio output with mixing
  │   └── wifi/wifi.*          # WiFi management
  ├── ui/                      # LVGL UI (SquareLine Studio export)
  └── main.cpp
  ```
- **Note:** PicoTTS in this project requires a SPIRAM modification (`heap_caps_malloc(PICO_MEM_SIZE, MALLOC_CAP_SPIRAM)` instead of `malloc`). We won't need local TTS since we use server-side Kokoro, but the speaker HAL is essential.

### 2. `omeriko9/HeyClawy` — OpenClaw Voice Integration (PRIMARY for OpenClaw protocol)
- **URL:** https://github.com/omeriko9/HeyClawy
- **License:** Not explicitly stated (check repo)
- **Why:** Voice-first ESP32 companion purpose-built for OpenClaw. Implements the full voice pipeline: wake word → record → stream to STT → OpenClaw gateway (WebSocket) → TTS response → speaker.
- **Framework:** ESP-IDF v5.5+
- **Use:** Port the OpenClaw WebSocket client, audio streaming pipeline, STT/TTS service integration, and wake word (WakeNet) integration. HeyClawy currently supports SenseCAP Watcher, M5StickC Plus2, and Waveshare Audio Board — the 1.85C is NOT yet supported but the architecture is board-agnostic.
- **Key components to extract:**
  - OpenClaw WebSocket client (chat, live task updates, cron notifications, status/activity)
  - Audio capture → STT streaming (to faster-whisper HTTP endpoint)
  - TTS audio reception → speaker playback (from Kokoro/OpenAI-compatible endpoint)
  - WakeNet integration (default "Hey Jarvis" model)
  - Device authentication / key pairing with OpenClaw
  - Local web settings page (runtime config without reflashing)
- **Architecture:**
  - OpenClaw gateway: `ws://<host>:18789`
  - STT service: faster-whisper HTTP endpoint (default port 5051)
  - TTS service: OpenAI-compatible endpoint (default port 5050) — in our case Kokoro TTS
- **Key structure:**
  ```
  main/
  ├── include/secrets.h     # WiFi, OpenClaw host/port/token, device key, TTS/STT config
  components/
  ├── ...                   # Audio, network, OpenClaw protocol components
  tools/
  ├── test_openclaw_auth.py # Device key generation/pairing
  ```

### 3. `78/xiaozhi-esp32` — Display UI Reference (REFERENCE for round-display LVGL patterns)
- **URL:** https://github.com/78/xiaozhi-esp32
- **License:** MIT
- **Why:** Has a working board definition for `waveshare-esp32-s3-touch-lcd-1.85c` with display integration, animated states (listening/thinking/speaking), and a polished round-display UI. Also has custom wake word support.
- **Use as reference only** — the Xiaozhi protocol is different from OpenClaw. Extract UI patterns, LVGL screen layouts for round displays, and animation state machine concepts.
- **Relevant patterns:**
  - LCD display layout using layers (not grids) for round screens
  - Visual state machine: idle → listening → thinking → speaking → idle
  - Emoji/text rendering on 360×360 round display
  - Battery level, WiFi status indicators

### 4. `espressif/esp-skainet` — Wake Word Engine (REFERENCE for WakeNet integration)
- **URL:** https://github.com/espressif/esp-skainet
- **Why:** Espressif's official voice assistant SDK. Contains WakeNet wake word detection and Audio Front End (AFE) with noise suppression, echo cancellation. HeyClawy already wraps this, but refer here for advanced AFE configuration.
- **Available free wake words:** "Hi ESP", "Hi Lexin", "Hey Jarvis"
- **Note:** WakeNet9/9l is the current model version for ESP32-S3.

### 5. `CoreWorxLab/openwakeword-training` — Custom Wake Word Training
- **URL:** https://github.com/CoreWorxLab/openwakeword-training
- **Why:** Dockerized pipeline for training custom openWakeWord models using Kokoro TTS synthetic voices. Produces ~200KB ONNX/TFLite models. Can reuse the existing Kokoro TTS instance for sample generation.
- **Runs on:** The separate GPU machine (not the ESP32, not the OpenClaw server)
- **Output:** `.tflite` model file deployable either server-side (openWakeWord) or on-device (microWakeWord)

### 6. `waveshareteam/Waveshare-ESP32-components` — Official BSP
- **URL:** https://github.com/waveshareteam/Waveshare-ESP32-components
- **Why:** Official Waveshare Board Support Package for ESP-IDF component registry. Can be used alongside or instead of flatsphere's HAL for display/peripheral initialization.

---

## Implementation Phases

### Phase 1: Hardware Foundation & Basic Audio Loop

**Goal:** Get the 1.85C board running with display, touch, mic, and speaker working. Verify audio round-trip (record from mic, play back on speaker).

**Steps:**

1. **Clone and build flatsphere** as the baseline project.
   - Set up ESP-IDF v5.5+ development environment.
   - Clone `https://github.com/d4rkmen/flatsphere.git`
   - Build and flash to verify the board works: `idf.py build && idf.py -p /dev/ttyUSB0 flash monitor`
   - Confirm: LCD displays the clock face, touch responds, speaker plays boot sound, serial monitor shows sensor data.

2. **Strip flatsphere down to a skeleton.**
   - Keep the entire `main/hal/` directory intact.
   - Keep LVGL initialization and display setup.
   - Remove the clock UI, PicoTTS, and SquareLine Studio generated screens.
   - Create a minimal `main.cpp` that initializes all HAL components and shows a blank screen.

3. **Verify audio I/O independently.**
   - Write a simple loopback test: capture audio from mic via `hal/mic/`, play back through `hal/speaker/`.
   - Confirm I2S configuration is correct and audio quality is acceptable.
   - Test at 16kHz mono 16-bit (the format needed for wake word and STT).

4. **Verify WiFi connectivity.**
   - Use `hal/wifi/` to connect to the local network.
   - Confirm the board can reach the OpenClaw server (simple HTTP GET or WebSocket handshake).

**Deliverable:** A skeleton ESP-IDF project with working HAL for all peripherals, verified audio I/O, and WiFi connectivity.

---

### Phase 2: OpenClaw Integration (Voice Pipeline)

**Goal:** Implement the full voice pipeline: button-press → record → STT → OpenClaw → TTS → speaker. No wake word yet — use a physical button or touch gesture to trigger.

**Steps:**

1. **Port HeyClawy's OpenClaw WebSocket client.**
   - Study HeyClawy's `components/` directory for the OpenClaw protocol implementation.
   - Port the WebSocket client that connects to `ws://<openclaw-host>:18789`.
   - Implement device authentication using `tools/test_openclaw_auth.py` to generate a device key pair.
   - Handle: chat messages, task updates, cron notifications, connection state.

2. **Implement STT streaming.**
   - Port HeyClawy's audio capture → STT pipeline.
   - On trigger (button/touch), start recording from the I2S mic.
   - Stream audio to the Whisper STT endpoint (faster-whisper HTTP, same host as OpenClaw).
   - Receive transcription text.
   - HeyClawy sends audio to STT at the configured `STT_HOST:STT_PORT`.

3. **Implement TTS playback.**
   - Port HeyClawy's TTS reception → speaker playback pipeline.
   - Send OpenClaw's text response to Kokoro TTS endpoint (OpenAI-compatible API at `TTS_HOST:TTS_PORT`).
   - Receive audio stream and play through the I2S speaker via `hal/speaker/`.
   - Handle streaming playback (start playing before full response is received).

4. **Wire up the full loop.**
   - Touch screen to trigger → mic records → STT transcribes → OpenClaw processes → TTS speaks → speaker plays.
   - Add HeyClawy's local web settings page for runtime configuration (WiFi, OpenClaw host, TTS/STT endpoints) so you don't need to reflash for config changes.

5. **Create a `secrets.h` configuration file** following HeyClawy's pattern:
   ```c
   // WiFi
   #define WIFI_SSID "your_ssid"
   #define WIFI_PASS "your_pass"
   
   // OpenClaw
   #define OPENCLAW_HOST "192.168.x.x"
   #define OPENCLAW_PORT 18789
   #define OPENCLAW_TOKEN "your_token"
   #define DEVICE_KEY_HEX "your_device_private_key"
   
   // STT (Whisper)
   #define STT_HOST "192.168.x.x"
   #define STT_PORT 5051
   
   // TTS (Kokoro)
   #define TTS_HOST "192.168.x.x"
   #define TTS_PORT 5050
   #define TTS_VOICE "alloy"  // or preferred Kokoro voice
   ```

**Deliverable:** Working voice assistant with touch-to-talk. Full round-trip: speak → transcribe → OpenClaw response → spoken reply.

---

### Phase 3: Wake Word Detection

**Goal:** Add always-on wake word detection so the device activates hands-free.

**Steps:**

1. **Integrate WakeNet via ESP-Skainet's AFE.**
   - Port HeyClawy's wake word integration code.
   - Configure ESP-SR's Audio Front End (AFE) for:
     - Wake word detection (WakeNet9 model)
     - Noise suppression
     - Automatic gain control
   - Use the pre-built "Hey Jarvis" wake word model (free, included with ESP-SR).
   - The AFE runs continuously on one core, processing mic input in 30ms frames.

2. **Implement the state machine for wake word → recording → processing.**
   - **Idle:** AFE listens for wake word, display shows idle screen.
   - **Woken:** Wake word detected → play confirmation sound → start recording user speech.
   - **Listening:** Record until voice activity ends (VAD from AFE) or timeout.
   - **Processing:** Stream audio to STT, send to OpenClaw, wait for response.
   - **Speaking:** Play TTS response through speaker.
   - **Return to Idle.**

3. **Handle audio routing conflicts.**
   - When the speaker is playing TTS, the mic picks up the speaker output. The AFE's echo cancellation helps, but consider:
     - Suppressing wake word detection while TTS is playing.
     - Or using the AFE's AEC (Acoustic Echo Cancellation) with a reference signal from the speaker output.

4. **Partition scheme.**
   - WakeNet models require dedicated flash partitions. Use a partition scheme like:
     `"ESP SR 16M (3MB APP/7MB SPIFFS/2.9MB MODEL)"` — the Waveshare wiki recommends this for speech recognition on this board.
   - This is configured in `menuconfig` under Partition Table.

**Deliverable:** Hands-free voice assistant. Say "Hey Jarvis" → device activates → speak → get response.

---

### Phase 4: Display UI

**Goal:** Build a polished round-display LVGL UI with visual feedback for all assistant states.

**Steps:**

1. **Design the visual state machine.**
   Reference xiaozhi-esp32's round display patterns. Implement screens/states:
   - **Idle:** Clock face or ambient display (time, date, battery %, WiFi status). Use `hal/rtc/` for time, `hal/bat/` for battery.
   - **Listening:** Animated waveform or pulsing circle indicating mic is active.
   - **Thinking:** Spinner or animated dots while waiting for OpenClaw response.
   - **Speaking:** Text of the response scrolling on screen, with audio waveform visualization.
   - **Error:** Connection lost, timeout, etc.

2. **Implement using LVGL 9.**
   - Flatsphere already initializes LVGL with the ST77916 driver and CST816S touch. Reuse this setup.
   - Create round-optimized layouts (the display is circular 360×360, so avoid content in corners).
   - Consider using SquareLine Studio v1.5.4 (what flatsphere uses) for rapid UI prototyping, then export LVGL code.
   - Use LVGL animations (`lv_anim_t`) for smooth state transitions.

3. **Add touch interactions.**
   - Tap to interrupt/cancel current response.
   - Swipe for settings or history.
   - Long press as alternative trigger (in addition to wake word).

4. **Optimize rendering performance.**
   - Use LVGL's partial refresh (not full screen redraw).
   - Offload LVGL rendering to the second core if audio processing is on the first.
   - Target 30fps for animations, 10fps for static states.

**Deliverable:** Polished visual assistant with animated state transitions on the round display.

---

### Phase 5: Custom Wake Word Training

**Goal:** Train a custom wake word model to replace "Hey Jarvis" with a personalized phrase.

**Steps:**

1. **Set up the training environment on the GPU machine.**
   ```bash
   git clone https://github.com/CoreWorxLab/openwakeword-training.git
   cd openwakeword-training
   docker compose build trainer
   docker compose run --rm trainer ./setup-data.sh
   ```

2. **Configure the wake word.**
   - Choose a 2-word phrase (e.g., "Hey [Name]", "Okay [Name]"). Two-word phrases with a prefix like "Hey" or "Okay" produce significantly better models than single words.
   - The training pipeline will use Kokoro TTS to generate ~13,000 synthetic positive samples across 67 voices with speed variation (0.7–1.3x).
   - Negative samples are generated automatically (clearly different phrases like "hello", "hey siri", "alexa"). Do NOT use similar-sounding negatives — they hurt performance.

3. **Optionally record real voice samples for improved accuracy.**
   ```bash
   # On host machine with microphone
   pip install pyaudio numpy scipy
   python record_samples.py --wake-word "hey <name>"
   # Record 20-50 samples — these get weighted 3x in training
   ```

4. **Train the model.**
   - Training takes 4-8 hours on GPU.
   - Output: `my_custom_model/<wake_word>.onnx` (~200KB)
   - Test on host: `python test_model.py --model my_custom_model/<wake_word>.onnx`

5. **Deploy the trained model.**

   **Option A: Server-side (simpler, recommended first).**
   - Install openWakeWord on the OpenClaw Linux server.
   - Place the `.onnx` model in the openWakeWord models directory.
   - The ESP32 streams audio continuously to the server; the server runs wake word detection.
   - Pro: Easy to iterate on models. Con: Constant audio streaming over WiFi, slight latency.

   **Option B: On-device via microWakeWord (better UX, more complex).**
   - Convert the trained model to `.tflite` format (the training pipeline can output this).
   - Flash the `.tflite` model into the ESP32's SPIFFS/model partition.
   - Replace WakeNet with microWakeWord for on-device detection.
   - Pro: No streaming until wake word detected, lower latency. Con: More constrained model size, harder to iterate.
   - Reference: `https://github.com/OHF-Voice/micro-wake-word` for the TFLite-based on-device framework.

**Deliverable:** Custom wake word model trained on synthetic voices, deployed either server-side or on-device.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                   ESP32-S3-Touch-LCD-1.85C               │
│                                                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐ │
│  │ WakeNet  │  │ I2S Mic  │  │ I2S Spkr │  │  LVGL   │ │
│  │ (AFE)    │◄─┤ (16kHz)  │  │ (playback)│  │  UI     │ │
│  └────┬─────┘  └────┬─────┘  └────▲─────┘  └────▲────┘ │
│       │              │              │              │      │
│       │  wake        │  audio       │  audio       │      │
│       │  detected    │  frames      │  stream      │ state│
│       ▼              ▼              │              │      │
│  ┌─────────────────────────────────────────────────────┐ │
│  │              State Machine / App Logic               │ │
│  │  idle → listening → processing → speaking → idle    │ │
│  └──────────────────────┬──────────────────────────────┘ │
│                         │ WiFi                           │
└─────────────────────────┼────────────────────────────────┘
                          │
              ┌───────────┼───────────┐
              │     Local Network     │
              │                       │
    ┌─────────▼─────────┐   ┌────────▼────────┐
    │  OpenClaw Gateway  │   │  Whisper STT    │
    │  ws://host:18789   │   │  http://host:   │
    │                    │   │  5051            │
    │  ┌──────────────┐  │   └─────────────────┘
    │  │ LLM Backend  │  │
    │  │ (Claude/etc) │  │   ┌─────────────────┐
    │  └──────────────┘  │   │  Kokoro TTS     │
    └────────────────────┘   │  http://host:   │
                             │  5050            │
                             └─────────────────┘
```

## File Structure Target

```
openclaw-voice-assistant/
├── CMakeLists.txt
├── partitions.csv                 # 16MB: 3MB APP / 7MB SPIFFS / 2.9MB MODEL
├── sdkconfig.defaults
├── secrets.h.example
├── main/
│   ├── main.cpp                   # App entry, state machine
│   ├── hal/                       # From flatsphere (Apache 2.0)
│   │   ├── bat/
│   │   ├── button/
│   │   ├── display/
│   │   ├── exio/
│   │   ├── i2c/
│   │   ├── mic/
│   │   ├── rtc/
│   │   ├── sdcard/
│   │   ├── speaker/
│   │   └── wifi/
│   ├── openclaw/                  # From HeyClawy (ported)
│   │   ├── openclaw_client.*      # WebSocket client
│   │   ├── openclaw_auth.*        # Device key auth
│   │   └── openclaw_protocol.*    # Message parsing
│   ├── voice/                     # From HeyClawy (ported)
│   │   ├── stt_client.*           # Whisper STT HTTP streaming
│   │   ├── tts_client.*           # Kokoro TTS HTTP streaming
│   │   ├── wake_word.*            # WakeNet / AFE integration
│   │   └── audio_pipeline.*       # Mic → STT, TTS → Speaker routing
│   ├── ui/                        # New, referencing xiaozhi-esp32 patterns
│   │   ├── ui_idle.*
│   │   ├── ui_listening.*
│   │   ├── ui_thinking.*
│   │   ├── ui_speaking.*
│   │   ├── ui_error.*
│   │   ├── ui_settings.*
│   │   └── ui_manager.*           # State-driven screen transitions
│   └── config/
│       ├── settings.*             # NVS-based persistent config
│       └── web_settings.*         # Local HTTP config page
├── components/                    # ESP-IDF components
│   └── ...
└── tools/
    ├── test_openclaw_auth.py      # From HeyClawy
    └── wake_word_training/        # Reference to CoreWorxLab pipeline
```

## Build & Flash

```bash
# Setup
git clone <this-repo>
cd openclaw-voice-assistant
cp secrets.h.example main/include/secrets.h
# Edit secrets.h with your WiFi, OpenClaw, STT, TTS config

# Build
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py menuconfig  # Set partition scheme, PSRAM, WiFi, etc.
idf.py build

# Flash
idf.py -p /dev/ttyUSB0 flash monitor
```

## Dependencies

| Component | Version | Source |
|-----------|---------|--------|
| ESP-IDF | ≥ 5.5.1 | Espressif |
| LVGL | ^9.4.0 | Via ESP component registry |
| ESP-SR (WakeNet/AFE) | Latest | Via ESP component registry |
| ESP-ADF (audio) | Latest | Espressif Audio Development Framework |

## Notes for Implementation

- **PSRAM is critical.** The 8MB PSRAM must be enabled (`CONFIG_SPIRAM=y`, octal mode, 80MHz). LVGL frame buffers, audio buffers, and WakeNet models all go into PSRAM. Use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for large allocations.
- **Dual-core task allocation.** Run the AFE/wake word on Core 0, and the app logic + LVGL rendering on Core 1. Audio streaming (I2S DMA) runs via hardware with minimal CPU involvement.
- **The ST77916 display uses QSPI** (not standard SPI). Flatsphere's driver handles this, but note that Tasmota and some other frameworks don't support QSPI displays yet.
- **Audio format:** WakeNet and Whisper STT both expect 16kHz, mono, 16-bit signed PCM. Ensure the I2S mic is configured to this spec.
- **HeyClawy's web settings page** is extremely useful for development — it lets you change WiFi, server endpoints, audio thresholds, and wake word sensitivity without reflashing. Prioritize porting this early.
- **The board has a single I2S bus shared between mic and speaker.** Check flatsphere's HAL for how it handles half-duplex or time-multiplexed I2S. HeyClawy's audio pipeline may assume separate I2S channels — verify and adapt.
