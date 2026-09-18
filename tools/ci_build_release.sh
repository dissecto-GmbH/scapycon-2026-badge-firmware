#!/usr/bin/env bash
# CI / local helper: build flashloader + main, emit OTA + factory release artifacts.
#
# Env:
#   PROJECT_VER — optional; default UTC YYYY.MM.DD.HH.MM.SS
#   IDF_PATH    — required (script will source export.sh if needed)
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

# Child shells do not inherit idf.py shell functions from a parent export.sh.
# Always load the env here, then invoke idf.py via the IDF Python + tools script.
if [[ -f "${IDF_PATH}/export.sh" ]]; then
  # shellcheck disable=SC1091
  . "${IDF_PATH}/export.sh"
fi

idf() {
  local py="${IDF_PYTHON_ENV_PATH:-}/bin/python"
  if [[ ! -x "$py" ]]; then
    py="$(command -v python3)"
  fi
  "$py" "${IDF_PATH}/tools/idf.py" "$@"
}

VER="${PROJECT_VER:-$(date -u +%Y.%m.%d.%H.%M.%S)}"
export PROJECT_VER="$VER"
echo "PROJECT_VER=$PROJECT_VER"
echo "IDF_PATH=$IDF_PATH"
idf --version || true

# Seed gitignored conf from examples (factory image --from-conf).
mkdir -p config build
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
idf set-target esp32c5
idf build
popd >/dev/null

echo "==> main app (ota_0) PROJECT_VER=$PROJECT_VER"
idf set-target esp32c5
idf reconfigure build

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
