#!/usr/bin/env bash
# One-shot diagnose + dual flash recovery for Scapycon badge.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${BADGE_PORT:-/dev/ttyACM0}"
LOG="$ROOT/tmp_diag/recover_$(date +%Y%m%d_%H%M%S).txt"

mkdir -p "$ROOT/tmp_diag"
exec > >(tee -a "$LOG") 2>&1

source /home/enrico/.espressif/tools/activate_idf_v6.0.2.sh
cd "$ROOT"

echo "=== USB ==="
ls -la "$PORT" || true
lsusb | grep -iE '303a|espressif' || true

echo "=== Reset + serial 5s ==="
python3 tools/badge_reset.py || true
stty -F "$PORT" 115200 raw -echo 2>/dev/null || true
timeout 5 cat "$PORT" || true
echo "(serial capture end)"

echo "=== esptool flash_id + headers ==="
python3 -m esptool --chip esp32c5 -p "$PORT" flash_id || true
for off in 0x2000 0x8000 0x20000 0x160000 0xf000; do
  echo "--- read $off ---"
  python3 -m esptool --chip esp32c5 -p "$PORT" read_flash "$off" 16 "/tmp/badge_${off}.bin" 2>/dev/null && xxd "/tmp/badge_${off}.bin" || true
done

echo "=== sdkconfig FLASHSIZE ==="
grep -h FLASHSIZE "$ROOT/sdkconfig.defaults" "$ROOT/flashloader/sdkconfig.defaults" 2>/dev/null || true
for d in build flashloader/build; do
  if [[ -f "$ROOT/$d/sdkconfig" ]]; then
    echo "--- $d/sdkconfig ---"
    grep FLASHSIZE "$ROOT/$d/sdkconfig" || true
  fi
done

echo "=== Build + dual flash ==="
python3 tools/flash_badge_dual.py

echo "=== Post-flash reset + serial ==="
python3 tools/badge_reset.py || true
stty -F "$PORT" 115200 raw -echo 2>/dev/null || true
timeout 5 cat "$PORT" || true

echo "=== Done — log: $LOG ==="
