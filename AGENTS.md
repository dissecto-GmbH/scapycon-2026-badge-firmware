# Scapycon 2026 Badge — Agent Instructions

Firmware for the **Scapycon 2026** conference badge. Theme: **V2X** (vehicle-to-everything) and **IEEE 802.11p** (DSRC/WAVE) — operating in the 5.9 GHz ITS band.

The production badge uses **ESP32-C5-WROOM-1-N4** (**4 MB flash, no PSRAM**). LCD init is attempted every boot; if the ST7789 is missing or init fails, the UI falls back to serial status logs (`badge_display_is_ready()`).

Espressif does not officially support 802.11p; the conference goal is to explore **hacky / experimental** use of the C5's 5 GHz radio for V2X-adjacent work (monitor mode, raw frames, out-of-band channels, etc.).

- **Agents:** use [`netlist.net`](netlist.net) for pins and nets (source of truth for `main/board.h`).
- **Humans:** use [`schematics.pdf`](schematics.pdf) for reading the PCB.

## Shared Ground Rules

- Target **ESP32-C5** only (`idf.py set-target esp32c5`).
- Use **ESP-IDF v6.0.2**. Activate the environment before any build or flash:
  ```bash
  source /home/enrico/.espressif/tools/activate_idf_v6.0.2.sh
  ```
- Prefer existing ESP-IDF components (`esp_lcd`, `led_strip`, `esp_wifi`, etc.) over reinventing drivers.
- Pin assignments come from the **netlist** — update `main/board.h` when nets change (do not infer pins from the PDF).
- Keep firmware memory-conscious; badge runs on battery.
- Do not commit `build/`, `sdkconfig`, or secrets.

## Bootstrap and Build

```bash
source /home/enrico/.espressif/tools/activate_idf_v6.0.2.sh
cd /home/enrico/Projects/badge2026-v2x
cp config/wifi.conf.example config/wifi.conf   # first time only; edit credentials
cp config/badge.conf.example config/badge.conf
idf.py set-target esp32c5    # once, or after sdkconfig wipe
idf.py build                 # main app → ota_0
cd flashloader && idf.py set-target esp32c5 && idf.py build && cd ..
python3 tools/flash_badge_dual.py   # bootloader + table + factory + ota_0
python3 tools/flash_user.py --from-conf   # seed name + Wi-Fi into user partition
python3 tools/png_to_background.py background.png  # → build/background.bin (320×240 RGB565 BE)
python3 tools/flash_background.py background.png   # convert + flash bg (keeps conf name/Wi-Fi)
python3 tools/build_firmware_release.py   # rebuild main; writes build/firmware.ver (YYYY.MM.DD.HH.MM.SS)
python3 tools/make_factory_image.py --from-conf -o build/factory_flash.bin  # monolithic first-flash / factory-reset image
# Hold SW_D at reset while on USB to run OTA check against munich.dissec.to
```

**Monolithic factory image** (`build/factory_flash.bin`): full 4 MiB flash with bootloader @ `0x2000`, partition table, flashloader, main app, and seeded `user` partition. Flash at offset 0:

```bash
esptool.py --chip esp32c5 -p /dev/ttyACM0 write_flash 0x0 build/factory_flash.bin
# Mass production (hold SW_BOOT + reset/plug each badge):
python3 -u tools/flash_factory_loop.py
```

Wi-Fi credentials: preferred in the **`user`** partition (`tools/flash_user.py`); `config/wifi.conf` remains a build-time fallback for the main app. Never commit passwords in source (`config/*.conf` is gitignored; examples may ship conference defaults).

Main app `PROJECT_VER` is **`YYYY.MM.DD.HH.MM.SS`** (local time, set at CMake configure; use `build_firmware_release.py` or `PROJECT_VER=… idf.py reconfigure build` so each release gets a fresh stamp). Upload that string as `firmware.ver` next to `firmware.bin`.

