"""Generate the executable icon (launcher/TrueCrimeNYC.ico).

Usage:
    python scripts/make_icon.py [path-to-chromium-browser]

Needs Pillow (pip install pillow) and a Chromium browser (Brave, Chrome or
Edge) for rendering. Each icon size is drawn separately, so the small sizes
stay sharp instead of being shrunk from the large one:

  * 32 px and up: "TC" over a red rule, with "NYC" underneath
  * 16 and 24 px: "TC" alone, filling the tile ("NYC" would be unreadable)

The lettering uses Impact, which ships with Windows, so run this on Windows.
The colours match the README banner (scripts/make_banner.py).
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
from io import BytesIO
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "launcher" / "TrueCrimeNYC.ico"
SIZES = [256, 128, 64, 48, 32, 24, 16]

CREAM = "#f1ece4"
RED = "#e01f2b"

BROWSERS = [
    r"C:\Program Files\BraveSoftware\Brave-Browser\Application\brave.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]


def defs(uid):
    return f"""
  <defs>
    <linearGradient id="bg{uid}" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#3d080c"/>
      <stop offset="0.5" stop-color="#14080b"/>
      <stop offset="1" stop-color="#10213a"/>
    </linearGradient>
    <filter id="sh{uid}" x="-20%" y="-20%" width="140%" height="150%">
      <feDropShadow dx="0" dy="4" stdDeviation="4" flood-color="#000" flood-opacity="0.7"/>
    </filter>
  </defs>"""


def edge(size, inset, radius):
    """A thin light edge so the dark tile stays visible on a dark taskbar."""
    w = max(256 / size, 2)
    opacity = 0.22 if size < 128 else 0.12
    return (f'<rect x="{inset + w / 2}" y="{inset + w / 2}" width="{256 - 2 * inset - w}" '
            f'height="{256 - 2 * inset - w}" rx="{radius - w / 2}" fill="none" '
            f'stroke="#fff" stroke-opacity="{opacity}" stroke-width="{w}"/>')


def icon_svg(size):
    uid = str(size)
    # A little inset at large sizes, edge to edge at small ones so the
    # lettering gets every pixel.
    if size >= 128:
        inset, radius = 10, 46
    elif size >= 32:
        inset, radius = 4, 40
    else:
        inset, radius = 0, 36
    tile = (f'<rect x="{inset}" y="{inset}" width="{256 - 2 * inset}" height="{256 - 2 * inset}" '
            f'rx="{radius}" fill="url(#bg{uid})"/>')
    shadow = f' filter="url(#sh{uid})"' if size >= 64 else ""

    if size >= 32:
        # Letter spacing adds space after the last letter too, so nudge
        # "NYC" right by half of it to keep it centred.
        mark = (f'<g{shadow} font-family="Impact, sans-serif" text-anchor="middle">'
                f'<text x="128" y="136" font-size="140" fill="{CREAM}">T<tspan fill="{RED}">C</tspan></text>'
                f'<text x="135" y="232" font-size="70" letter-spacing="14" fill="{CREAM}">NYC</text></g>'
                f'<rect x="52" y="150" width="152" height="10" fill="{RED}"/>')
    else:
        mark = (f'<g font-family="Impact, sans-serif" font-size="232" text-anchor="middle">'
                f'<text x="128" y="214" fill="{CREAM}">T<tspan fill="{RED}">C</tspan></text></g>')

    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 256 256">'
            f'{defs(uid)}{tile}{mark}{edge(size, inset, radius)}</svg>')


def find_browser():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for name in ("brave", "chrome", "msedge", "chromium"):
        if shutil.which(name):
            return shutil.which(name)
    for path in BROWSERS:
        if os.path.exists(path):
            return path
    sys.exit("No Chromium browser found; pass its path as the first argument.")


def render():
    """Render every size side by side in one screenshot and crop them out."""
    total = sum(SIZES)
    x = 0
    pieces = []
    for s in SIZES:
        pieces.append(f'<div style="position:absolute;left:{x}px;top:0">{icon_svg(s)}</div>')
        x += s
    html = ('<!doctype html><html><body style="margin:0;background:transparent">'
            + "".join(pieces) + "</body></html>")

    with tempfile.TemporaryDirectory() as tmp:
        page = Path(tmp) / "icon.html"
        shot = Path(tmp) / "icon.png"
        page.write_text(html, encoding="utf-8")
        subprocess.run(
            [find_browser(), "--headless=new", "--disable-gpu", "--hide-scrollbars",
             "--force-device-scale-factor=1", "--default-background-color=00000000",
             f"--window-size={total},256", f"--screenshot={shot}", page.as_uri()],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        sheet = Image.open(shot).convert("RGBA")
        sheet.load()

    images = {}
    x = 0
    for s in SIZES:
        images[s] = sheet.crop((x, 0, x + s, s))
        x += s
    return images


def write_ico(images, path):
    """Pack PNG-compressed frames into an .ico, largest first."""
    blobs = []
    for s in SIZES:
        buf = BytesIO()
        images[s].save(buf, format="PNG", optimize=True)
        blobs.append((s, buf.getvalue()))
    header = struct.pack("<HHH", 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    entries, data = b"", b""
    for s, blob in blobs:
        dim = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset + len(data))
        data += blob
    path.write_bytes(header + entries + data)


if __name__ == "__main__":
    imgs = render()
    write_ico(imgs, OUT)
    if os.environ.get("ICON_PREVIEW_DIR"):
        for s, im in imgs.items():
            im.save(Path(os.environ["ICON_PREVIEW_DIR"]) / f"icon_{s}.png")
    print(f"wrote {OUT}")
