#!/usr/bin/env python3
"""
Build (optional), flash ESP32-C5 badge via OpenOCD, then reset into the app.

OpenOCD ``reset run`` leaves the core JTAG-halted; ``esp32c5_app_run`` (see
tools/badge_openocd_run.cfg) triggers a full system reset over JTAG — the same
effect as pressing SW_RESET, without needing the serial port.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

OPENOCD_BOARD_CFG = "board/esp32c5-builtin.cfg"
OPENOCD_RUN_CFG = "tools/badge_openocd_run.cfg"


def project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def run_idf_build(root: Path) -> None:
    subprocess.run(["idf.py", "build"], cwd=root, check=True)


def run_openocd_flash_and_run(root: Path, build_dir: Path) -> None:
    flasher_args = build_dir / "flasher_args.json"
    if not flasher_args.is_file():
        raise SystemExit(f"Missing {flasher_args}; run idf.py build first.")

    run_cfg = root / OPENOCD_RUN_CFG
    if not run_cfg.is_file():
        raise SystemExit(f"Missing {run_cfg}")

    cmd = [
        "openocd",
        "-f",
        OPENOCD_BOARD_CFG,
        "-f",
        str(run_cfg),
        "-c",
        f"program_esp_bins {build_dir} flasher_args.json verify",
        "-c",
        "esp32c5_app_run",
        "-c",
        "shutdown",
    ]
    subprocess.run(cmd, check=True, cwd=build_dir)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Flash badge firmware via OpenOCD and JTAG system reset.",
    )
    parser.add_argument(
        "-p",
        "--port",
        default=os.environ.get("BADGE_PORT", "/dev/ttyACM0"),
        help="USB serial port (esptool fallback only)",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="Skip idf.py build (flash existing build/ artifacts).",
    )
    parser.add_argument(
        "--reset-only",
        action="store_true",
        help="Only reset into app; do not flash.",
    )
    parser.add_argument(
        "--verify-usb",
        action="store_true",
        help="After flash/reset, loop usb_serial_test.py until TX+RX pass.",
    )
    parser.add_argument(
        "--verify-attempts",
        type=int,
        default=8,
        help="Max attempts for --verify-usb (default: 8)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = project_root()
    build_dir = root / "build"

    parts = root / "partitions.csv"
    if parts.is_file() and "ota_0" in parts.read_text() and "factory" in parts.read_text():
        print(
            "ERROR: partitions.csv has factory+ota_0. "
            "Use tools/flash_badge_dual.py or tools/restore_dual_flash.sh — "
            "this script would flash the main app at factory (0x20000) and brick boot.",
            file=sys.stderr,
        )
        return 2

    if args.reset_only:
        print("==> OpenOCD system reset (JTAG)", flush=True)
        subprocess.run(
            [
                "openocd",
                "-f",
                OPENOCD_BOARD_CFG,
                "-f",
                str(root / OPENOCD_RUN_CFG),
                "-c",
                "init; esp32c5_app_run; shutdown",
            ],
            check=True,
            cwd=build_dir,
        )
    else:
        if not args.no_build:
            print("==> idf.py build", flush=True)
            run_idf_build(root)

        print("==> OpenOCD flash + system reset", flush=True)
        run_openocd_flash_and_run(root, build_dir)

    print("Done — firmware should be running.")

    if args.verify_usb:
        test = root / "tools" / "usb_serial_test.py"
        print("==> USB serial verify (loop until pass)", flush=True)
        rc = subprocess.call(
            [
                sys.executable,
                str(test),
                "--loop",
                "--reset-between",
                "--max-attempts",
                str(args.verify_attempts),
                "--settle",
                "3",
                "-p",
                args.port,
            ],
            cwd=root,
        )
        if rc != 0:
            raise SystemExit(rc)

    return 0


if __name__ == "__main__":
    sys.exit(main())
