#!/usr/bin/env python3
# Clear the screen window of the shipped console frames (system/rootfs/usr/share/banana/overlays) to fully transparent.
# SkyWalker541's art carries a 1px LCD grid inside the window, aligned to one integer scale. The compositor places a
# game of any scale there (docs/superpowers/specs/2026-11-10-compositor-bezels-design.md), so the grid has to go. The
# art is CC0. Run once; running it again changes nothing.
#
#   tools/strip-frame-grid.py [overlays dir]
import os
import sys

from PIL import Image

WINDOWS = {  # x, y, w, h: the integer window each frame was cut for
    "gb.png": (60, 36, 1120, 1008),
    "gbc.png": (60, 36, 1120, 1008),
    "gba.png": (20, 140, 1200, 800),
}


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    d = argv[1] if len(argv) > 1 else os.path.join(root, "system/rootfs/usr/share/banana/overlays")
    for name, (x, y, w, h) in WINDOWS.items():
        path = os.path.join(d, name)
        im = Image.open(path).convert("RGBA")
        im.paste((0, 0, 0, 0), (x, y, x + w, y + h))
        im.save(path, optimize=True)
        box = im.getchannel("A").point(lambda v: 255 if v == 0 else 0).getbbox()
        print(f"{name}: hole {box}")
        if box != (x, y, x + w, y + h):
            print(f"{name}: transparent pixels outside the window", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
