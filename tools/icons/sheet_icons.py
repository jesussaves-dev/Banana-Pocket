#!/usr/bin/env python3
"""BananaOS system icons from painted sprite sheets: tools/icons/sheets/*.jpg -> 32x32 PNGs in
ui/assets/overrides/icons (the source of truth, see docs/ICONS.md) and ui/assets/rnds/icons (the working set).

The sheets are 1024x1024 JPEGs on a chroma-green (or magenta) ground, a 3x3 (or 2x2) grid of sprites drawn on a
~5 px grid. Each cell is keyed off the ground, cropped to its sprite, centred on a square, box-reduced to ART px
(premultiplied, so no green fringe) and quantised to a flat palette so the JPEG noise goes; then the set's drop
shadow, one icon px down-right. Ported from banana-sm8550's 64 px cutter: 32 px is what this tree's themes draw
(the SNES street's `pixel` boxes are 32 and 16; Classic's `pixelContain` takes 32 px at a whole 3x), so the art is
reduced from the sheet once instead of nearest-shrunk from 64.

Only icons that ui/assets/rnds/systems.cfg names are written. The six small UI icons (gear, music, picture, gamepad,
moon, phone) are hand-made at 32 px in sheets/32 and copied as they are. `apps` is left alone: its green tile is
eaten by the chroma key.

    python3 tools/icons/sheet_icons.py [name ...]

Needs Pillow and numpy. Deterministic.
"""
import os
import re
import shutil
import sys

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC = os.path.join(ROOT, "tools/icons/sheets")
OUTS = [os.path.join(ROOT, "ui/assets/overrides/icons"), os.path.join(ROOT, "ui/assets/rnds/icons")]
CFG = os.path.join(ROOT, "ui/assets/rnds/systems.cfg")
N, ART, COLOURS = 32, 30, 24
SHADOW = (8, 10, 18, 96)
SKIP = {"apps"}

# icon name -> (sheet file prefix, grid, row, col) for an even grid of cells, or (sheet file prefix, (x0, y0, x1, y1))
# for a sprite in a box of the 1024 px sheet
B = "batch_sheet_"
R = "retro_consoles_spritesheet_"
N1, N2, N3 = "batch1_nintendo_modern", "batch2_sony_microsoft", "batch3_sega_arcades"
CELLS = {
    "apps": (B + "1", 3, 0, 0), "collection": (B + "1", 3, 0, 1), "gamepad": (B + "1", 3, 0, 2),
    "moon": (B + "1", 3, 1, 0), "ports": (B + "1", 3, 1, 1),
    "arcade": (B + "1", 3, 2, 0), "computer": (B + "1", 3, 2, 1), "gba": (B + "1", 3, 2, 2),
    "music": (B + "2", 3, 0, 0), "psp": (B + "2", 3, 0, 1), "sword": (B + "2", 3, 0, 2),
    "atari2600": (B + "2", 3, 1, 0), "console": (B + "2", 3, 1, 1),
    "n64": (B + "2", 3, 2, 0), "psx": (B + "2", 3, 2, 1), "tools": (B + "2", 3, 2, 2),
    "atarilynx": (B + "3", 3, 0, 0), "disc": (B + "3", 3, 0, 1), "gb": (B + "3", 3, 0, 2),
    "nds": (B + "3", 3, 1, 0), "recent": (B + "3", 3, 1, 1), "video": (B + "3", 3, 1, 2),
    "bananatube": (B + "3", 3, 2, 0), "dodakvarter": (B + "3", 3, 2, 1), "gear": (B + "3", 3, 2, 2),
    "nes": (B + "4", 3, 0, 0), "saturn": (B + "4", 3, 0, 1), "wonderswan": (B + "4", 3, 0, 2),
    "bank": (B + "4", 3, 1, 0), "dreamcast": (B + "4", 3, 1, 1),
    "ngp": (B + "4", 3, 2, 0), "search": (B + "4", 3, 2, 1), "zxspectrum": (B + "4", 3, 2, 2),
    "book": (B + "5", 3, 0, 0), "fantasy": (B + "5", 3, 0, 1), "joystick": (B + "5", 3, 0, 2),
    "pcengine": (B + "5", 3, 1, 0), "sega32x": (B + "5", 3, 1, 1), "c64": (B + "5", 3, 1, 2),
    "fav": (B + "5", 3, 2, 0), "mastersystem": (B + "5", 3, 2, 1), "phone": (B + "5", 3, 2, 2),
    "segacd": (B + "6", 3, 0, 0), "cart": (B + "6", 3, 0, 1), "gamegear": (B + "6", 3, 0, 2),
    "md": (B + "6", 3, 1, 0), "picture": (B + "6", 3, 1, 1), "snes": (B + "6", 3, 1, 2),
    "steam": (R, 2, 0, 0), "gbc": (R, 2, 0, 1), "handheld": (R, 2, 1, 0),
    # batch1-3: systems with no emulator shipped yet, so a user's own systems.conf entry gets its own icon
    "switch": (N1, 3, 0, 0), "wiiu": (N1, 3, 0, 1), "wii": (N1, 3, 0, 2),
    "gc": (N1, 3, 1, 0), "3ds": (N1, 3, 1, 1), "virtualboy": (N1, 3, 1, 2),
    "ps2": (N2, (0, 0, 364, 340)), "ps3": (N2, (364, 0, 684, 340)), "psvita": (N2, (684, 0, 1024, 340)),
    "xbox": (N2, (0, 340, 445, 764)), "xbox360": (N2, (445, 340, 684, 764)),
    "naomi": (N3, 3, 0, 2), "neogeo": (N3, 3, 1, 0), "neocd": (N3, 3, 1, 1), "sg1000": (N3, 3, 2, 0),
}
SMALL = ["gear", "music", "picture", "gamepad", "moon", "phone"]
def sheet(prefix):
    for f in sorted(os.listdir(SRC)):
        if f.startswith(prefix) and f.endswith((".jpg", ".png")):
            return os.path.join(SRC, f)
    sys.exit("no sheet " + prefix + "* in " + SRC)


