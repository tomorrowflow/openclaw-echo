#!/usr/bin/env bash
# Setup script for microWakeWord training environment
# Run this on the GPU machine (2x RTX 3090)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRAINER_DIR="${SCRIPT_DIR}/microWakeWord-Trainer-Nvidia-Docker"

echo "=== microWakeWord Training Environment Setup ==="

# Clone the NVIDIA Docker trainer if not already present
if [ ! -d "$TRAINER_DIR" ]; then
    echo "Cloning TaterTotterson's NVIDIA Docker trainer..."
    git clone https://github.com/TaterTotterson/microWakeWord-Trainer-Nvidia-Docker.git "$TRAINER_DIR"
else
    echo "Trainer repo already cloned at $TRAINER_DIR"
fi

cd "$TRAINER_DIR"

# Build Docker image
echo "Building Docker image (this may take a while on first run)..."
docker build -t microwakeword-trainer .

# Create data directory for persistent storage
mkdir -p data

echo ""
echo "=== Setup complete! ==="
echo ""
echo "To start training, run:"
echo "  cd $SCRIPT_DIR"
echo "  docker compose up --build"
echo ""
echo "Then open https://localhost:9443 in your browser (accept the self-signed certificate)."
echo ""
echo "Note: HTTPS is required for browser microphone access."
echo "Caddy reverse proxy provides automatic self-signed TLS."
echo ""
echo "In the notebook, set:"
echo "  target_word = 'hey_snorri'"
echo ""
echo "If Piper TTS mispronounces 'Snorri', try these alternatives:"
echo "  target_word = 'hey_snorree'"
echo "  target_word = 'hey_snorrey'"
echo ""
echo "After training, copy the .tflite and .json files to:"
echo "  $SCRIPT_DIR/models/"
echo ""
echo "Then run the swap script from the project root:"
echo "  ./training/swap_model.sh training/models/<model_name>.tflite"
