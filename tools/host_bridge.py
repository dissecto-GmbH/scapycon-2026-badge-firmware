#!/usr/bin/env python3
"""Read badge 802.11p frames from USB and write a libpcap file or stdout summary.

Wire format (outer frame, same magic as esp32-its-tap):
  0xBE 0xEF | uint16 LE payload_len | payload

Payload types:
  0x01 RX: uint8 type, uint64 ts_us, int8 rssi, uint8 channel, uint16 len, data[len]
  0x02 HB: uint8 type, uint32 uptime_s, uint32 rx_count, uint8 mode
  0x03 MODE: uint8 type, uint8 mode (0=sniffer, 1=schedule, 2=badge)

Usage:
  # Prefer the by-id path; close picocom first.
  python3 tools/host_bridge.py /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<SERIAL>-if00
  python3 tools/host_bridge.py /dev/ttyACM0 captures.pcap
  python3 tools/host_bridge.py /dev/ttyACM0   # summary only

Requires pyserial (`pip install pyserial`). Close picocom / idf.py monitor first.
If the badge reboots into badge mode on open, the port open asserted DTR — this
tool clears HUPCL/DTR/RTS to avoid that (same idea as picocom --noreset).
After open it sends CTRL+B 2 to select sniffer (1=badge 2=sniffer 3=schedule).
"""

from __future__ import annotations

import array
import fcntl
import os
import struct
import subprocess
import sys
import termios
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    raise SystemExit(1)

MAGIC = b"\xbe\xef"
LINKTYPE_IEEE802_11 = 105
RECONNECT_DELAY_S = 1.0

# Linux modem-bit ioctls (clear DTR/RTS without a pulse-high-first open).
_TIOCMBIC = getattr(termios, "TIOCMBIC", 0x5417)
_TIOCM_DTR = 0x002
_TIOCM_RTS = 0x004


def _clear_modem_lines(fd: int) -> None:
    """Drop DTR/RTS and disable HUPCL so ACM open does not reset ESP USB-JTAG."""
    try:
        attrs = termios.tcgetattr(fd)
        attrs[2] &= ~termios.HUPCL  # c_cflag
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    except termios.error:
        pass
    try:
        buf = array.array("i", [_TIOCM_DTR | _TIOCM_RTS])
        fcntl.ioctl(fd, _TIOCMBIC, buf)
    except OSError:
        pass


def open_port(path: str) -> serial.Serial:
    """Open badge USB CDC without resetting into badge mode.

    pyserial's default open can assert DTR/RTS; on ESP32-C5 USB-Serial/JTAG that
    causes rst:0x15 and a reboot back to default badge mode. Picocom with
    --noreset/--noinit avoids this; we mirror that with stty -hupcl + modem clear.
    """
    resolved = str(Path(path).resolve())
    subprocess.run(
        ["stty", "-F", resolved, "-hupcl", "clocal", "-crtscts"],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # Open at the OS level first so we can clear modem lines before pyserial
    # finishes configuring the port.
    fd = os.open(resolved, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        _clear_modem_lines(fd)
    finally:
        os.close(fd)

    ser = serial.Serial()
    ser.port = resolved
    ser.baudrate = 115200
    ser.timeout = 1.0
    ser.write_timeout = 1.0
    ser.dsrdtr = False
    ser.rtscts = False
    ser.xonxoff = False
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    try:
        _clear_modem_lines(ser.fileno())
    except (OSError, termios.error, serial.SerialException):
        pass
    return ser


def request_sniffer_mode(ser: serial.Serial) -> None:
    """After a possible DTR reboot (badge mode), ask firmware for sniffer via CTRL+B 3."""
    time.sleep(1.5)
    ser.write(bytes([0x02, ord("2")]))  # CTRL+B, then 2 (= SW_B / sniffer)
    ser.flush()
    print("Sent CTRL+B 3 (request sniffer mode)")


def read_exact(ser: serial.Serial, n: int) -> bytes | None:
    buf = b""
    while len(buf) < n:
        if not ser.is_open:
            return None
        chunk = ser.read(n - len(buf))
        if chunk:
            buf += chunk
    return buf


def sync_magic(ser: serial.Serial) -> bool:
    """Scan byte stream until 0xBE 0xEF (skips interleaved ESP log text)."""
    pending = 0
    while ser.is_open:
        b = ser.read(1)
        if not b:
            continue
        if pending == 0:
            if b == b"\xbe":
                pending = 1
        elif b == b"\xef":
            return True
        elif b == b"\xbe":
            pending = 1
        else:
            pending = 0
    return False


def pcap_writer(path: str):
    f = open(path, "wb")
    f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, LINKTYPE_IEEE802_11))
    count = 0

    def write_packet(data: bytes, ts_us: int) -> None:
        nonlocal count
        sec = ts_us // 1_000_000
        usec = ts_us % 1_000_000
        f.write(struct.pack("<IIII", sec, usec, len(data), len(data)))
        f.write(data)
        count += 1

    def close() -> None:
        f.close()
        print(f"Wrote {count} frames to {path}")

    return write_packet, close


