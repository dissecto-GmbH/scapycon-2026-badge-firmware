# Flashloader — factory OTA helper for Scapycon 2026 badge (ESP32-C5 N4)

Hold **SW_D** at reset **and** power from **USB** to check for updates.
Otherwise boots the main app (`ota_0`) immediately.

## Update URLs

- Version: `https://munich.dissec.to/scapycon2026/firmware.ver` (single line `YYYY.MM.DD.HH.MM.SS`)
- Image: `https://munich.dissec.to/scapycon2026/firmware.bin` (ESP-IDF app image for `ota_0`)

Update runs when remote `.ver` ≠ version string embedded in the flashed `ota_0` image.

Build main + emit upload files:

```bash
python3 tools/build_firmware_release.py
# → build/badge2026_v2x.bin  and  build/firmware.ver
```

Wi-Fi SSID/password are read from the **`user`** partition (see `components/user_store`).

## Build

```bash
source /home/enrico/.espressif/tools/activate_idf_v6.0.2.sh
cd flashloader
idf.py set-target esp32c5
idf.py build
```

Artifact: `flashloader/build/badge2026_flashloader.bin` (flash to `factory` at `0x20000`, size budget **1.25 MiB**).

Main firmware is built from the repo root into `ota_0` (`0x160000`).
