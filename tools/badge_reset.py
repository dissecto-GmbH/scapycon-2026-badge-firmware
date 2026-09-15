#!/usr/bin/env python3
"""Reset badge into flashed app (SW_RESET equivalent) via OpenOCD/JTAG."""

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


def openocd_app_run(root: Path) -> None:
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
        "init; esp32c5_app_run; shutdown",
    ]
    subprocess.run(cmd, check=True, cwd=root / "build")


def esptool_watchdog_reset(port: str) -> None:
    """Fallback when OpenOCD cannot attach; needs exclusive access to ttyACM0."""
    subprocess.run(
        [
            sys.executable,
            "-m",
            "esptool",
            "--chip",
            "esp32c5",
            "-p",
            port,
            "--before",
            "usb-reset",
            "--after",
            "watchdog-reset",
            "chip-id",
        ],
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Reset badge into app firmware (SW_RESET equivalent).",
    )
    parser.add_argument(
        "-p",
        "--port",
        default=os.environ.get("BADGE_PORT", "/dev/ttyACM0"),
        help="USB serial port for esptool fallback (default: /dev/ttyACM0)",
    )
    parser.add_argument(
        "--esptool",
        action="store_true",
        help="Use esptool watchdog reset instead of OpenOCD (requires exclusive serial port).",
    )
    args = parser.parse_args()
    root = project_root()

    if args.esptool:
        print(f"==> esptool watchdog reset on {args.port}")
        esptool_watchdog_reset(args.port)
    else:
        print("==> OpenOCD system reset (JTAG)")
        openocd_app_run(root)

    print("Reset complete — app should be running.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