def handle_payload(payload: bytes, write_pcap) -> None:
    if not payload:
        return

    msg_type = payload[0]
    if msg_type == 0x01 and len(payload) >= 13:
        (ts_us,) = struct.unpack("<Q", payload[1:9])
        rssi = struct.unpack("<b", payload[9:10])[0]
        channel = payload[10]
        (flen,) = struct.unpack("<H", payload[11:13])
        frame = payload[13 : 13 + flen]
        if write_pcap:
            write_pcap(frame, ts_us)
        else:
            print(f"RX ts={ts_us} rssi={rssi} ch={channel} len={flen}")
    elif msg_type == 0x02 and len(payload) >= 10:
        uptime, rx_count, mode = struct.unpack("<IIB", payload[1:10])
        mode_s = {0: "sniffer", 1: "schedule", 2: "badge"}.get(mode, f"mode{mode}")
        print(f"HB uptime={uptime}s rx={rx_count} mode={mode_s}")
    elif msg_type == 0x03 and len(payload) >= 2:
        mode_s = {0: "sniffer", 1: "schedule", 2: "badge"}.get(payload[1], f"mode{payload[1]}")
        print(f"MODE → {mode_s}")


def run_session(ser: serial.Serial, write_pcap) -> None:
    while True:
        if not sync_magic(ser):
            return

        hdr = read_exact(ser, 2)
        if hdr is None:
            return

        (plen,) = struct.unpack("<H", hdr)
        if plen == 0 or plen > 4096:
            continue

        payload = read_exact(ser, plen)
        if payload is None:
            return

        handle_payload(payload, write_pcap)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    port = sys.argv[1]
    pcap_path = sys.argv[2] if len(sys.argv) > 2 else None
    write_pcap, close_pcap = pcap_writer(pcap_path) if pcap_path else (None, None)

    print(f"Opening {port} — binary host link (Ctrl+C to stop)")
    print("(no DTR reset; ESP log lines on the same port are skipped)")
    print("Leave the badge in sniffer mode (SW_B). Close picocom first.")

    try:
        while True:
            try:
                ser = open_port(port)
            except serial.SerialException as exc:
                print(f"Cannot open {port}: {exc}", file=sys.stderr)
                print("Close picocom / idf.py monitor or other programs using the port.",
                      file=sys.stderr)
                return 1

            try:
                request_sniffer_mode(ser)
                run_session(ser, write_pcap)
            except serial.SerialException as exc:
                print(f"Serial error: {exc}", file=sys.stderr)
            finally:
                # Avoid DTR pulse on close when possible
                try:
                    ser.dtr = False
                    ser.rts = False
                except Exception:
                    pass
                ser.close()

            print(f"Port closed — retrying in {RECONNECT_DELAY_S:.0f}s…")
            print("(If the badge rebooted, press SW_B again before the retry.)")
            time.sleep(RECONNECT_DELAY_S)
    finally:
        if close_pcap:
            close_pcap()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nStopped.")
        raise SystemExit(0)
