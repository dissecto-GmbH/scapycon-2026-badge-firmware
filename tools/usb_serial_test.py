#!/usr/bin/env python3
"""
Verify badge-mode USB serial TX and RX on /dev/ttyACM0.

Checks that the setup-console menu appears (TX) and that option 1 + a test name
are accepted (RX) with a "Saved name:" response (TX again). Use --loop to retry
until both directions work or --max-attempts is exhausted.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    raise SystemExit("Install pyserial: pip install pyserial")

BANNER = b"Scapycon 2026 Badge"
MENU = b"Enter 1-3:"
SAVED = b"Saved name:"
TEST_NAME = "usbloop"


def project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def idf_shell_prefix() -> str:
    if os.environ.get("IDF_PATH"):
        return ""
    activate = Path.home() / ".espressif/tools/activate_idf_v6.0.2.sh"
    if activate.is_file():
        return f"source {activate} && "
    return ""


def run_reset(root: Path, port: str, use_esptool: bool) -> None:
    prefix = idf_shell_prefix()
    if use_esptool:
        cmd = f"{prefix}python3 {root / 'tools' / 'badge_reset.py'} --esptool -p {port}"
    else:
        cmd = f"{prefix}python3 {root / 'tools' / 'badge_reset.py'}"
    subprocess.run(["bash", "-lc", cmd], check=False, cwd=root)


def wait_port(port: str, timeout_s: float) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if Path(port).exists():
            return True
        time.sleep(0.2)
    return False


def wait_for_prompt(ser: serial.Serial, io_timeout_s: float) -> tuple[bool, bytearray]:
    collected = bytearray()
    deadline = time.time() + io_timeout_s
    while time.time() < deadline:
        ser.write(b"\n")
        time.sleep(0.25)
        chunk = ser.read(4096)
        if chunk:
            collected.extend(chunk)
        if BANNER in collected or MENU in collected or b"Enter 1-3:" in collected:
            return True, collected
    return False, collected


def serial_roundtrip(port: str, settle_s: float, io_timeout_s: float) -> tuple[bool, str, bytes]:
    if not wait_port(port, timeout_s=10):
        return False, f"port {port} not found", b""

    try:
        ser = serial.Serial(port, 115200, timeout=0.25, write_timeout=2.0)
    except serial.SerialException as exc:
        return False, f"open failed: {exc}", b""

    collected = bytearray()
    try:
        ser.dtr = True
        ser.rts = False
        ser.reset_input_buffer()
        time.sleep(settle_s)

        def drain() -> None:
            chunk = ser.read(4096)
            if chunk:
                collected.extend(chunk)

        deadline = time.time() + io_timeout_s
        while time.time() < deadline:
            try:
                ser.write(b"\n")
            except serial.SerialTimeoutException:
                return False, "host write timeout on wake", bytes(collected)
            time.sleep(0.25)
            drain()
            if BANNER in collected or MENU in collected:
                break

        if BANNER not in collected and MENU not in collected:
            return False, "TX fail: no banner/menu", bytes(collected)

        try:
            ser.write(b"1\n")
            time.sleep(0.3)
            drain()
            ser.write(f"{TEST_NAME}\n".encode())
        except serial.SerialTimeoutException:
            return False, "host write timeout on name", bytes(collected)

        reply_deadline = time.time() + io_timeout_s
        while time.time() < reply_deadline:
            drain()
            if SAVED in collected and TEST_NAME.encode() in collected:
                return True, "TX+RX ok", bytes(collected)
            time.sleep(0.05)

        if SAVED in collected:
            return True, "TX+RX ok (saved)", bytes(collected)
        if TEST_NAME.encode() in collected:
            return False, "RX ok but no save confirmation", bytes(collected)
        return False, "RX fail: no response after name", bytes(collected)
    finally:
        ser.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify badge USB serial TX and RX.")
    parser.add_argument(
        "-p",
        "--port",
        default=os.environ.get("BADGE_PORT", "/dev/ttyACM0"),
        help="USB serial device (default: /dev/ttyACM0)",
    )
    parser.add_argument(
        "--settle",
        type=float,
        default=2.0,
        help="Seconds to wait after opening port before testing",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=10.0,
        help="Seconds to wait for banner and save response",
    )
    parser.add_argument(
        "--loop",
        action="store_true",
        help="Retry until success or --max-attempts",
    )
    parser.add_argument(
        "--max-attempts",
        type=int,
        default=8,
        help="Attempts when --loop is set (default: 8)",
    )
    parser.add_argument(
        "--reset-between",
        action="store_true",
        help="From attempt 2 onward, reset before each try",
    )
    parser.add_argument(
        "--esptool-reset",
        action="store_true",
        help="Use esptool watchdog reset (needs exclusive port) instead of JTAG",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Only print final result",
    )
    parser.add_argument(
        "--reopen",
        action="store_true",
        help="Close and reopen the port to test late host attach",
    )
    return parser.parse_args()


def serial_reopen_test(port: str, settle_s: float, io_timeout_s: float) -> tuple[bool, str]:
    if not wait_port(port, timeout_s=10):
        return False, f"port {port} not found"

    try:
        ser = serial.Serial(port, 115200, timeout=0.25, write_timeout=2.0)
    except serial.SerialException as exc:
        return False, f"open failed: {exc}"

    try:
        ser.dtr = True
        ser.rts = False
        ser.reset_input_buffer()
        time.sleep(settle_s)

        ok1, buf1 = wait_for_prompt(ser, io_timeout_s)
        if not ok1:
            return False, "first open: no prompt"

        ser.close()
        time.sleep(1.5)

        ser = serial.Serial(port, 115200, timeout=0.25, write_timeout=2.0)
        ser.dtr = True
        ser.rts = False
        ser.reset_input_buffer()
        time.sleep(settle_s)

        ok2, buf2 = wait_for_prompt(ser, io_timeout_s)
        if not ok2:
            return False, "reopen: no prompt (late attach broken)"

        ser.write(b"1\n")
        time.sleep(0.3)
        buf2.extend(ser.read(4096))
        ser.write(f"{TEST_NAME}\n".encode())
        time.sleep(1.5)
        buf2.extend(ser.read(4096))
        if SAVED not in buf2:
            return False, "reopen: no save confirmation"
        return True, "reopen attach ok"
    except serial.SerialTimeoutException as exc:
        return False, f"write timeout: {exc}"
    finally:
        try:
            ser.close()
        except Exception:
            pass


def main() -> int:
    args = parse_args()
    root = project_root()
    attempts = args.max_attempts if args.loop else 1

    for attempt in range(1, attempts + 1):
        if not args.quiet:
            print(f"==> attempt {attempt}/{attempts}", flush=True)

        if args.loop and attempt > 1 and args.reset_between:
            # JTAG reset often leaves CDC serial dead; alternate esptool after attempt 2.
            use_esptool = args.esptool_reset or (attempt >= 3 and attempt % 2 == 1)
            kind = "esptool" if use_esptool else "JTAG"
            if not args.quiet:
                print(f"    reset ({kind})", flush=True)
            run_reset(root, args.port, use_esptool)
            time.sleep(5.0 if not use_esptool else 2.0)

        if args.reopen:
            ok, msg = serial_reopen_test(args.port, args.settle, args.timeout)
            data = b""
        else:
            ok, msg, data = serial_roundtrip(args.port, args.settle, args.timeout)
        if ok:
            if not args.quiet:
                preview = data.decode("utf-8", errors="replace")
                if len(preview) > 400:
                    preview = preview[:400] + "..."
                print(f"PASS ({msg}):\n{preview}")
            else:
                print("PASS")
            return 0

        if not args.quiet:
            print(f"    FAIL: {msg}", flush=True)
            if data:
                preview = data.decode("utf-8", errors="replace")
                if preview.strip():
                    print(f"    captured: {preview[:200]!r}")

        if not args.loop:
            break
        time.sleep(1.0)

    print("FAIL: USB serial check did not pass", file=sys.stderr)
    print(
        "Hint: quit picocom, press SW_RESET on the badge, then re-run this script.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