Current build profile: **4 MB flash**, **no PSRAM**, custom [`partitions.csv`](partitions.csv):
`factory` 1.25 MiB flashloader + `ota_0` 1.875 MiB main + `user` 768 KiB.

First full build takes several minutes. Subsequent builds are incremental.

## Project Layout

```
badge2026-v2x/
├── README.md              # human overview
├── AGENTS.md              # this file
├── partitions.csv         # N4: factory + ota_0 + user
├── flashloader/           # factory OTA helper (HTTPS update)
├── components/user_store/ # name / wifi / RGB565 bg in user partition
├── netlist.net            # for agents: pin/net source of truth
├── schematics.pdf         # for humans: PCB schematics PDF
├── config/
│   ├── wifi.conf.example
│   └── wifi.conf          # gitignored — SSID/password
├── tools/
│   ├── flash_badge.py     # build + flash main @ ota_0 + JTAG reset
│   ├── flash_badge_dual.py    # full flash: bootloader + flashloader + main
│   ├── make_factory_image.py  # build 4 MiB factory_flash.bin
│   ├── build_firmware_release.py  # OTA firmware.bin + firmware.ver
│   ├── ci_build_release.sh        # CI: flashloader + main + factory + OTA artifacts
│   ├── flash_factory_loop.py  # mass-flash factory image (esptool)
│   ├── badge_reset.py     # JTAG system reset only (SW_RESET equivalent)
│   ├── badge_openocd_run.cfg  # OpenOCD helper: esp32c5_app_run
│   ├── host_bridge.py     # USB → pcap capture on host PC
│   ├── png_to_background.py  # PNG → 320×240 RGB565 BE background.bin
│   ├── flash_background.py   # convert PNG + flash into user partition
│   └── flash_user.py      # seed name / Wi-Fi / optional RGB565 bg
├── background.png         # optional full-screen art (host-converted, not in firmware)
├── CMakeLists.txt
├── sdkconfig.defaults     # esp32c5 + USB console defaults
├── main/
│   ├── CMakeLists.txt
│   ├── board.h            # GPIO / peripheral pin map
│   ├── main.c             # app entry, LED animation, button hook
│   ├── radio.c / radio.h  # sniffer ↔ Wi-Fi station mode manager
│   ├── its_g5.c           # 802.11p promiscuous sniffer (5900 MHz)
│   ├── wifi.c             # station connect using wifi.conf credentials
│   ├── host_link.c        # USB 0xBEEF framing to host
│   ├── display.c / ui.c   # ST7789 SPI + status screen (or headless logs)
│   ├── leds.c             # 4× SK6805 NeoPixels
│   └── button.c           # SW1 debounce → mode toggle
└── build/                 # generated — do not commit
```

### Runtime architecture

| Module | Role |
|--------|------|
| `radio.c` | Owns Wi-Fi driver lifecycle; **SW1** toggles sniffer ↔ station |
| `its_g5.c` | Promiscuous RX on ITS-G5 G5CC (5900 MHz) via `phy_11p_set` / `phy_change_channel` |
| `host_link.c` | Optional USB export to PC; sniffer and packet count work **without** a host connected |
| `display.c` | ST7789 SPI init every boot; sets `badge_display_is_ready()` on success |
| `ui.c` | LCD status (mode, packet count, IP) or 5 s serial heartbeat when headless |

## Hardware Summary

| Subsystem | Part | Notes |
|-----------|------|-------|
| MCU | ESP32-C5-WROOM-1-N4 | 4 MB flash, no PSRAM, RISC-V single-core |
| Power | TP4056 + RT9013-33GB | Li-ion charge (≤1 A), 3.3 V LDO |
| Battery | PH2.0 2-pin (H1) | Single-cell Li-ion |
| USB | USB-C (USB1) | Charge + native USB (IO13/IO14) |
| Display | HS20HS072RX | 2.0" 320×240, **ST7789, SPI** (FPC2) |
| LEDs | 4× SK6805SIDE-G-003 | Addressable RGB, side-mount, WS2812-like |
| Buttons | SW1, SW3 | SW1 = user; SW3 = EN/reset |
| SAO | H5 2×3 female | SAO v1.69+ add-on connector |
| Debug UART | H3 | RX0 / TX0 |
| Expansion | H2, H3, H4 | 2.54 mm headers |

