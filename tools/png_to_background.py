#!/usr/bin/env python3
"""Convert background.png → RGB565 background.bin for the user partition.

Output is a raw 320×240 framebuffer, 2 bytes/pixel (153 600 bytes), in the
same big-endian RGB565 byte order the ST7789 path uses (`rgb565_be` before
`esp_lcd_panel_draw_bitmap`). Layout matches `user_store`:
  user @ 0x340000 + 0x1000 → this blob.

Examples:
  python3 tools/png_to_background.py background.png
  python3 tools/png_to_background.py background.png -o build/background.bin --fit cover
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

WIDTH = 320
HEIGHT = 240
BG_BYTES = WIDTH * HEIGHT * 2


def rgb888_to_rgb565_be(r: int, g: int, b: int) -> bytes:
    """Pack one pixel as big-endian RGB565 (panel SPI order)."""
    c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes(((c >> 8) & 0xFF, c & 0xFF))


def pack_rgb888(raw: bytes) -> bytes:
    if len(raw) != WIDTH * HEIGHT * 3:
        raise ValueError(f"expected {WIDTH * HEIGHT * 3} RGB bytes, got {len(raw)}")
    out = bytearray(BG_BYTES)
    j = 0
    for i in range(0, len(raw), 3):
        out[j : j + 2] = rgb888_to_rgb565_be(raw[i], raw[i + 1], raw[i + 2])
        j += 2
    return bytes(out)


def load_rgb888_pillow(path: Path, fit: str) -> bytes:
    from PIL import Image

    im = Image.open(path).convert("RGBA")
    # Composite onto black so translucent PNGs don't keep a checkerboard
    bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
    im = Image.alpha_composite(bg, im).convert("RGB")

    if fit == "stretch":
        im = im.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    elif fit == "contain":
        im.thumbnail((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        canvas = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0))
        canvas.paste(im, ((WIDTH - im.width) // 2, (HEIGHT - im.height) // 2))
        im = canvas
    elif fit == "cover":
        scale = max(WIDTH / im.width, HEIGHT / im.height)
        nw, nh = max(1, int(round(im.width * scale))), max(1, int(round(im.height * scale)))
        im = im.resize((nw, nh), Image.Resampling.LANCZOS)
        left = (nw - WIDTH) // 2
        top = (nh - HEIGHT) // 2
        im = im.crop((left, top, left + WIDTH, top + HEIGHT))
    else:
        raise ValueError(fit)

    return im.tobytes()


def load_rgb888_imagemagick(path: Path, fit: str) -> bytes:
    magick = shutil.which("magick") or shutil.which("convert")
    if not magick:
        raise RuntimeError("neither Pillow nor ImageMagick (magick/convert) is available")

    # Build geometry for ImageMagick
    if fit == "stretch":
        ops = [f"{WIDTH}x{HEIGHT}!"]
        cmd_geom = ["-resize", ops[0]]
    elif fit == "contain":
        cmd_geom = [
            "-resize",
            f"{WIDTH}x{HEIGHT}",
            "-background",
            "black",
            "-gravity",
            "center",
            "-extent",
            f"{WIDTH}x{HEIGHT}",
        ]
    elif fit == "cover":
        cmd_geom = [
            "-resize",
            f"{WIDTH}x{HEIGHT}^",
            "-gravity",
            "center",
            "-extent",
            f"{WIDTH}x{HEIGHT}",
        ]
    else:
        raise ValueError(fit)

    with tempfile.NamedTemporaryFile(suffix=".rgb", delete=False) as tmp:
        raw_path = Path(tmp.name)

    try:
        cmd = [
            magick,
            str(path),
            "-alpha",
            "remove",
            "-alpha",
            "off",
            *cmd_geom,
            "-depth",
            "8",
            f"rgb:{raw_path}",
        ]
        subprocess.run(cmd, check=True, capture_output=True)
        return raw_path.read_bytes()
    except subprocess.CalledProcessError as e:
        err = (e.stderr or e.stdout or b"").decode("utf-8", errors="replace")
        raise RuntimeError(f"ImageMagick failed: {err}") from e
    finally:
        raw_path.unlink(missing_ok=True)


def convert(path: Path, fit: str) -> tuple[bytes, str]:
    try:
        raw = load_rgb888_pillow(path, fit)
        backend = "Pillow"
    except ImportError:
        raw = load_rgb888_imagemagick(path, fit)
        backend = "ImageMagick"
    blob = pack_rgb888(raw)
    assert len(blob) == BG_BYTES
    return blob, backend


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "png",
        nargs="?",
        type=Path,
        default=Path("background.png"),
        help="Input PNG (default: ./background.png)",
    )
    ap.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output path (default: build/background.bin)",
    )
    ap.add_argument(
        "--fit",
        choices=("cover", "contain", "stretch"),
        default="cover",
        help="How to map the PNG onto 320×240 (default: cover)",
    )
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    png = args.png if args.png.is_absolute() else (Path.cwd() / args.png)
    if not png.is_file():
        # also try repo root
        alt = root / args.png.name
        if alt.is_file():
            png = alt
        else:
            print(f"PNG not found: {args.png}", file=sys.stderr)
            return 1

    out = args.output or (root / "build" / "background.bin")
    try:
        blob, backend = convert(png, args.fit)
    except Exception as e:
        print(f"convert failed: {e}", file=sys.stderr)
        return 1

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    print(f"Wrote {out} ({len(blob)} bytes, 320x240 RGB565 BE) via {backend}")
    print(f"  source: {png}  fit={args.fit}")
    print("Flash with:")
    print(f"  python3 tools/flash_user.py --from-conf --bg {out}")
    print("  # or: python3 tools/flash_background.py", png.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
