<p align="center">
  <a href="./README.md">
    <img alt="English" src="https://img.shields.io/badge/English-111111?style=for-the-badge" />
  </a>
  <a href="./README.zh-CN.md">
    <img alt="简体中文" src="https://img.shields.io/badge/简体中文-EAEAEA?style=for-the-badge&labelColor=EAEAEA&color=111111" />
  </a>
</p>

<p align="center">
  <img src="screenshots/cover.png" alt="Code Buddy cover" width="100%" />
</p>

<h1 align="center">Code Buddy</h1>

<p align="center">
  A StickS3 Codex companion adapted from
  <a href="https://github.com/anthropics/claude-desktop-buddy">Claude Desktop Buddy</a>.
</p>

<p align="center">
  Flash the device once, run <code>code-buddy</code> once on macOS, then keep using <code>codex</code> normally while approvals and live session status move to dedicated hardware.
</p>

> Building your own hardware client? See [firmware/REFERENCE.md](firmware/REFERENCE.md) for the BLE protocol and JSON payloads.

## What ships

- A macOS bridge that pairs with the StickS3, syncs time, installs the native BLE helper, and manages the local `codex` shim.
- A StickS3 firmware build with status, approval, settings, and offline screens.
- A daily workflow designed to stay out of the way: run `code-buddy` once, then just use `codex`.

## Quick start

### 1. Flash the StickS3

Download `code-buddy-sticks3-v{version}-full.bin` from GitHub Releases and flash it at `0x0`.

Preferred path:

- If a release includes a web flasher, use it and write the merged image at `0x0`.

Fallback:

```bash
esptool --chip esp32s3 --port /dev/cu.usbmodem101 --baud 460800 write_flash 0x0 code-buddy-sticks3-v0.1.1-full.bin
```

Developer release build:

```bash
./scripts/build-firmware-release.sh
```

### 2. Install on macOS

```bash
brew install CharlexH/tap/code-buddy
code-buddy
```

On first run, Code Buddy will:

- install the native Bluetooth helper
- pair with a `Codex-*` device
- sync device time
- install the launchd agent
- install the local `codex` shim
- add `~/.code-buddy/bin` to `~/.zprofile`

### 3. Use it normally

```bash
codex
```

Open a new shell after setup. From there, Code Buddy keeps the bridge alive and shows approval prompts on the StickS3 while you keep your normal CLI flow.

## Controls

|                         | Normal               | Pet         | Info        | Approval    |
| ----------------------- | -------------------- | ----------- | ----------- | ----------- |
| **A** (front)           | next screen          | next screen | next screen | **approve** |
| **B** (right)           | scroll transcript    | next page   | next page   | **deny**    |
| **Hold A**              | menu                 | menu        | menu        | menu        |
| **Power** (left, short) | toggle screen off    |             |             |             |
| **Power** (left, ~6s)   | hard power off       |             |             |             |
| **Shake**               | dizzy                |             |             | —           |
| **Face-down**           | nap (energy refills) |             |             |             |

The screen auto-powers off after 30 seconds of inactivity and stays on while an approval prompt is pending. Any button press wakes it.

## Buddy states

| State       | Trigger                     | Feel                        |
| ----------- | --------------------------- | --------------------------- |
| `sleep`     | bridge not connected        | eyes closed, slow breathing |
| `idle`      | connected, nothing urgent   | blinking, looking around    |
| `busy`      | sessions actively running   | sweating, working           |
| `attention` | approval pending            | alert, **LED blinks**       |
| `celebrate` | level up (every 50K tokens) | confetti, bouncing          |
| `dizzy`     | you shook the stick         | spiral eyes, wobbling       |
| `heart`     | approved in under 5s        | floating hearts             |

<details>
<summary><strong>Characters and custom packs</strong></summary>

The firmware ships with eighteen ASCII pets. Each one includes seven animations: `sleep`, `idle`, `busy`, `attention`, `celebrate`, `dizzy`, and `heart`.

Use `menu -> next pet` on the device to cycle through them. The selection is saved in device storage.

If you want a custom GIF character, create a pack with a `manifest.json` and 96px-wide GIFs for the same seven states:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

Notes:

- `idle` can be a single GIF or an array of GIFs.
- Heights up to about 140px fit well on the StickS3 screen.
- See [firmware/characters/bufo/](firmware/characters/bufo/) for a working example.
- Use [firmware/tools/prep_character.py](firmware/tools/prep_character.py) and [firmware/tools/flash_character.py](firmware/tools/flash_character.py) to prepare and flash assets.
</details>

## Recovery

```bash
code-buddy doctor
code-buddy repair
code-buddy uninstall
```

`doctor` explains what is wrong, why it happened, and what to do next.

<details>
<summary><strong>Build from source</strong></summary>

```bash
python3 -m venv .venv
.venv/bin/pip install -e '.[dev]'
.venv/bin/code-buddy
```

Verification:

- Host tests: `.venv/bin/pytest -q`
- Firmware build: `cd firmware && pio run`
</details>
