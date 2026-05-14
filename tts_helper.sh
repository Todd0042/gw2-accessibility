#!/usr/bin/env bash
# tts_helper.sh — Named pipe daemon that receives text from the GW2 addon
# and speaks it via espeak-ng or Piper. Runs natively on Linux.
#
# Usage: ./tts_helper.sh
# Starts a background daemon. Safe to run multiple times.
#
# Pipe path: $HOME/.gw2-tts-pipe
# Config:    $HOME/.gw2-tts-config
#
# Config variables:
#   ENGINE    "espeak-ng" (default) or "piper"
#   VOICE     espeak-ng voice name (default "en-us") or Piper model tag
#   SPEED     espeak-ng words per minute (default 150)
#   PITCH     espeak-ng pitch (default 40)
#   AMPLITUDE espeak-ng volume (default 175)
#   PIPER_MODEL_PATH  override Piper model directory

PIPE="$HOME/.gw2-tts-pipe"
CONFIG="$HOME/.gw2-tts-config"

# ── Defaults ──────────────────────────────────────────────────────────────
ENGINE="espeak-ng"
VOICE="en-us"
SPEED="150"
PITCH="40"
AMPLITUDE="175"

# Auto-detect Piper binary
PIPER_BIN=""
for candidate in "$HOME/piper/piper/piper" "/usr/bin/piper-tts" "/opt/piper-tts/piper" "/usr/bin/piper"; do
    [ -x "$candidate" ] && { PIPER_BIN="$candidate"; break; }
done

# Piper voice search path (created automatically if missing)
PIPER_VOICE_DIR="$HOME/.local/share/piper-voices"

# ── Load config ───────────────────────────────────────────────────────────
if [ -f "$CONFIG" ]; then
    while IFS='=' read -r key value; do
        [[ "$key" =~ ^[[:space:]]*# ]] && continue
        [[ -z "$key" ]] && continue
        key=$(echo "$key" | tr -d '[:space:]')
        value=$(echo "$value" | tr -d '[:space:]' | tr -d '"' | tr -d "'")
        case "$key" in
            ENGINE)             ENGINE="$value" ;;
            VOICE)              VOICE="$value" ;;
            SPEED)              SPEED="$value" ;;
            PITCH)              PITCH="$value" ;;
            AMPLITUDE)          AMPLITUDE="$value" ;;
            PIPER_MODEL_PATH)   PIPER_MODEL_PATH="$value" ;;
        esac
    done < "$CONFIG"
fi

# ── Resolve Piper model file ──────────────────────────────────────────────
resolve_piper_model() {
    local tag="$1"
    # Tag format: en_US-ryan-high
    # Voice dir structure: <lang>/<name>/<quality>/<tag>.onnx
    local search_dir="${PIPER_MODEL_PATH:-$PIPER_VOICE_DIR}"

    # Direct path
    if [ -f "$tag" ]; then
        echo "$tag"
        return 0
    fi
    # Full path to .onnx
    if [ -f "$tag.onnx" ]; then
        echo "$tag.onnx"
        return 0
    fi

    # Search by tag (e.g. "en_US-ryan-high")
    local found
    found=$(find "$search_dir" -name "${tag}.onnx" 2>/dev/null | head -1)
    if [ -n "$found" ]; then
        echo "$found"
        return 0
    fi

    # Search by basename (e.g. "ryan-high")
    found=$(find "$search_dir" -name "*${tag}*.onnx" 2>/dev/null | head -1)
    if [ -n "$found" ]; then
        echo "$found"
        return 0
    fi

    echo ""
    return 1
}

# ── Clean up stale pipe ───────────────────────────────────────────────────
rm -f "$PIPE"
mkfifo "$PIPE"

echo "[gw2-tts-helper] Listening on $PIPE"
if [ "$ENGINE" = "piper" ]; then
    model_file=$(resolve_piper_model "$VOICE")
    if [ -n "$model_file" ]; then
        echo "[gw2-tts-helper] Engine=Piper Voice=$VOICE Model=$model_file"
    else
        echo "[gw2-tts-helper] WARNING: Piper model '$VOICE' not found, falling back to espeak-ng"
        ENGINE="espeak-ng"
    fi
else
    echo "[gw2-tts-helper] Engine=espeak-ng Voice=$VOICE Speed=$SPEED Pitch=$PITCH Amp=$AMPLITUDE"
fi
echo "[gw2-tts-helper] Press Ctrl+C to stop"

cleanup() {
    rm -f "$PIPE"
    echo "[gw2-tts-helper] Stopped"
    exit 0
}
trap cleanup INT TERM

# ── Main loop ─────────────────────────────────────────────────────────────
while true; do
    while IFS= read -r line; do
        if [ -n "$line" ]; then
            case "$ENGINE" in
                piper)
                    model_file=$(resolve_piper_model "$VOICE")
                    if [ -n "$model_file" ] && [ -n "$PIPER_BIN" ]; then
                        LD_LIBRARY_PATH="$(dirname "$PIPER_BIN"):$LD_LIBRARY_PATH" \
                            echo "$line" | "$PIPER_BIN" \
                            --model "$model_file" --output-raw 2>/dev/null |
                            aplay -r 22050 -f S16_LE -c 1 2>/dev/null &
                    else
                        # Fallback to espeak-ng if piper unavailable
                        espeak-ng -v "$VOICE" -s "$SPEED" -p "$PITCH" -a "$AMPLITUDE" "$line" 2>/dev/null &
                    fi
                    ;;
                *)
                    espeak-ng -v "$VOICE" -s "$SPEED" -p "$PITCH" -a "$AMPLITUDE" "$line" 2>/dev/null &
                    ;;
            esac
        fi
    done < "$PIPE"
    sleep 0.1
done
