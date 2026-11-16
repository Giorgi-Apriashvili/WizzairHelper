"""Regenerate the WizzairHelper application icon from its two SVG sources (DECISIONS D-50).

    python -I tools/make_app_icon.py [--out <dir>] [--edge <msedge.exe>]

Reads src/app/resources/icon/WizzairHelper.svg (the full design, used from 40 px up) and
WizzairHelper-small.svg (hand-tuned for 16 to 32 px) and writes into that folder, or into
--out:

    WizzairHelper-<N>.png  N = 16 20 24 32 40 48 64 96 128 256
    WizzairHelper.ico      one PNG-compressed entry per size, each holding exactly the
                           bytes of WizzairHelper-<N>.png (nothing resampled)

Every size is drawn from the vector art at that pixel size by headless Microsoft Edge (a
full SVG engine) on a transparent background and cropped with Pillow. The outputs are
committed, so building the app needs none of this; run it after changing an SVG and commit
the SVG together with the regenerated files. Needs Python 3.10+, Pillow and Microsoft Edge.

The output is deterministic: the same SVGs, Edge and Pillow versions give byte-identical
files (no timestamps or metadata chunks; the .ico is written here, not by an encoder).
"""
import argparse
import io
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

from PIL import Image

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
SMALL_MAX = 32  # sizes up to this use WizzairHelper-small.svg
ICON_DIR = pathlib.Path(__file__).resolve().parent.parent / "src" / "app" / "resources" / "icon"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def find_edge() -> pathlib.Path:
    for root in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles"),
                 os.environ.get("LOCALAPPDATA")):
        if root:
            candidate = pathlib.Path(root) / "Microsoft" / "Edge" / "Application" / "msedge.exe"
            if candidate.is_file():
                return candidate
    raise SystemExit("Microsoft Edge (msedge.exe) not found; pass --edge <path of msedge.exe>")


def render(edge: pathlib.Path, svg_text: str, size: int) -> bytes:
    """The SVG drawn at size x size pixels, as PNG bytes without metadata."""
    html = ("<!doctype html><html><head><style>html,body{margin:0;padding:0;background:transparent;"
            f"overflow:hidden;width:{size}px;height:{size}px}}svg{{display:block;width:{size}px;"
            f"height:{size}px}}</style></head><body>{svg_text}</body></html>")
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
        page = pathlib.Path(tmp) / "icon.html"
        page.write_text(html, encoding="utf-8")
        shot = pathlib.Path(tmp) / "shot.png"
        # Headless windows have a minimum width, so render into a larger viewport and crop.
        # A throw-away profile keeps a running Edge (and the user's profile) out of it.
        viewport = max(size, 320)
        subprocess.run([str(edge), "--headless=new", "--disable-gpu", "--hide-scrollbars",
                        "--force-device-scale-factor=1", "--force-color-profile=srgb",
                        "--default-background-color=00000000", "--no-first-run",
                        "--no-default-browser-check", "--disable-extensions",
                        f"--user-data-dir={pathlib.Path(tmp) / 'profile'}",
                        f"--window-size={viewport},{viewport}", f"--screenshot={shot}",
                        page.as_uri()], check=True, capture_output=True, timeout=120)
        with Image.open(shot) as screenshot:
            pixels = screenshot.convert("RGBA").crop((0, 0, size, size)).tobytes()
    # A fresh image carries none of the screenshot's info (gamma, ICC profile, text).
    image = Image.frombytes("RGBA", (size, size), pixels)
    if image.getextrema()[3][1] == 0:
        raise SystemExit(f"Edge drew nothing at {size} px")
    out = io.BytesIO()
    image.save(out, format="PNG", optimize=False, compress_level=9)
    return out.getvalue()


def ico(pngs: dict[int, bytes]) -> bytes:
    """An .ico with one PNG-compressed 32-bit entry per size, smallest first (Windows Vista
    and later read PNG entries; rc.exe, llvm-rc and Inno Setup copy them as they are)."""
    sizes = sorted(pngs)
    header = struct.pack("<HHH", 0, 1, len(sizes))  # reserved, type 1 = icon, count
    entries = b""
    data = b""
    offset = len(header) + 16 * len(sizes)
    for size in sizes:
        png = pngs[size]
        assert png.startswith(PNG_SIGNATURE)
        side = 0 if size == 256 else size  # 0 means 256 in the one-byte width and height
        # width, height, palette colours, reserved, planes, bits per pixel, bytes, offset
        entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(png), offset + len(data))
        data += png
    return header + entries + data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=pathlib.Path, default=ICON_DIR,
                        help="output folder (default: the SVGs' folder, src/app/resources/icon)")
    parser.add_argument("--edge", type=pathlib.Path, help="path of msedge.exe")
    args = parser.parse_args()
    edge = args.edge or find_edge()
    full = (ICON_DIR / "WizzairHelper.svg").read_text(encoding="utf-8")
    small = (ICON_DIR / "WizzairHelper-small.svg").read_text(encoding="utf-8")
    args.out.mkdir(parents=True, exist_ok=True)
    pngs = {}
    for size in SIZES:
        pngs[size] = render(edge, small if size <= SMALL_MAX else full, size)
        (args.out / f"WizzairHelper-{size}.png").write_bytes(pngs[size])
    (args.out / "WizzairHelper.ico").write_bytes(ico(pngs))
    print(f"wrote {len(pngs)} PNGs and WizzairHelper.ico to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
