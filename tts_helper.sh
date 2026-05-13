#!/usr/bin/env bash
# tts_helper.sh — Named pipe daemon that receives text from the GW2 addon
# and speaks it via espeak-ng. Runs natively on Linux.
#
# Usage: ./tts_helper.sh
# Starts a background daemon. Safe to run multiple times.
#
# Pipe path: $HOME/.gw2-tts-pipe
# Config:    $HOME/.gw2-tts-config

PIPE="$HOME/.gw2-tts-pipe"
CONFIG="$HOME/.gw2-tts-config"

# ── Load config (defaults if file missing) ──────────────────────────────
VOICE="en-us"
SPEED="150"
PITCH="40"
AMPLITUDE="175"

if [ -f "$CONFIG" ]; then
    while IFS='=' read -r key value; do
        # Skip comments and blank lines
        [[ "$key" =~ ^[[:space:]]*# ]] && continue
        [[ -z "$key" ]] && continue
        key=$(echo "$key" | tr -d '[:space:]')
        value=$(echo "$value" | tr -d '[:space:]' | tr -d '"' | tr -d "'")
        case "$key" in
            VOICE)      VOICE="$value" ;;
            SPEED)      SPEED="$value" ;;
            PITCH)      PITCH="$value" ;;
            AMPLITUDE)  AMPLITUDE="$value" ;;
        esac
    done < "$CONFIG"
fi

# Clean up stale pipe
rm -f "$PIPE"
mkfifo "$PIPE"

echo "[gw2-tts-helper] Listening on $PIPE"
echo "[gw2-tts-helper] Voice=$VOICE Speed=$SPEED Pitch=$PITCH Amp=$AMPLITUDE"
echo "[gw2-tts-helper] Press Ctrl+C to stop"

cleanup() {
    rm -f "$PIPE"
    echo "[gw2-tts-helper] Stopped"
    exit 0
}
trap cleanup INT TERM

# Main loop: read lines from the pipe and speak each one
while true; do
    # Open pipe for reading; blocks until a writer connects
    while IFS= read -r line; do
        if [ -n "$line" ]; then
            espeak-ng -v "$VOICE" -s "$SPEED" -p "$PITCH" -a "$AMPLITUDE" "$line" 2>/dev/null &
        fi
    done < "$PIPE"
    # Writer disconnected, loop back to wait for next connection
    sleep 0.1
done
