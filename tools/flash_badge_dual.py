#!/usr/bin/env python3
"""Build flashloader + main, flash both via OpenOCD, JTAG reset into factory."""

from __future__ import annotations

import argparse
import datetime as dt
import os
import subprocess
import sys
from pathlib import Path

OPENOCD_BOARD_CFG = "board/esp32c5-builtin.cfg"
OPENOCD_RUN_CFG = "tools/badge_openocd_run.cfg"


def root() -> Path:
    return Path(__file__).resolve().parent.parent


def run(cmd: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(cmd), f"(cwd={cwd})")
    subprocess.run(cmd, cwd=cwd, check=True, env=env)


def build_all(do_build: bool) -> tuple[Path, Path, Path, Path, Path, Path]:
    r = root()
    main_build = r / "build"
    fl_build = r / "flashloader" / "build"

    if do_build:
        run(["idf.py", "fullclean"], r / "flashloader")
        run(["idf.py", "set-target", "esp32c5"], r / "flashloader")
        run(["idf.py", "build"], r / "flashloader")
        ver = dt.datetime.now().strftime("%Y.%m.%d.%H.%M.%S")
        env = os.environ.copy()
        env["PROJECT_VER"] = ver
        print(f"Main PROJECT_VER={ver}")
        run(["idf.py", "fullclean"], r, env=env)
        run(["idf.py", "set-target", "esp32c5"], r, env=env)
        run(["idf.py", "reconfigure", "build"], r, env=env)
        (main_build / "firmware.ver").write_text(ver + "\n", encoding="utf-8")
        print(f"Wrote {main_build / 'firmware.ver'}")

    bootloader = main_build / "bootloader" / "bootloader.bin"
    part = main_build / "partition_table" / "partition-table.bin"
    otadata = main_build / "ota_data_initial.bin"
    phy = main_build / "phy_init_data.bin"
    factory = fl_build / "badge2026_flashloader.bin"
    ota0 = main_build / "badge2026_v2x.bin"

    for p in (bootloader, part, otadata, phy, factory, ota0):
        if not p.is_file():
            raise SystemExit(f"Missing {p}; build both projects first.")

    return bootloader, part, otadata, phy, factory, ota0


def erase_otadata(port: str) -> None:
    """Erase otadata so next boot uses factory (flashloader) partition."""
    subprocess.run(
        [
            sys.executable,
            "-m",
            "esptool",
            "--chip",
            "esp32c5",
            "-p",
            port,
            "erase_region",
            "0xf000",
            "0x2000",
        ],
        check=True,
    )


def flash(
    bootloader: Path,
    part: Path,
    otadata: Path,
    phy: Path,
    factory: Path,
    ota0: Path,
) -> None:
    r = root()
    run_cfg = r / OPENOCD_RUN_CFG
    # Offsets match build/flasher_args.json (bootloader @ 0x2000, not 0x0).
    cmd = [
        "openocd",
        "-f",
        OPENOCD_BOARD_CFG,
        "-f",
        str(run_cfg),
        "-c",
        f"program_esp {bootloader} 0x2000 verify",
        "-c",
        f"program_esp {part} 0x8000 verify",
        "-c",
        f"program_esp {otadata} 0xf000 verify force",
        "-c",
        f"program_esp {phy} 0x11000 verify force",
        "-c",
        f"program_esp {factory} 0x20000 verify",
        "-c",
        f"program_esp {ota0} 0x160000 verify",
        "-c",
        "esp32c5_app_run",
        "-c",
        "shutdown",
    ]
    subprocess.run(cmd, check=True, cwd=r / "build")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument(
        "-p",
        "--port",
        default=os.environ.get("BADGE_PORT", "/dev/ttyACM0"),
        help="USB serial port for otadata erase (default: /dev/ttyACM0)",
    )
    ap.add_argument(
        "--skip-otadata-erase",
        action="store_true",
        help="Do not erase otadata before flash (default: erase 0xf000/0x2000)",
    )
    args = ap.parse_args()
    bootloader, part, otadata, phy, factory, ota0 = build_all(not args.no_build)
    if not args.skip_otadata_erase:
        print("==> erase otadata 0xf000 (boot factory flashloader)")
        erase_otadata(args.port)
    flash(bootloader, part, otadata, phy, factory, ota0)
    print(
        "Flashed bootloader@0x2000 + table + otadata + phy + "
        "flashloader(factory) + main(ota_0)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
