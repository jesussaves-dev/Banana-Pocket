#!/usr/bin/env python3
"""Banana Blast's art (the Genesis style), baked to PNGs at one pixel per layout unit on the 310-wide canvas (drawn 4x).

  tools/gen-blast.py [themes-dir]      writes <themes-dir>/blast/<palette>/... (default system/rootfs/usr/share/banana/themes)

The rules are the Mega Drive's: every colour snapped to 9 bits (levels 0-7 a channel), true black (#000) for every
backdrop so the AMOLED's pixels stay off, no blends (gradients are checker dithers or one colour per scanline), and
level 1 only for thin distant lines. Always night. Per palette it writes:

  sky.png             the black sky and the horizon's raster bar, 310 x 151 (the horizon line is row 150)
  stars, shapes, city
                      parallax layers, 640 wide and repeating every 320 (the theme's `layer` component); stars.png
                      holds the steady stars, and the twinkling ones are 1x1 rects in blast.json (see --twinkle)
  floor/row<i>.png    the neon floor, one picture per scanline below the horizon (i = 0..118, depth d = i + 1): the
                      Mega Drive's per-line horizontal scroll. Each row is drawn at x = -round(mod(scroll*130, 32)*d/26),
                      so its perspective lines sit where they belong on every line and the grid never kinks
  ride{0,1}, idle{0,1}, speed{0,1}
                      the banana on its board (wheels turning), off it tapping a foot, and the boost's speed lines
"""
import os, sys
from PIL import Image

LV = [0, 36, 73, 109, 146, 182, 219, 255]
W, HZ, FLOOR = 310, 150, 119          # canvas width, horizon row, floor scanlines (151..269)
SHOP, CELL, NEAR = 130, 32, 26        # units per system, grid cell at depth NEAR

# 9-bit triples (0-7 a channel)
PALETTES = {
    "night": {  # Night Drive: magenta floor, cyan rails, violet city
        "h": (7, 0, 5), "v": (0, 5, 7), "city": (3, 0, 7), "shapes": [(7, 0, 5), (0, 6, 7), (7, 6, 0)],
        "band": [(1, 0, 2), (2, 0, 3), (3, 0, 4), (4, 0, 5), (5, 0, 5), (6, 0, 5), (7, 0, 5), (7, 1, 4), (7, 3, 3), (7, 5, 2)],
    },
}


def C(t): return (LV[t[0]], LV[t[1]], LV[t[2]], 255)
def S(t, lv): return C(tuple(round(v * lv / 7) for v in t))          # the same hue at a lower level
def q9(rgb): return tuple(LV[round(v / 255 * 7)] for v in rgb[:3]) + ((rgb[3],) if len(rgb) > 3 else (255,))


class Canvas:
    def __init__(self, w, h, fill=None):
        self.im = Image.new("RGBA", (w, h), fill or (0, 0, 0, 0))
        self.px = self.im.load()

    def P(self, x, y, c):
        if 0 <= x < self.im.width and 0 <= y < self.im.height: self.px[x, y] = c

    def R(self, x, y, w, h, c):
        for yy in range(max(0, y), min(self.im.height, y + h)):
            for xx in range(max(0, x), min(self.im.width, x + w)): self.px[xx, yy] = c

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        e = dx + dy
        while True:
            self.P(x0, y0, c)
            if x0 == x1 and y0 == y1: break
            e2 = 2 * e
            if e2 >= dy: e += dy; x0 += sx
            if e2 <= dx: e += dx; y0 += sy

    def wrap(self, x, y, c, period=320):
        """A pixel of a repeating layer: written at x and x +- the period, so the 640 picture tiles seamlessly."""
        for o in (-period, 0, period): self.P(x + o, y, c)

    def save(self, path):
        for y in range(self.im.height):          # the hardware's colours only, whatever was drawn
            for x in range(self.im.width):
                c = self.px[x, y]
                if c[3]: self.px[x, y] = q9(c)
        self.im.save(path)


def rng(seed):
    """mulberry32, the mockup's generator, so the layout matches the design page."""
    s = [seed & 0xffffffff]
    def r():
        s[0] = (s[0] + 0x6D2B79F5) & 0xffffffff
        t = s[0]
        t = ((t ^ (t >> 15)) * (1 | t)) & 0xffffffff
        t = (t + (((t ^ (t >> 7)) * (61 | t)) & 0xffffffff) ^ t) & 0xffffffff
        return ((t ^ (t >> 14)) & 0xffffffff) / 4294967296
    return r


