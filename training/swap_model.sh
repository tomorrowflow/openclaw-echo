#!/usr/bin/env bash
# Swap the wake word model in the firmware
# Usage: ./training/swap_model.sh <path_to_new.tflite> [path_to_manifest.json]
#
# This script:
# 1. Copies the new .tflite to vendor/flatsphere/main/assets/
# 2. Updates CMakeLists.txt EMBED_FILES
# 3. Updates micro_wake.cpp asm symbols and config values
# 4. Optionally reads config from the training JSON manifest
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ASSETS_DIR="$PROJECT_ROOT/vendor/flatsphere/main/assets"
CMAKE_FILE="$PROJECT_ROOT/vendor/flatsphere/main/CMakeLists.txt"
WAKE_FILE="$PROJECT_ROOT/vendor/flatsphere/main/micro_wake.cpp"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <path_to_model.tflite> [path_to_manifest.json]"
    echo ""
    echo "Example:"
    echo "  $0 training/models/hey_snorri.tflite training/models/hey_snorri.json"
    exit 1
fi

TFLITE_PATH="$1"
JSON_PATH="${2:-}"

if [ ! -f "$TFLITE_PATH" ]; then
    echo "Error: TFLite file not found: $TFLITE_PATH"
    exit 1
fi

MODEL_FILE="$(basename "$TFLITE_PATH")"

echo "=== Swapping wake word model ==="
echo "Model: $MODEL_FILE"

# Copy model file
echo "1. Copying $MODEL_FILE to assets/"
cp "$TFLITE_PATH" "$ASSETS_DIR/$MODEL_FILE"

# Run Python to do the actual file modifications
python3 "$SCRIPT_DIR/_swap_model_impl.py" \
    "$MODEL_FILE" \
    "$CMAKE_FILE" \
    "$WAKE_FILE" \
    "$ASSETS_DIR" \
    ${JSON_PATH:+"$JSON_PATH"}

echo ""
echo "=== Model swap complete! ==="
echo ""
echo "To build and flash:"
echo "  source ~/esp/esp-idf/export.sh"
echo "  cd vendor/flatsphere"
echo "  idf.py build && idf.py -p /dev/cu.usbmodem1101 flash monitor"
echo ""
echo "Quick tuning (no retraining needed):"
echo "  - Edit WAKE_CUTOFF_U8 in micro_wake.cpp"
echo "    Lower (e.g., 230) = more sensitive"
echo "    Higher (e.g., 250) = fewer false positives"
echo "  - Edit WAKE_SLIDING_WINDOW: larger = slower but fewer false triggers"
