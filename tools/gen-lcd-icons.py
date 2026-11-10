#!/usr/bin/env python3
"""Recolour the system icons into each LCD theme's four shades.

For every theme named below: reads the shades from the theme's own "vars" (bg, shade, soft, ink), maps each source
icon's brightness onto them, and writes themes/lcd/<theme>/<icon>.png. Run it again after changing a theme's palette.

    tools/gen-lcd-icons.py
"""
import re
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
THEMES = ROOT / "system/rootfs/usr/share/banana/themes"
ICONS = ROOT / "ui/assets/rnds/icons"
# a theme, and the folder its colour icons come from
SOURCES = {
    "banana": ICONS, "ocean": ICONS, "sakura": ICONS, "sunset": ICONS, "mint": ICONS,
    "fantasy": THEMES / "fantasy",
}


def shades(theme):
    """The theme's four shades, darkest first."""
    text = (THEMES / f"lcd-{theme}.json").read_text()
    out = []
    for name in ("ink", "soft", "shade", "bg"):
        m = re.search(r'"%s":\s*"#([0-9a-fA-F]{6})"' % name, text)
        if not m:
            sys.exit(f"gen-lcd-icons: lcd-{theme}.json has no var {name}")
        out.append(tuple(bytes.fromhex(m.group(1))))
    return out


def recolour(src, ramp):
    """An RGBA copy of src: its opaque pixels' brightness, stretched to the full range, in four steps of ramp."""
    img = Image.open(src).convert("RGBA")
    px = img.load()
    w, h = img.size
    lum = {}
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a >= 128:
                lum[x, y] = 0.299 * r + 0.587 * g + 0.114 * b
    out = Image.new("RGBA", img.size, (0, 0, 0, 0))
    if not lum:
        return out
    lo, hi = min(lum.values()), max(lum.values())
    span = max(hi - lo, 1)
    dst = out.load()
    for (x, y), v in lum.items():
        dst[x, y] = ramp[min(3, int((v - lo) / span * 4))] + (255,)
    return out


def main():
    count = 0
    for theme, folder in SOURCES.items():
        ramp = shades(theme)
        dest = THEMES / "lcd" / theme
        dest.mkdir(parents=True, exist_ok=True)
        for src in sorted(folder.glob("*.png")):
            recolour(src, ramp).save(dest / src.name, optimize=True)
            count += 1
    print(f"gen-lcd-icons: {count} icons")


if __name__ == "__main__":
    main()
