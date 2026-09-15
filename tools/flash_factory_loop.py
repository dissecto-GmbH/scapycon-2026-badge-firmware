#!/usr/bin/env python3
"""Mass-flash badges with the monolithic factory image via esptool.

For each badge: hold SW_BOOT, press SW_RESET (or plug while holding BOOT) so the
chip is in ROM download mode, then wait for the script to finish. Unplug and
repeat. Ctrl+C to stop.

  python3 tools/flash_factory_loop.py
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

ESP_VID = "303a"


def root() -> Path:
    return Path(__file__).resolve().parent.parent


def espressif_usb_present() -> bool:
    try:
        out = subprocess.check_output(["lsusb"], text=True, stderr=subprocess.DEVNULL)
        if ESP_VID in out.lower():
            return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    return Path("/dev/ttyACM0").exists() or Path("/dev/ttyUSB0").exists()


def find_port() -> str | None:
    for p in ("/dev/ttyACM0", "/dev/ttyUSB0"):
        if Path(p).exists():
            return p
    return None


def wait_until(want_present: bool, label: str, poll_s: float = 0.25) -> None:
    state = "in download mode (BOOT held + RESET)" if want_present else "gone (unplug)"
    print(f"\n>>> Waiting for badge {state}… ({label})", flush=True)
    while espressif_usb_present() != want_present:
        time.sleep(poll_s)
    if want_present:
        # USB settle + tty node
        for _ in range(40):
            if find_port():
                break
            time.sleep(0.1)
        time.sleep(0.5)
    print(f">>> Badge {'connected' if want_present else 'disconnected'}.", flush=True)


def esptool_bin() -> list[str]:
    if shutil.which("esptool.py"):
        return ["esptool.py"]
    return [sys.executable, "-m", "esptool"]


def flash_factory(image: Path, port: str, baud: int) -> None:
    # Already in ROM download mode — no reset before/after so a USB
    # re-enumerate or early unplug after verify is not treated as failure.
    cmd = [
        *esptool_bin(),
        "--chip",
        "esp32c5",
        "-p",
        port,
        "-b",
        str(baud),
        "--before",
        "no-reset",
        "--after",
        "no-reset",
        "write-flash",
        "0x0",
        str(image),
    ]
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def bell() -> None:
    sys.stdout.write("\a")
    sys.stdout.flush()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", type=Path, default=None)
    ap.add_argument("-b", "--baud", type=int, default=921600)
    ap.add_argument("--start-count", type=int, default=0)
    args = ap.parse_args()

    r = root()
    image = (args.image or (r / "build" / "factory_flash.bin")).resolve()
    if not image.is_file():
        print(f"Missing {image}", file=sys.stderr)
        return 1

    count = args.start_count
    print("=" * 60, flush=True)
    print("Factory mass-flash loop (esptool / download mode)", flush=True)
    print(f"  image: {image} ({image.stat().st_size} bytes)", flush=True)
    print("  Per badge: hold SW_BOOT, reset/plug → flash → unplug → next.", flush=True)
    print("  Ctrl+C to stop.", flush=True)
    print("=" * 60, flush=True)

    try:
        while True:
            if not espressif_usb_present():
                wait_until(True, "plug next badge")
            else:
                print("\n>>> Badge already connected — ensure download mode (BOOT+RESET).", flush=True)
                time.sleep(0.3)

            port = find_port()
            if not port:
                print("!!! No /dev/ttyACM0 — unplug and retry.", flush=True)
                wait_until(False, "unplug after missing port")
                continue

            t0 = time.monotonic()
            try:
                flash_factory(image, port, args.baud)
            except subprocess.CalledProcessError as e:
                print(f"!!! Flash FAILED (exit {e.returncode}) — reseat, BOOT+RESET, unplug.", flush=True)
                bell()
                wait_until(False, "unplug after failure")
                continue

            elapsed = time.monotonic() - t0
            count += 1
            bell()
            print(f"\n*** OK #{count}  ({elapsed:.1f}s)  — unplug this badge ***", flush=True)
            wait_until(False, "unplug")
    except KeyboardInterrupt:
        print(f"\nStopped. Successfully flashed: {count}", flush=True)
        return 0


if __name__ == "__main__":
    sys.exit(main())
