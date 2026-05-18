# Linux TTS Setup for GW2 Accessibility Addon

On Linux/Wine, the addon detects it's not running on native Windows and switches from SAPI to a pipe-based TTS system. Text is written to a named pipe at `~/.gw2-tts-pipe`, and a small helper script (`tts_helper.sh`) reads from that pipe and speaks it using **Piper** (default) or **espeak-ng** natively on Linux.

## TTS Engine Options

### Piper (default — recommended)

Piper is a neural TTS engine that produces much more natural-sounding speech than espeak-ng.

**Install Piper:**

Download the pre-built binary from the [Piper releases page](https://github.com/rhasspy/piper/releases). Extract it so that `piper/piper` exists inside your home directory, or place the binary at `/usr/bin/piper-tts`.

The helper script searches these locations automatically:
- `~/piper/piper/piper`
- `/usr/bin/piper-tts`
- `/opt/piper-tts/piper`
- `/usr/bin/piper`

**Download a voice model:**

Voice models go in `~/.local/share/piper-voices/`. Each voice needs two files: `<name>.onnx` and `<name>.onnx.json`.

Good English voices:
| Voice tag | Style |
|-----------|-------|
| `en_US-ryan-high` | US English, male, high quality |
| `en_US-lessac-high` | US English, female, high quality |
| `en_GB-alan-medium` | British English, male |

Download from [rhasspy/piper-voices on Hugging Face](https://huggingface.co/rhasspy/piper-voices/tree/main/en). Each `.onnx` file has a matching `.onnx.json` — download both.

### espeak-ng (fallback)

espeak-ng is fast and lightweight but sounds robotic. It is used automatically if Piper is not found, and can be selected explicitly in the addon settings.

| Distro | Command |
|--------|---------|
| Arch / CachyOS | `sudo pacman -S espeak-ng` |
| Ubuntu / Debian | `sudo apt install espeak-ng` |
| Fedora | `sudo dnf install espeak-ng` |
| openSUSE | `sudo zypper install espeak-ng` |

Common espeak-ng voices: `en-us`, `en-gb`, `en-gb-scotland`, `en-029` (Caribbean English).

## Setup

### 1. Start the TTS helper daemon

```bash
cd ~/Documents/GitHub/gw2-accessibility
chmod +x tts_helper.sh
./tts_helper.sh
```

This creates a named pipe at `~/.gw2-tts-pipe` and waits for text. Leave it running while playing.

### 2. Auto-start: systemd user service (recommended)

Copy the provided service file and enable it so the daemon starts at login:

```bash
cp ~/Documents/GitHub/gw2-accessibility/systemd/gw2-tts-helper.service \
   ~/.config/systemd/user/gw2-tts-helper.service

systemctl --user daemon-reload
systemctl --user enable --now gw2-tts-helper.service
```

The service file uses `%h` which systemd expands to your home directory — no path editing needed if your repo is at `~/Documents/GitHub/gw2-accessibility`.

Check status and logs:
```bash
systemctl --user status gw2-tts-helper.service
tail -f /tmp/tts_helper.log
```

### 3. Auto-start: Lutris prelaunch (alternative)

Edit your Guild Wars 2 game config YAML (usually `~/.local/share/lutris/games/guild-wars-2-*.yml`) and add:

```yaml
system:
  prelaunch: bash -c 'pgrep -f tts_helper.sh >/dev/null || nohup /home/YOUR_USER/Documents/GitHub/gw2-accessibility/tts_helper.sh >/tmp/tts_helper.log 2>&1 &'
  prelaunch_sleep: 1
```

### 4. Auto-start: Steam/Proton launch options

```bash
bash /path/to/gw2-accessibility/tts_helper.sh & sleep 1; %command%
```

## In-Game Settings

When the addon detects it's running under Wine/Proton, the **Auditory → TTS Voice & Output** section shows Linux-specific controls instead of Windows SAPI controls:

| Setting | Description |
|---------|-------------|
| **Engine** | `piper` (high quality) or `espeak-ng` (lightweight) |
| **Voice** | Piper: voice tag like `en_US-ryan-high`. espeak-ng: voice name like `en-us` |
| **Speed (WPM)** | Words per minute, 80–500 (espeak-ng only; Piper ignores this) |
| **Pitch** | 0–99 (espeak-ng only) |
| **Amplitude** | Volume 0–200 (espeak-ng only) |

Click **Apply & Reload** after changing settings. This writes `~/.gw2-tts-config` and sends a `CMD:RELOAD` signal to the running daemon — no restart needed.

## Manual Configuration

You can also edit `~/.gw2-tts-config` directly:

```ini
ENGINE=piper
VOICE=en_US-ryan-high
SPEED=150
PITCH=40
AMPLITUDE=175
```

Signal the running daemon to reload after editing:
```bash
echo "CMD:RELOAD" > ~/.gw2-tts-pipe
```

Or restart the service:
```bash
systemctl --user restart gw2-tts-helper.service
```

## How It Works

1. The addon detects Wine via the `wine_get_version` export in `ntdll.dll` (most reliable method — never present on real Windows)
2. SAPI `Speak()` calls fail under Wine — the addon activates the pipe fallback
3. Text is written line-by-line to `~/.gw2-tts-pipe` (accessed as `Z:\.gw2-tts-pipe` from Wine)
4. `tts_helper.sh` reads from the pipe and invokes Piper or espeak-ng
5. Audio plays through PipeWire/PulseAudio normally

Control commands prefixed with `CMD:` are processed by the daemon instead of being spoken:
- `CMD:RELOAD` — re-reads `~/.gw2-tts-config` and applies new settings live (no restart needed)

## Troubleshooting

**No sound:**
```bash
# Check daemon is running
systemctl --user status gw2-tts-helper.service
# Test the pipe manually
echo "hello world" > ~/.gw2-tts-pipe
# Check logs
tail /tmp/tts_helper.log
```

**"Pipe open failed" in Nexus log:**
- Make sure `tts_helper.sh` is running before starting GW2
- Verify: `ls -la ~/.gw2-tts-pipe` (should show `p` prefix = named pipe)
- Wine maps `Z:\` to `/` — verify with `explorer Z:\` from within Wine

**Piper model not found:**
```bash
ls ~/.local/share/piper-voices/
# Should contain en_US-ryan-high.onnx and en_US-ryan-high.onnx.json
# Test piper directly:
echo "hello" | ~/piper/piper/piper --model ~/.local/share/piper-voices/en_US-ryan-high.onnx --output-raw | aplay -r 22050 -f S16_LE -c 1
```

**espeak-ng voice sounds wrong:**
```bash
espeak-ng --voices=en   # list available English voices
```
