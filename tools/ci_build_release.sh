#!/usr/bin/env bash
# CI / local helper: build flashloader + main, emit OTA + factory release artifacts.
#
# Env:
#   PROJECT_VER   — optional; default UTC YYYY.MM.DD.HH.MM.SS
#   IDF_PATH      — required (source export.sh first in CI)
#
# Outputs under build/release/:
#   firmware.bin, firmware.ver, factory_flash.bin, SHA256SUMS
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH is not set. Source ESP-IDF export.sh first." >&2
  exit 1
fi

VER="${PROJECT_VER:-$(date -u +%Y.%m.%d.%H.%M.%S)}"
export PROJECT_VER="$VER"
echo "PROJECT_VER=$PROJECT_VER"

# Seed gitignored conf from examples (factory image --from-conf).
mkdir -p config
if [[ ! -f config/wifi.conf ]]; then
  cp config/wifi.conf.example config/wifi.conf
fi
if [[ ! -f config/badge.conf ]]; then
  cp config/badge.conf.example config/badge.conf
fi

# Background.bin for factory user partition (Pillow preferred; ImageMagick fallback).
python3 -m pip install --quiet 'pillow>=10' || true
if [[ -f background.png ]]; then
  python3 tools/png_to_background.py background.png -o build/background.bin
fi

echo "==> flashloader (factory)"
pushd flashloader >/dev/null
idf.py set-target esp32c5
idf.py build
popd >/dev/null

echo "==> main app (ota_0) PROJECT_VER=$PROJECT_VER"
idf.py set-target esp32c5
idf.py reconfigure build

test -f build/badge2026_v2x.bin
test -f flashloader/build/badge2026_flashloader.bin

echo "==> OTA artifacts"
mkdir -p build/release
cp -f build/badge2026_v2x.bin build/release/firmware.bin
printf '%s\n' "$PROJECT_VER" > build/release/firmware.ver
cp -f build/release/firmware.ver build/firmware.ver
cp -f build/release/firmware.bin build/firmware.bin

echo "==> factory image"
python3 tools/make_factory_image.py --from-conf -o build/release/factory_flash.bin
cp -f build/release/factory_flash.bin build/factory_flash.bin

(
  cd build/release
  sha256sum firmware.bin firmware.ver factory_flash.bin > SHA256SUMS
)

echo "==> release artifacts"
ls -lh build/release/
cat build/release/firmware.ver
cat build/release/SHA256SUMS
