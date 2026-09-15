#!/usr/bin/env python3
"""Build a monolithic 4 MiB factory flash image for first-flash / factory reset.

Layout (matches partitions.csv + C5 bootloader @ 0x2000):

  0x2000    bootloader
  0x8000    partition table
  0xf000    otadata (blank 0xFF → boot factory/flashloader)
  0x20000   flashloader (factory app)
  0x160000  main app (ota_0)
  0x340000  user (name / Wi-Fi / optional RGB565 bg)

Example:
  python3 tools/make_factory_image.py --from-conf -o build/factory_flash.bin
  esptool.py -p /dev/ttyACM0 write_flash 0x0 build/factory_flash.bin
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

FLASH_SIZE = 0x400000  # 4 MiB N4

BOOTLOADER_OFF = 0x2000
PART_TABLE_OFF = 0x8000
OTADATA_OFF = 0xF000
OTADATA_SIZE = 0x2000
FACTORY_OFF = 0x20000
OTA0_OFF = 0x160000
USER_OFF = 0x340000
USER_SIZE = 0xC0000

USER_MAGIC = 0x42444755
USER_LAYOUT = 1
NAME_MAX = 24
SSID_MAX = 32
PASS_MAX = 64
HDR_SIZE = 0x1000
BG_OFFSET = 0x1000
BG_BYTES = 320 * 240 * 2
FLAG_BG = 1 << 0


def root() -> Path:
    return Path(__file__).resolve().parent.parent


def parse_conf(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def fnv_header(blob: bytes) -> int:
    crc_off = 4 + 2 + 2 + NAME_MAX + SSID_MAX + PASS_MAX
    s = 2166136261
    for b in blob[:crc_off]:
        s ^= b
        s = (s * 16777619) & 0xFFFFFFFF
    return s


def pack_user(name: str, ssid: str, password: str, bg: bytes | None) -> bytes:
    flags = FLAG_BG if bg else 0
    name_b = name.encode("utf-8")[: NAME_MAX - 1].ljust(NAME_MAX, b"\0")
    ssid_b = ssid.encode("utf-8")[: SSID_MAX - 1].ljust(SSID_MAX, b"\0")
    pass_b = password.encode("utf-8")[: PASS_MAX - 1].ljust(PASS_MAX, b"\0")
    body = struct.pack("<IHH", USER_MAGIC, USER_LAYOUT, flags) + name_b + ssid_b + pass_b
    body += struct.pack("<I", fnv_header(body + b"\0\0\0\0"))
    hdr = body.ljust(HDR_SIZE, b"\0")
    if bg is not None:
        if len(bg) != BG_BYTES:
            raise SystemExit(f"bg must be {BG_BYTES} bytes, got {len(bg)}")
        blob = hdr + bg
    else:
        blob = hdr
    return blob.ljust(USER_SIZE, b"\xff")


def place(image: bytearray, offset: int, data: bytes, label: str) -> None:
    end = offset + len(data)
    if end > len(image):
        raise SystemExit(f"{label}: offset 0x{offset:x}+{len(data)} exceeds flash size")
    image[offset:end] = data
    print(f"  0x{offset:06x}  {label:16s}  {len(data):8d} bytes")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", type=Path, default=None, help="Output path (default: build/factory_flash.bin)")
    ap.add_argument("--from-conf", action="store_true", help="Seed user name/Wi-Fi from config/*.conf")
    ap.add_argument("--name", default="")
    ap.add_argument("--ssid", default="")
    ap.add_argument("--password", default="")
    ap.add_argument("--bg", type=Path, help="Optional RGB565 background (153600 bytes)")
    ap.add_argument(
        "--png",
        type=Path,
        help="PNG to convert into RGB565 bg (via tools/png_to_background.py)",
    )
    ap.add_argument("--no-user", action="store_true", help="Leave user partition erased (0xFF)")
    args = ap.parse_args()

    r = root()
    out = args.output or (r / "build" / "factory_flash.bin")

    bootloader = r / "build" / "bootloader" / "bootloader.bin"
    part_table = r / "build" / "partition_table" / "partition-table.bin"
    otadata = r / "build" / "ota_data_initial.bin"
    flashloader = r / "flashloader" / "build" / "badge2026_flashloader.bin"
    app = r / "build" / "badge2026_v2x.bin"

    missing = [p for p in (bootloader, part_table, flashloader, app) if not p.is_file()]
    if missing:
        print("Missing binaries (build main + flashloader first):", file=sys.stderr)
        for p in missing:
            print(f"  {p}", file=sys.stderr)
        return 1

    image = bytearray(b"\xff" * FLASH_SIZE)
    print(f"Assembling {FLASH_SIZE // (1024 * 1024)} MiB factory image:")

    place(image, BOOTLOADER_OFF, bootloader.read_bytes(), "bootloader")
    place(image, PART_TABLE_OFF, part_table.read_bytes(), "partition-table")
    if otadata.is_file():
        place(image, OTADATA_OFF, otadata.read_bytes()[:OTADATA_SIZE], "otadata")
    else:
        print(f"  0x{OTADATA_OFF:06x}  {'otadata':16s}  (erased 0xFF — boots factory)")

    place(image, FACTORY_OFF, flashloader.read_bytes(), "flashloader")
    place(image, OTA0_OFF, app.read_bytes(), "main/ota_0")

    if not args.no_user:
        name, ssid, password = args.name, args.ssid, args.password
        if args.from_conf:
            wifi = parse_conf(r / "config" / "wifi.conf")
            badge = parse_conf(r / "config" / "badge.conf")
            ssid = ssid or wifi.get("ssid", "")
            password = password or wifi.get("password", "")
            name = name or badge.get("name", "")

        bg_path = args.bg
        if args.png:
            import subprocess

            bg_out = r / "build" / "background.bin"
            subprocess.run(
                [
                    sys.executable,
                    str(r / "tools" / "png_to_background.py"),
                    str(args.png),
                    "-o",
                    str(bg_out),
                ],
                check=True,
                cwd=r,
            )
            bg_path = bg_out
        if bg_path is None:
            # Prefer build/background.bin, else convert repo-root background.png
            cand = r / "build" / "background.bin"
            png = r / "background.png"
            if cand.is_file() and cand.stat().st_size == BG_BYTES:
                bg_path = cand
            elif png.is_file():
                import subprocess

                cand.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run(
                    [
                        sys.executable,
                        str(r / "tools" / "png_to_background.py"),
                        str(png),
                        "-o",
                        str(cand),
                    ],
                    check=True,
                    cwd=r,
                )
                bg_path = cand

        bg = bg_path.read_bytes() if bg_path else None
        user = pack_user(name, ssid, password, bg)
        place(image, USER_OFF, user, "user")
        if bg:
            print(f"           user background: {bg_path} ({len(bg)} bytes)")
        if ssid:
            print(f"           user Wi-Fi SSID={ssid!r} name={name!r}")
        else:
            print("           user header present (empty Wi-Fi — set later)")
    else:
        print(f"  0x{USER_OFF:06x}  {'user':16s}  (erased 0xFF)")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    print(f"Wrote {out} ({len(image)} bytes)")
    print("Flash with:")
    print(f"  esptool.py --chip esp32c5 -p PORT write_flash 0x0 {out}")
    print("  # or OpenOCD: program_esp <file> 0x0 verify")
    return 0


if __name__ == "__main__":
    sys.exit(main())
