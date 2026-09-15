# Scapycon 2026 Badge Firmware

Firmware for the **Scapycon 2026** conference badge (ESP32-C5), themed around
**V2X** / **IEEE 802.11p** in the 5.9 GHz ITS band.

## Authorship

**This firmware was written entirely by AI** (Cursor’s coding agent / Composer).
Treat the code as machine-authored: review carefully
before trusting it on hardware or shipping changes.

Agent-oriented build/flash notes live in [`AGENTS.md`](AGENTS.md).

## Hardware docs

| Audience | File |
|----------|------|
| Agents / pin map | [`netlist.net`](netlist.net) |
| Humans / schematic reading | [`schematics.pdf`](schematics.pdf) |

Production module: **ESP32-C5-WROOM-1-N4** (4 MB flash, no PSRAM).

## What you get

- Main badge app (`main/`) — badge UI, 802.11p sniffer, conference schedule, USB host link
- Factory flashloader (`flashloader/`) — OTA helper partition
- User partition store (`components/user_store/`) — name, Wi‑Fi, RGB565 background
- Host tools under `tools/` — factory image, OTA release stamp, mass flash, OpenOCD helpers

## Quick start

Requires **ESP-IDF v6.0.2**:

```bash
source /home/enrico/.espressif/tools/activate_idf_v6.0.2.sh   # or your IDF env
cp config/wifi.conf.example config/wifi.conf
cp config/badge.conf.example config/badge.conf
idf.py set-target esp32c5
idf.py build
cd flashloader && idf.py set-target esp32c5 && idf.py build && cd ..
python3 tools/make_factory_image.py --from-conf -o build/factory_flash.bin
```

Flash a full factory image (badge in download mode: hold **SW_BOOT**, reset/plug):

```bash
esptool.py --chip esp32c5 -p /dev/ttyACM0 write_flash 0x0 build/factory_flash.bin
# or mass-flash: python3 -u tools/flash_factory_loop.py
```

OTA artifacts: `python3 tools/build_firmware_release.py` → upload `build/firmware.bin`
and `build/firmware.ver`.

See [`AGENTS.md`](AGENTS.md) for partitions, buttons, and OpenOCD details.

## Legal / RF

Espressif does not officially support 802.11p. ITS-band transmission may be
restricted by locale; default firmware behaviour is receive-oriented / lab-safe
unless you deliberately enable TX.
