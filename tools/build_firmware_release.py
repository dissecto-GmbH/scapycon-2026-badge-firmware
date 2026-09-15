#!/usr/bin/env python3
"""Build main app with a second-resolution PROJECT_VER and emit firmware.ver for upload."""

from __future__ import annotations

import argparse
import datetime as dt
import os
import subprocess
import sys
from pathlib import Path


def root() -> Path:
    return Path(__file__).resolve().parent.parent


def stamp() -> str:
    return dt.datetime.now().strftime("%Y.%m.%d.%H.%M.%S")


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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--ver",
        default="",
        help="Override version (default: local time YYYY.MM.DD.HH.MM.SS)",
    )
    ap.add_argument("--no-build", action="store_true", help="Only write firmware.ver using --ver")
    args = ap.parse_args()

    ver = args.ver or stamp()
    r = root()
    out_dir = r / "build"
    out_dir.mkdir(parents=True, exist_ok=True)
    ver_path = out_dir / "firmware.ver"
    bin_path = out_dir / "badge2026_v2x.bin"

    if not args.no_build:
        env = os.environ.copy()
        env["PROJECT_VER"] = ver
        print(f"PROJECT_VER={ver}")
        subprocess.run(
            idf_command(env, "reconfigure", "build"),
            cwd=r,
            env=env,
            check=True,
        )

    ver_path.write_text(ver + "\n", encoding="utf-8")
    print(f"Wrote {ver_path}")
    if bin_path.is_file():
        print(f"Upload: {bin_path} as firmware.bin")
        print(f"Upload: {ver_path} as firmware.ver  (contents: {ver})")
    else:
        print(f"Warning: {bin_path} missing — build first", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
