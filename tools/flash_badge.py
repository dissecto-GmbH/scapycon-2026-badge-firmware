#!/usr/bin/env python3
"""
Build (optional), flash main app via OpenOCD, then JTAG reset into the app.

On the dual layout (factory=flashloader, ota_0=main), programs only the main
binary at the ota_0 offset. Use tools/flash_badge_dual.py for a full first-time
or recovery flash (bootloader + table + flashloader + main).

OpenOCD ``reset run`` leaves the core JTAG-halted; ``esp32c5_app_run`` (see
tools/badge_openocd_run.cfg) triggers a full system reset over JTAG — the same
effect as pressing SW_RESET, without needing the serial port.
"""

from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
from pathlib import Path

OPENOCD_BOARD_CFG = "board/esp32c5-builtin.cfg"
OPENOCD_RUN_CFG = "tools/badge_openocd_run.cfg"


def project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def idf_command(env: dict[str, str], *args: str) -> list[str]:
    """Invoke idf.py without relying on shell functions from activate_idf."""
    idf_path = env.get("IDF_PATH", "")
    idf_venv = env.get("IDF_PYTHON_ENV_PATH", "")
    if idf_path and idf_venv:
        py = Path(idf_venv) / "bin" / "python"
        script = Path(idf_path) / "tools" / "idf.py"
        if py.is_file() and script.is_file():
            return [str(py), str(script), *args]
    return ["idf.py", *args]


def run_idf_build(root: Path) -> None:
    env = os.environ.copy()
    subprocess.run(idf_command(env, "build"), cwd=root, check=True, env=env)


def parse_app_partitions(parts_csv: Path) -> dict[str, int]:
    """Return {name: offset} for app partitions in partitions.csv."""
    out: dict[str, int] = {}
    if not parts_csv.is_file():
        return out
    with parts_csv.open(newline="", encoding="utf-8") as f:
        for row in csv.reader(f):
            if not row or row[0].startswith("#"):
                continue
            if len(row) < 4:
                continue
            name = row[0].strip()
            ptype = row[1].strip().lower()
            if ptype != "app":
                continue
            try:
                out[name] = int(row[3].strip(), 0)
            except ValueError:
                continue
    return out


def main_app_bin(build_dir: Path) -> Path:
    """Resolve the main application .bin (project name may vary)."""
    preferred = build_dir / "badge2026_v2x.bin"
    if preferred.is_file():
        return preferred
    bins = sorted(
        p
        for p in build_dir.glob("*.bin")
        if p.name
        not in {
            "ota_data_initial.bin",
            "phy_init_data.bin",
            "background.bin",
            "user_partition.bin",
            "factory_flash.bin",
            "firmware.bin",
        }
    )
    if len(bins) == 1:
        return bins[0]
    raise SystemExit(
        f"Missing main app binary in {build_dir} "
        "(expected badge2026_v2x.bin); run idf.py build first."
    )


def run_openocd(root: Path, build_dir: Path, program_cmds: list[str]) -> None:
    run_cfg = root / OPENOCD_RUN_CFG
    if not run_cfg.is_file():
        raise SystemExit(f"Missing {run_cfg}")

    cmd = [
        "openocd",
        "-f",
        OPENOCD_BOARD_CFG,
        "-f",
        str(run_cfg),
        *program_cmds,
        "-c",
        "esp32c5_app_run",
        "-c",
        "shutdown",
    ]
    subprocess.run(cmd, check=True, cwd=build_dir)


def flash_dual_ota0(root: Path, build_dir: Path, ota0_off: int) -> None:
    app = main_app_bin(build_dir)
    print(f"==> dual layout: flash {app.name} @ 0x{ota0_off:x} (ota_0)", flush=True)
    run_openocd(
        root,
        build_dir,
        ["-c", f"program_esp {app.resolve()} 0x{ota0_off:x} verify"],
    )


def flash_via_flasher_args(root: Path, build_dir: Path) -> None:
    flasher_args = build_dir / "flasher_args.json"
    if not flasher_args.is_file():
        raise SystemExit(f"Missing {flasher_args}; run idf.py build first.")

    print("==> single-app layout: program_esp_bins flasher_args.json", flush=True)
    run_openocd(
        root,
        build_dir,
        ["-c", f"program_esp_bins {build_dir} flasher_args.json verify"],
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Flash main badge firmware via OpenOCD and JTAG system reset.",
    )
    parser.add_argument(
        "-p",
        "--port",
        default=os.environ.get("BADGE_PORT", "/dev/ttyACM0"),
        help="USB serial port (esptool fallback / USB verify only)",
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
    apps = parse_app_partitions(root / "partitions.csv")
    dual = "factory" in apps and "ota_0" in apps

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
        if dual:
            flash_dual_ota0(root, build_dir, apps["ota_0"])
        else:
            flash_via_flasher_args(root, build_dir)

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