Charge/status LEDs: **LED1** (yellow, standby), **LED3** (green, charging) — driven by TP4056, not GPIO.

## Pin Map (`main/board.h`)

| Function | GPIO | Net name | Notes |
|----------|------|----------|-------|
| LCD RST | IO0 | LCD_RST | ST7789 reset |
| LCD MOSI | IO1 | LCD_SDA | SPI data |
| LCD BL | IO3 | LCD_BL | Backlight (N-MOSFET, active high) |
| LCD SCLK | IO6 | LCD_SCL | SPI clock |
| LCD DC | IO8 | LCD_RS | Data/command |
| LCD CS | IO9 | LCD_CS | Chip select |
| NeoPixel DIN | IO10 | — | 330 Ω series, 4-LED chain U5→U8 |
| USB D− | IO13 | USB_D− | Native USB |
| USB D+ | IO14 | USB_D+ | Native USB |
| SAO SDA | IO4 | SAO_SDA | I²C (SAO addon) |
| SAO SCL | IO5 | SAO_SCL | I²C (SAO addon) |
| SAO GPIO1 | IO24 | SAO_GPIO1 | |
| SAO GPIO2 | IO23 | SAO_GPIO2 | |
| Button SW1 | IO28 | — | Active low, 10 kΩ pull-up |
| UART TX | TX0 | — | H3 pin 4 |
| UART RX | RX0 | — | H3 pin 5 |

**Display bus:** SPI (not I²C). The SAO connector exposes I²C for add-on boards.

**NeoPixel chain order:** U5 → U6 → U7 → U8 (4 pixels).

## V2X / 802.11p Notes

- **802.11p** uses **5.9 GHz** (ITS band); channels differ from normal Wi-Fi 5 GHz.
- ESP32-C5 officially supports **2.4 & 5 GHz Wi-Fi 6**, not 802.11p/WAVE.
- Expect to use **undocumented or experimental** Wi-Fi APIs, monitor/promiscuous mode, or vendor-specific register tweaks — document findings in code comments and commit messages.
- Legal/regulatory: ITS-band transmission may be restricted by locale; badge firmware should default to **receive-only / lab-safe** modes unless explicitly configured otherwise.

### Minimal ITS-G5 sniffer (`main/its_g5.c`)

Uses `phy_11p_set()` + `phy_change_channel()` from `libphy.a` (OpenTrafficMap approach), promiscuous RX on **5900 MHz (G5CC)**. **Cannot run simultaneously with Wi-Fi station** on the same radio.

- **SW1 (GPIO28)** toggles sniffer ↔ Wi-Fi station (`badge_radio_toggle_mode()`).
- **USB binary framing** (`main/host_link.c`): outer magic `0xBEEF` + LE16 length (same as esp32-its-tap); inner badge payload types documented in `tools/host_bridge.py`.
- Host capture: `python3 tools/host_bridge.py /dev/ttyACM0 capture.pcap` → open in Wireshark (link type 802.11). Close `idf.py monitor` first — same port.
- **Packet counting** is local (`badge_its_g5_packet_count()`); USB only forwards copies to the host. Unplugged from a PC, the on-badge counter still increments when RF traffic is present.

Station Wi-Fi credentials remain in `config/wifi.conf`.

## Useful ESP-IDF Components