def sky(p):
    c = Canvas(W, HZ + 1, (0, 0, 0, 255))
    for i, b in enumerate(p["band"]): c.R(0, HZ - len(p["band"]) + i, W, 1, C(b))
    c.R(0, HZ, W, 1, S(p["h"], 7))
    return c


def star_field():
    """The 40 stars: (x, y, twinkles). Every other star twinkles; the rest hold a steady mid level."""
    r = rng(1994)
    out = []
    for i in range(40):
        x, y = int(r() * 320), 22 + int(r() * 96)   # clear of the HUD
        out.append((x, y, i % 2 == 0, i % 4 == 0))   # (x, y, twinkles, dim on frame 0)
    return out


def stars():
    """The steady stars only. The twinkling ones are 1x1 rects in blast.json (a small region each, so an idle
    twinkle redraws a few pixels and not the sky); `gen-blast.py --twinkle` prints their pick() lists."""
    c = Canvas(640, HZ)
    for x, y, tw, _ in star_field():
        if not tw: c.wrap(x, y, C((4, 4, 6)))
    return c


def twinkle_picks():
    """Twinkler j starts dim when j is even (the old frame 0), so blast.json's phase is mod(i + frame, 2)."""
    tw = [(x, y) for x, y, t, _ in star_field() if t]
    return (str(len(tw)), "pick(i, %s)" % ", ".join(str(x) for x, _ in tw), "pick(i, %s)" % ", ".join(str(y) for _, y in tw))


def shapes(p):
    r = rng(77)
    c = Canvas(640, HZ)
    for i in range(4):
        x0, y0, col = i * 80 + int(r() * 30), 26 + int(r() * 60), S(p["shapes"][i % 3], 5)
        t = i % 3
        pts = []
        if t == 0:                                # a zigzag
            for k in range(4): pts.append(((x0 + k * 6, y0 + (6 if k & 1 else 0)), (x0 + k * 6 + 6, y0 + (0 if k & 1 else 6))))
        elif t == 1:                              # a triangle
            pts = [((x0, y0 + 12), (x0 + 7, y0)), ((x0 + 7, y0), (x0 + 14, y0 + 12)), ((x0, y0 + 12), (x0 + 14, y0 + 12))]
        for (a, b) in pts:
            for o in (0, 320): c.line(a[0] + o, a[1], b[0] + o, b[1], col)
        if t == 2:                                # a cross
            for o in (0, 320): c.R(x0 + 5 + o, y0, 2, 12, col); c.R(x0 + o, y0 + 5, 12, 2, col)
    return c


