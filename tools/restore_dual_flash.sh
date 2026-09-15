#!/bin/bash
# Restore dual-app layout after flash_badge.py wrongly put main @ factory.
set -euo pipefail
ROOT=/home/enrico/Projects/badge2026-v2x
source /home/enrico/.espressif/tools/activate_idf_v6.0.2.sh
export OPENOCD_SCRIPTS="${OPENOCD_SCRIPTS:-$(dirname "$(dirname "$(command -v openocd)")")/share/openocd/scripts}"
# Prefer Espressif OpenOCD scripts path if present
for d in /home/enrico/.espressif/tools/openocd-esp32/*/openocd-esp32/share/openocd/scripts; do
  if [[ -d "$d" ]]; then export OPENOCD_SCRIPTS="$d"; break; fi
done

cd "$ROOT/flashloader"
idf.py build

cd "$ROOT"
# Ensure main exists (do not reflash via flash_badge.py — wrong offset)
test -f build/badge2026_v2x.bin
test -f build/bootloader/bootloader.bin
test -f build/partition_table/partition-table.bin
test -f flashloader/build/badge2026_flashloader.bin

echo "==> Erase otadata"
esptool.py --chip esp32c5 -p /dev/ttyACM0 erase_region 0xf000 0x2000 || true

echo "==> OpenOCD flash correct offsets (bootloader @ 0x2000 per C5 flasher_args)"
openocd -f "$OPENOCD_SCRIPTS/board/esp32c5-builtin.cfg" \
  -f "$ROOT/tools/badge_openocd_run.cfg" \
  -c "program_esp $ROOT/build/bootloader/bootloader.bin 0x2000 verify" \
  -c "program_esp $ROOT/build/partition_table/partition-table.bin 0x8000 verify" \
  -c "program_esp $ROOT/flashloader/build/badge2026_flashloader.bin 0x20000 verify" \
  -c "program_esp $ROOT/build/badge2026_v2x.bin 0x160000 verify" \
  -c "esp32c5_app_run" \
  -c "shutdown"

echo "RESTORE_OK — factory=flashloader @0x20000, main @0x160000"
echo "Do NOT use tools/flash_badge.py with this partition table (it puts main at factory)."
