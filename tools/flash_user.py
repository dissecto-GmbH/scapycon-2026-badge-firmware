#!/usr/bin/env python3
"""Write Wi-Fi / name / RGB565 background into the user partition (0x340000)."""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

USER_MAGIC = 0x42444755
USER_LAYOUT = 1
USER_OFFSET = 0x340000
NAME_MAX = 24
SSID_MAX = 32
PASS_MAX = 64
HDR_SIZE = 0x1000
BG_OFFSET = 0x1000
BG_BYTES = 320 * 240 * 2
FLAG_BG = 1 << 0


def fnv_header(blob: bytes) -> int:
    """Match firmware FNV-1a over bytes before crc32 field."""
    # struct: magic u32, layout u16, flags u16, name[24], ssid[32], pass[64], crc u32
    crc_off = 4 + 2 + 2 + NAME_MAX + SSID_MAX + PASS_MAX
    s = 2166136261
    for b in blob[:crc_off]:
        s ^= b
        s = (s * 16777619) & 0xFFFFFFFF
    return s


def pack_header(name: str, ssid: str, password: str, flags: int = 0) -> bytes:
    name_b = name.encode("utf-8")[: NAME_MAX - 1].ljust(NAME_MAX, b"\0")
    ssid_b = ssid.encode("utf-8")[: SSID_MAX - 1].ljust(SSID_MAX, b"\0")
    pass_b = password.encode("utf-8")[: PASS_MAX - 1].ljust(PASS_MAX, b"\0")
    body = struct.pack("<IHH", USER_MAGIC, USER_LAYOUT, flags) + name_b + ssid_b + pass_b
    crc = fnv_header(body + b"\0\0\0\0")
    body += struct.pack("<I", crc)
    return body.ljust(HDR_SIZE, b"\0")


def parse_conf(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("--name", default="")
    ap.add_argument("--ssid", default="")
    ap.add_argument("--password", default="")
    ap.add_argument("--from-conf", action="store_true", help="Load name/ssid/pass from config/*.conf")
    ap.add_argument("--bg", type=Path, help="RGB565 bin (153600 bytes)")
    ap.add_argument(
        "--png",
        type=Path,
        help="PNG to convert via tools/png_to_background.py (writes build/background.bin)",
    )
    ap.add_argument("--dry-run", type=Path, help="Write image to file instead of flashing")
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    name, ssid, password = args.name, args.ssid, args.password
    if args.from_conf:
        wifi = parse_conf(root / "config" / "wifi.conf")
        badge = parse_conf(root / "config" / "badge.conf")
        ssid = ssid or wifi.get("ssid", "")
        password = password or wifi.get("password", "")
        name = name or badge.get("name", "")

    if args.png:
        bg_out = root / "build" / "background.bin"
        subprocess.run(
            [
                sys.executable,
                str(root / "tools" / "png_to_background.py"),
                str(args.png),
                "-o",
                str(bg_out),
            ],
            check=True,
            cwd=root,
        )
        args.bg = bg_out

    flags = 0
    bg = b""
    if args.bg:
        bg = args.bg.read_bytes()
        if len(bg) != BG_BYTES:
            raise SystemExit(f"bg must be {BG_BYTES} bytes, got {len(bg)}")
        flags |= FLAG_BG

    hdr = pack_header(name, ssid, password, flags)
    image = hdr + bg
    if args.bg:
        # pad not required; esptool writes exact length
        pass

    if args.dry_run:
        args.dry_run.write_bytes(image)
        print(f"Wrote {args.dry_run} ({len(image)} bytes)")
        return 0

    tmp = root / "build" / "user_partition.bin"
    tmp.parent.mkdir(parents=True, exist_ok=True)
    tmp.write_bytes(image)

    cmd = [
        "esptool.py",
        "-p",
        args.port,
        "-b",
        "460800",
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "write_flash",
        f"0x{USER_OFFSET:x}",
        str(tmp),
    ]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)
    print(f"Flashed user partition at 0x{USER_OFFSET:x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
