#!/bin/zsh
set -euo pipefail
setopt null_glob

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIRMWARE_DIR="$ROOT/firmware"
BUILD_DIR="$FIRMWARE_DIR/.pio/build/m5stack-sticks3"
DIST_DIR="$ROOT/dist/firmware"
PACKAGE_FIRMWARE_DIR="$ROOT/src/codex_buddy/firmware"

VERSION="${1:-$(
  python3 - <<'PY' "$ROOT/pyproject.toml"
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(r'^version = "([^"]+)"$', text, re.MULTILINE)
if match is None:
    raise SystemExit("Unable to read version from pyproject.toml")
print(match.group(1))
PY
)}"

if ! command -v pio >/dev/null 2>&1; then
  echo "PlatformIO (`pio`) is required to build the release firmware." >&2
  exit 1
fi

PIO_BIN="$(command -v pio)"
PIO_PYTHON="$(head -1 "$PIO_BIN" | sed 's/^#!//')"
if [[ ! -x "$PIO_PYTHON" ]]; then
  PIO_PYTHON="python3"
fi

mkdir -p "$DIST_DIR"

(
  cd "$FIRMWARE_DIR"
  pio run
)

BOOT_APP0=""
for candidate in \
  "$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin" \
  $HOME/.platformio/packages/framework-arduinoespressif32@*/tools/partitions/boot_app0.bin
do
  if [[ -f "$candidate" ]]; then
    BOOT_APP0="$candidate"
    break
  fi
done

if [[ -z "$BOOT_APP0" ]]; then
  echo "Unable to locate boot_app0.bin in PlatformIO packages." >&2
  exit 1
fi

ESPTOOL_COMMAND=()
if command -v esptool >/dev/null 2>&1; then
  ESPTOOL_COMMAND=(esptool)
fi

if [[ ${#ESPTOOL_COMMAND[@]} -eq 0 ]]; then
  for candidate in \
    "$HOME/.platformio/packages/tool-esptoolpy/esptool" \
    $HOME/.platformio/packages/tool-esptoolpy@*/esptool
  do
    if [[ -f "$candidate" && -x "$candidate" ]]; then
      ESPTOOL_COMMAND=("$candidate")
      break
    fi
  done
fi

if [[ ${#ESPTOOL_COMMAND[@]} -eq 0 ]]; then
  for candidate in \
    "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" \
    $HOME/.platformio/packages/tool-esptoolpy@*/esptool.py
  do
    if [[ -f "$candidate" ]]; then
      ESPTOOL_COMMAND=("$PIO_PYTHON" "$candidate")
      break
    fi
  done
fi

if [[ ${#ESPTOOL_COMMAND[@]} -eq 0 ]]; then
  echo "Unable to locate esptool. Install PlatformIO packages first." >&2
  exit 1
fi

OUTPUT="$DIST_DIR/code-buddy-sticks3-v${VERSION}-full.bin"
APP_OUTPUT="$DIST_DIR/code-buddy-sticks3-v${VERSION}-app.bin"
DEFAULT_APP_OUTPUT="$DIST_DIR/code-buddy-sticks3-app.bin"
PACKAGE_APP_OUTPUT="$PACKAGE_FIRMWARE_DIR/code-buddy-sticks3-app.bin"

for artifact in \
  "$BUILD_DIR/bootloader.bin" \
  "$BUILD_DIR/partitions.bin" \
  "$BUILD_DIR/firmware.bin"
do
  if [[ ! -f "$artifact" ]]; then
    echo "Missing firmware artifact: $artifact" >&2
    exit 1
  fi
done

cp "$BUILD_DIR/firmware.bin" "$APP_OUTPUT"
cp "$BUILD_DIR/firmware.bin" "$DEFAULT_APP_OUTPUT"
PYTHONPATH="$ROOT/src" "$PIO_PYTHON" - <<'PY' "$APP_OUTPUT" "$VERSION"
import sys
from pathlib import Path
from codex_buddy.ota_release import inspect_esp32s3_application_image

image = inspect_esp32s3_application_image(Path(sys.argv[1]))
if image.version != sys.argv[2]:
    raise SystemExit(
        "Embedded firmware version %s does not match release version %s"
        % (image.version, sys.argv[2])
    )
PY

"${ESPTOOL_COMMAND[@]}" --chip esp32s3 merge_bin -o "$OUTPUT" \
  0x0000 "$BUILD_DIR/bootloader.bin" \
  0x8000 "$BUILD_DIR/partitions.bin" \
  0xe000 "$BOOT_APP0" \
  0x10000 "$BUILD_DIR/firmware.bin"

mkdir -p "$PACKAGE_FIRMWARE_DIR"
PACKAGE_TEMP="$PACKAGE_APP_OUTPUT.tmp.$$"
trap 'rm -f "$PACKAGE_TEMP"' EXIT
cp "$APP_OUTPUT" "$PACKAGE_TEMP"
chmod 0644 "$PACKAGE_TEMP"
mv -f "$PACKAGE_TEMP" "$PACKAGE_APP_OUTPUT"
trap - EXIT

echo "$OUTPUT"
echo "$APP_OUTPUT"
