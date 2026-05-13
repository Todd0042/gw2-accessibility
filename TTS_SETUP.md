# Linux TTS Setup for GW2 Accessibility Addon

This addon uses Windows SAPI for text-to-speech, which works natively on Windows. Under Wine/Proton on Linux, SAPI is a stub that doesn't produce audio. This guide sets up a fallback that pipes text from the addon to `espeak-ng` running natively on Linux.

## Prerequisites

Install `espeak-ng`:

| Distro | Command |
|--------|---------|
| Arch / CachyOS | `sudo pacman -S espeak-ng` |
| Ubuntu / Debian | `sudo apt install espeak-ng` |
| Fedora | `sudo dnf install espeak-ng` |
| openSUSE | `sudo zypper install espeak-ng` |

## Setup

### 1. Start the TTS helper daemon

```bash
cd /path/to/gw2-accessibility
chmod +x tts_helper.sh
./tts_helper.sh
```

This creates a named pipe at `~/.gw2-tts-pipe` and waits for text from the addon. Leave this running while playing.

### 2. Auto-start with your game launcher

**Lutris:** Edit your Guild Wars 2 game config YAML (usually `~/.local/share/lutris/games/guild-wars-2-*.yml`) and add:

```yaml
system:
  prelaunch: /path/to/gw2-accessibility/tts_helper.sh &
  prelaunch_sleep: 1
```

**Steam/Proton:** Add to your launch options:
```bash
bash /path/to/gw2-accessibility/tts_helper.sh & sleep 1; %command%
```

### 3. Customize the voice (optional)

Edit `~/.gw2-tts-config`:

```ini
VOICE=en-us
SPEED=150
PITCH=40
AMPLITUDE=175
```

- **VOICE**: Run `espeak-ng --voices=en` to list options. Try `en-gb`, `en-029` (Caribbean), `en-gb-scotland`.
- **SPEED**: Words per minute (default ~175). Range: 80–500.
- **PITCH**: 0–99 (default 50). Higher = more squeaky.
- **AMPLITUDE**: 0–200 (default 100). Controls volume.

Restart `tts_helper.sh` after editing.

## How It Works

1. The addon detects it's running under Wine and that SAPI `Speak()` fails
2. It writes UTF-8 text to the named pipe `~/.gw2-tts-pipe` (via Wine's `Z:\` drive mapping)
3. `tts_helper.sh` reads from the pipe and calls `espeak-ng` with your configured voice settings
4. Audio plays through your Linux system's normal audio output (PulseAudio/PipeWire)

## Troubleshooting

**No sound:**
- Make sure `tts_helper.sh` is running: `ps aux | grep tts_helper`
- Check the pipe exists: `ls -la ~/.gw2-tts-pipe` (should show `p` prefix = named pipe)
- Test manually: `echo "hello" > ~/.gw2-tts-pipe`

**Log shows "Pipe open failed":**
- The addon logs the exact path it's trying. Verify it matches `~/.gw2-tts-pipe`
- Wine must have the `Z:\` drive mapped to `/` (default for Proton/Wine)

**Voice sounds wrong:**
- Try different voices with `espeak-ng --voices=en`
- Adjust `SPEED`, `PITCH`, and `AMPLITUDE` in `~/.gw2-tts-config`