def magenta(a):
    """True for a sheet keyed on magenta (its corner), else it's chroma green"""
    r, g, b = (int(v) for v in a[2:10, 2:10].reshape(-1, 3).mean(0))
    return r > 160 and b > 160 and g < 80


def key(a, mag):
    r, g, b = (a[..., i].astype(int) for i in range(3))
    if mag:
        return ~((r > 140) & (b > 140) & (g < np.minimum(r, b) - 60))
    return ~((g > 120) & (g > r + 50) & (g > b + 50))


def cell(path, spec):
    a = np.asarray(Image.open(path).convert("RGB"))
    m = 12	# clear of the lines some sheets draw between cells
    if len(spec) == 1:
        x0, y0, x1, y1 = spec[0]
    else:
        grid, row, col = spec
        h = a.shape[0] // grid
        x0, y0, x1, y1 = col * h, row * h, (col + 1) * h, (row + 1) * h
    c = a[y0 + m:y1 - m, x0 + m:x1 - m]
    mag = magenta(a)
    fg = key(c, mag)
    # a divider line still inside the box: a row or column that's all foreground, at the box's edge
    for ax in (0, 1):
        full = fg.mean(ax) > 0.97
        n = full.shape[0]
        edge = np.zeros(n, bool)
        edge[:24] = edge[-24:] = True
        if ax == 0:
            fg[:, full & edge] = False
        else:
            fg[full & edge, :] = False
    ys, xs = np.where(fg.any(1))[0], np.where(fg.any(0))[0]
    if len(ys) < 8:
        sys.exit("%s %s is empty" % (path, spec))
    c, fg = c[ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1].astype(int), fg[ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1]
    if mag:
        # despill the edge only (a sprite can be purple): pull red and blue down to the pixel's green there
        bg = ~fg
        near = np.zeros_like(fg)
        for dy in (-2, -1, 0, 1, 2):
            for dx in (-2, -1, 0, 1, 2):
                near |= np.roll(np.roll(bg, dy, 0), dx, 1)
        e = near & fg
        lo = np.minimum(c[..., 0], c[..., 2])
        spill = np.clip(lo - c[..., 1] - 30, 0, None) * e
        c[..., 0] -= spill
        c[..., 2] -= spill
    else:
        # despill: the JPEG bleeds green into the edge pixels
        c[..., 1] = np.minimum(c[..., 1], np.maximum(np.maximum(c[..., 0], c[..., 2]), c[..., 1] - 40))
    return c, fg


def reduce(c, fg, art=ART):
    h, w = fg.shape
    s = max(h, w)
    p = np.zeros((s, s, 4), float)
    y, x = (s - h) // 2, (s - w) // 2
    p[y:y + h, x:x + w, :3] = c * fg[..., None]
    p[y:y + h, x:x + w, 3] = fg * 255.0
    rgb = np.dstack([np.asarray(Image.fromarray(p[..., i].astype(np.uint8)).resize((art, art), Image.BOX), float)
                     for i in range(4)])
    al = rgb[..., 3]
    rgb = np.where(al[..., None] > 0, rgb[..., :3] * 255 / np.maximum(al[..., None], 1), 0).clip(0, 255)
    solid = al >= 128
    # one flat palette across the sprite's opaque pixels
    q = Image.fromarray(rgb.astype(np.uint8)).quantize(COLOURS, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    rgb = np.asarray(q.convert("RGB"))
    return rgb, solid


def finish(rgb, solid):
    out = np.zeros((N, N, 4), np.uint8)
    o = (N - ART - 1) // 2
    sh = np.zeros((N, N), bool)
    sh[o + 1:o + 1 + ART, o + 1:o + 1 + ART] = solid
    out[sh] = SHADOW
    v = out[o:o + ART, o:o + ART]
    v[solid, :3] = rgb[solid]
    v[solid, 3] = 255
    return out


def wanted():
    """the icon files systems.cfg names"""
    names = set()
    for ln in open(CFG, encoding="utf-8"):
        f = ln.split("#")[0].split()
        if len(f) >= 2 and f[1].endswith(".png"):
            names.add(f[1][:-4])
    return names


def save(img, name):
    for d in OUTS:
        img.save(os.path.join(d, name + ".png"), optimize=True)


def main():
    names = sys.argv[1:] or sorted((set(CELLS) | set(SMALL)) & wanted() - SKIP)
    for n in names:
        if n in SMALL:
            save(Image.open(os.path.join(SRC, "32", n + ".png")).convert("RGBA"), n)
            continue
        if n not in CELLS:
            sys.exit("unknown icon " + n)
        prefix, *spec = CELLS[n]
        save(Image.fromarray(finish(*reduce(*cell(sheet(prefix), spec)))), n)
    print("%d icons -> %s" % (len(names), ", ".join(os.path.relpath(d, ROOT) for d in OUTS)))


if __name__ == "__main__":
    main()