def city(p):
    r = rng(2049)
    c = Canvas(640, HZ)
    edge, lit = S(p["city"], 4), C((7, 6, 1))
    x = 0
    while x < 320 - 14:
        w, h = 12 + int(r() * 22), 12 + int(r() * 44)
        w = min(w, 320 - x)
        top = HZ - h
        for o in (0, 320):
            c.R(x + o, top, w, h, (0, 0, 0, 255))
            c.R(x + o, top, w, 1, edge); c.R(x + o, top, 1, h, edge); c.R(x + o + w - 1, top, 1, h, edge)
        for k in range(w * h // 60):
            if k % 5: c.wrap(x + 2 + int(r() * (w - 4)), top + 3 + int(r() * (h - 5)), lit)
        x += w + int(r() * 6)
    return c


def floor_lines():
    """The depths whose scanline carries a cross line: every 16 units of distance, while they stay two rows apart."""
    out, k = [], 1
    while True:
        d = round(100 / k)
        if d < 4 or (out and out[-1] - d < 2): break
        out.append(d); k += 1
    return set(out)


def floor_row(p, d, across):
    """Scanline d below the horizon. Lit wholly when a cross line runs along it; otherwise a dot where each rail
    crosses it, the rails CELL apart at depth NEAR and converging on the centre. Wide enough for one cell's scroll."""
    span = CELL * d / NEAR
    c = Canvas(W + int(span) + 2, 1)
    lv = min(7, 2 + d // 16)
    if d in across:
        c.R(0, 0, c.im.width, 1, S(p["h"], lv))
    elif d >= 3:
        # each rail joined to where it crosses the next scanline down, so a shallow rail is a line and not dots;
        # far out, where that join would be wider than half a cell, a dot (the distance's shimmer)
        col = S(p["v"], max(2, lv - 1))
        nxt = CELL * (d + 1) / NEAR
        far = S(p["v"], 2)
        k0 = -int(155 / span) - 1
        for k in range(k0, k0 + int(c.im.width / span) + 3):
            x0, x1 = round(155 + k * span), round(155 + k * nxt)
            if abs(x1 - x0) * 2 > span: c.P(x0, 0, far)
            else: c.R(min(x0, x1), 0, abs(x1 - x0) + 1, 1, col)
    return c


# ---- the banana: k outline, y body, o shade, b stem, g lens, c glint / sneaker, m deck, w wheel
BAN = ['.......kk.......', '......kbbk......', '......kbbk......', '.....kyyyyk.....', '....kyyyyyyk....', '....kggggggk....',
       '....kgcggcgk....', '....kyyyyyyok...', '....kyyyyyyok...', '....kyykkyyok...', '....kyyyyyyok...', '....kyyyyyyok...',
       '.....kyyyyyok...', '.....kyyyyok....', '......kyyyok....', '.......kkkk.....']
LEGS = [['.......o..o.....', '......cc..cc....'], ['.......o..cc....', '......cc........']]
BOARD = ['..kmmmmmmmmmmk..', '...kkkkkkkkkk...']
PAL = {"k": (0, 0, 0), "y": (7, 6, 0), "o": (6, 3, 0), "b": (3, 2, 0), "g": (2, 0, 4), "c": (0, 6, 7), "m": (7, 0, 5),
       "w": (7, 7, 7), "v": (4, 4, 4)}


def spr(c, rows, x, y):
    for r, row in enumerate(rows):
        for k, ch in enumerate(row):
            if ch != '.': c.P(x + k, y + r, C(PAL[ch]))


def ride(frame):
    """22 rows: the banana (16), its legs (2), the board (2) and the wheels (1), and a row of air under them."""
    c = Canvas(32, 22)
    x = 8
    spr(c, BAN, x, 0); spr(c, LEGS[0], x, 16); spr(c, BOARD, x, 18)
    wheel = C(PAL["v" if frame else "w"])
    for k in (4, 5, 10, 11): c.P(x + k, 20, wheel)
    return c


def idle(tap):
    """Off the board, tapping a foot and wagging a finger, the board parked on its edge behind."""
    c = Canvas(32, 22)
    x = 8
    spr(c, BOARD, x + 14, 19)
    rows = list(BAN)
    if tap: rows[6] = '....kcggcggk....'
    spr(c, rows, x, 3); spr(c, LEGS[tap], x, 19)
    y = C(PAL["y"])
    c.P(x + 12, 12, y); c.P(x + 13, 11, y); c.P(x + (14 if tap else 12), 10, y); c.P(x + (15 if tap else 12), 9, y)
    return c


def speed(frame):
    r = rng(13 + frame * 101)
    c = Canvas(W, 186)
    for i in range(16):
        y, n, x = int(r() * 186), 18 + int(r() * 50), int(r() * W)
        c.R(x, y, n, 1, C((0, 6, 7) if i & 1 else (7, 7, 7)))
    return c


def main():
    if "--twinkle" in sys.argv:
        for v in twinkle_picks(): print(v)
        return
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "system", "rootfs", "usr", "share", "banana", "themes")
    print("gen-blast: %d images in %s" % (len(write(root)), os.path.join(root, "blast")))


def write(root):
    """Bakes every palette under <root>/blast and returns the paths written, relative to root (check-themes.py
    regenerates into a temp dir and compares, so the output must stay byte-for-byte reproducible)."""
    done = []
    across = floor_lines()
    for name, p in PALETTES.items():
        out = os.path.join(root, "blast", name)
        os.makedirs(os.path.join(out, "floor"), exist_ok=True)
        files = {"sky": sky(p), "stars": stars(), "shapes": shapes(p), "city": city(p),
                 "ride0": ride(0), "ride1": ride(1), "idle0": idle(0), "idle1": idle(1), "speed0": speed(0), "speed1": speed(1)}
        for i in range(FLOOR): files["floor/row%d" % i] = floor_row(p, i + 1, across)
        for k, c in files.items():
            c.save(os.path.join(out, k + ".png"))
            done.append(os.path.join("blast", name, k + ".png"))
    return done


if __name__ == "__main__":
    main()
