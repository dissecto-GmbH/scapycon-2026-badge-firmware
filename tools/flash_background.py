#!/usr/bin/env python3
"""Convert background.png → RGB565 and flash it into the user partition.

Preserves name / Wi-Fi from config/*.conf (same as flash_user.py --from-conf).

  python3 tools/flash_background.py
  python3 tools/flash_background.py background.png -p /dev/ttyACM0
  python3 tools/flash_background.py --fit contain --dry-run
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "png",
        nargs="?",
        type=Path,
        default=Path("background.png"),
        help="Input PNG (default: ./background.png)",
    )
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("-o", "--output", type=Path, default=None, help="Where to write background.bin")
    ap.add_argument(
        "--fit",
        choices=("cover", "contain", "stretch"),
        default="cover",
        help="PNG fit mode (default: cover)",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="Only convert; do not flash",
    )
    ap.add_argument(
        "--name",
        default="",
        help="Override badge name (else config/badge.conf via --from-conf)",
    )
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    out = args.output or (root / "build" / "background.bin")

    conv = [
        sys.executable,
        str(root / "tools" / "png_to_background.py"),
        str(args.png),
        "-o",
        str(out),
        "--fit",
        args.fit,
    ]
    print("+", " ".join(conv))
    subprocess.run(conv, check=True, cwd=root)

    if args.dry_run:
        print("Dry run — skipped flash")
        return 0

    flash = [
        sys.executable,
        str(root / "tools" / "flash_user.py"),
        "-p",
        args.port,
        "--from-conf",
        "--bg",
        str(out),
    ]
    if args.name:
        flash.extend(["--name", args.name])
    print("+", " ".join(flash))
    subprocess.run(flash, check=True, cwd=root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