| Feature | Component / API |
|---------|-----------------|
| Display | `esp_lcd` + ST7789 panel driver |
| NeoPixels | `led_strip` (SK6805 ≈ WS2812 timing) |
| Wi-Fi / sniffing | `esp_wifi`, `esp_wifi_promiscuous` |
| USB host link | `esp_driver_usb_serial_jtag` (native USB on IO13/IO14) |
| Power | `esp_sleep`, brownout config in `esp_pm` |
| I²C (SAO) | `driver/i2c_master.h` |

## Autonomous flash and reset (no buttons)

Programming works without **SW_BOOT**. Rebooting into the app uses **OpenOCD over JTAG**
(same USB cable, JTAG interface — does **not** need exclusive access to `/dev/ttyACM0`).

| Step | Tool | What it does |
|------|------|--------------|
| 1 | OpenOCD JTAG | Flash bootloader + partition table + app (no ROM UART sync, no BOOT button). |
| 2 | `esp32c5_app_run` | System reset via LP_AON registers + release debug halt (`tools/badge_openocd_run.cfg`). Same effect as **SW_RESET**. |

**Why OpenOCD `reset run` is insufficient:** `program_esp_bins … reset exit` only clears JTAG
CPU halt (reset cause 24). The app never runs and the LCD stays black.

**Why esptool `HardReset` on ttyACM0 is a poor default:** toggling EN through the CDC serial
port requires exclusive access to `/dev/ttyACM0` (conflicts with `picocom` / monitor) and
was unreliable here. Use JTAG reset instead; esptool fallback: `python3 tools/badge_reset.py --esptool`.

**Recommended commands** (after `source …/activate_idf_v6.0.2.sh`):

```bash
python3 tools/flash_badge.py              # build + flash main @ ota_0 + JTAG reset
python3 tools/flash_badge.py --no-build   # flash existing build/badge2026_v2x.bin only
python3 tools/flash_badge_dual.py         # full: bootloader + table + flashloader + main
python3 tools/badge_reset.py              # JTAG reset only (reboot app)
python3 tools/badge_reset.py --esptool    # fallback via esptool watchdog reset
```

**Manual OpenOCD** (main app only, dual layout):

```bash
cd build && openocd -f board/esp32c5-builtin.cfg -f ../tools/badge_openocd_run.cfg \
  -c "program_esp badge2026_v2x.bin 0x160000 verify" \
  -c "esp32c5_app_run" \
  -c "shutdown"
```

Do **not** use `program_esp_bins` / `flasher_args.json` on this layout — that would write the main app at factory `0x20000`.

## Flashing and Debug

- USB serial/JTAG: native USB on IO13/IO14, or external UART on H3 (TX0/RX0).
- OpenOCD target: `esp32c5` (via ESP-IDF bundled OpenOCD).
- Agents: always end a flash with `tools/flash_badge.py` (includes JTAG reset) or run `tools/badge_reset.py` after manual OpenOCD flash.
- `idf.py flash` / esptool may time out when badge-mode firmware holds the USB port; OpenOCD JTAG flash does not need ROM sync.
- If flash fails, check **SW_RESET** (EN) and USB-C power; **SW_BOOT** is only for manual download mode.

### Mode buttons (netlist)

| Button | GPIO | Mode |
|--------|------|------|
| **SW_A** | 27 | Badge (default on boot) |
| **SW_B** | 26 | 802.11p V2X sniffer (press again → cycle ITS channel) |
| **SW_C** | 24 | Conference schedule (press again → Day 1 ↔ Day 2) |
| **SW_D** | 23 | Hold at reset + USB → flashloader OTA; in app, press → reboot to flashloader |
| **SW_BOOT** | 28 | ROM download (manual only) |
| **SW_RESET** | EN | Hardware reset |

Mode changes are queued to the `radio` task — do not call `esp_wifi_*` directly from the button task.

## When Changing Hardware

1. Re-export netlist to `netlist.net`.
2. Update `main/board.h` and this file's pin table.
3. Re-run `idf.py fullclean && idf.py build`.
