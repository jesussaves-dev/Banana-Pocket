#!/usr/bin/env python3
"""Banana Street's art, baked to PNGs at one pixel per layout unit (the 310-wide canvas draws them at 4x).

  tools/gen-street.py [themes-dir]     writes <themes-dir>/street/<palette>/... (default system/rootfs/usr/share/banana/themes)
                                       and street-<palette>.json for each neighborhood but Banana Street, from street.json

The rules are the Super NES's: every colour snapped to 15 bits, materials shaded in ramps with purple-shifted shadows,
skies stepped one scanline at a time, transparency only as half-tones and glow. Per palette and time of day (dawn,
day, dusk, night) it writes the sky, the parallax layers (320 wide, repeated), the road bands, the four shopfronts in
three awning colours, the lamp and the walker; the interface pieces (status panel, library) are per palette only.
The system icons stay live in the theme (drawn in each shop's window), so a new system needs no new art.
"""
import math, os, sys
from PIL import Image

PALETTES = {
    "banana": {"ink": "#2b1a33", "dk": "#5a3a4a", "mid": "#a98a78", "paper": "#fff3cf", "skyH": "#58b6e8", "skyL": "#a8e4f0",
               "wallA": "#e8a25c", "wallB": "#c9614a", "awnA": "#e84a5f", "awnB": "#3fa7a0", "awnC": "#7a5cc7", "gold": "#ffd21e",
               "goldLo": "#d08a14", "win": "#8fd3e8", "lit": "#ffc94a", "road": "#5b5670"},
    "harbor": {"ink": "#04121c", "dk": "#1d4763", "mid": "#5f93a8", "paper": "#e4f6ff", "skyH": "#2f86cf", "skyL": "#9fdcf0",
               "wallA": "#3d7f9e", "wallB": "#d9d3b8", "awnA": "#ee6a55", "awnB": "#2fb5b0", "awnC": "#f0ead2", "gold": "#56c9f0",
               "goldLo": "#2a86ad", "win": "#c0f0ff", "lit": "#ffe08a", "road": "#2e4a63"},
    "blossom": {"ink": "#1d0f16", "dk": "#6a3a55", "mid": "#b98aa0", "paper": "#fff0f4", "skyH": "#b6c8f0", "skyL": "#ffe0ea",
                "wallA": "#f6c4d3", "wallB": "#a8688a", "awnA": "#d64a7c", "awnB": "#7fb58a", "awnC": "#8a6cc0", "gold": "#ffb7cd",
                "goldLo": "#d9709a", "win": "#d8ecf5", "lit": "#ffdca0", "road": "#7a5a70"},
    "sundown": {"ink": "#1a0e10", "dk": "#5c2a3a", "mid": "#b0705a", "paper": "#ffe9d0", "skyH": "#e0604a", "skyL": "#ffb060",
                "wallA": "#d9764a", "wallB": "#8a3a50", "awnA": "#ffcc55", "awnB": "#3a8a90", "awnC": "#c04a7a", "gold": "#ff9e57",
                "goldLo": "#c8602c", "win": "#ffc98a", "lit": "#ffd87a", "road": "#4a2c44"},
    "oldtown": {"ink": "#0a0718", "dk": "#3a2c44", "mid": "#8a7a70", "paper": "#f4e6c0", "skyH": "#5a70b0", "skyL": "#c0b8d0",
                "wallA": "#b8a080", "wallB": "#6e5240", "awnA": "#7a2e3e", "awnB": "#2e6a5a", "awnC": "#3e4a8a", "gold": "#f2c14e",
                "goldLo": "#a8742a", "win": "#7ea8b8", "lit": "#ffb84a", "road": "#4a4050"},
}
# What sets each neighborhood apart beyond its colours: the shops' wall material, the backdrop behind them, the times
# of day it has (Sundown Strip is always dusk), its name, and an icon set (Old Town uses the Fantasy icons).
HOODS = {
    "banana": {"name": "Banana Street", "wall": "bricks", "back": "city", "phases": None},
    "harbor": {"name": "Harbor", "wall": "planks", "back": "sea", "phases": None},
    "blossom": {"name": "Blossom Row", "wall": "plaster", "back": "trees", "phases": None},
    "sundown": {"name": "Sundown Strip", "wall": "stucco", "back": "palms", "phases": ["dusk"]},
    "oldtown": {"name": "Old Town", "wall": "timber", "back": "towers", "phases": None, "icons": "fantasy.json"},
}
PHASES = {"dawn": ("#ff8a6a", .16, ["#5a68c8", "#e88aa8", "#ffc89a", "#ffe8b0"]), "day": (None, 0, None),
          "dusk": ("#6a3a8a", .3, ["#25286f", "#7a3f9a", "#f0785f", "#ffc070"]), "night": ("#14185a", .56, ["#060a30", "#141a58", "#2c2c78", "#4a3a80"])}
TINTED = ["dk", "mid", "wallA", "wallB", "awnA", "awnB", "awnC", "road", "win"]
RAMPED = ["ban", "wallA", "wallB", "awnA", "awnB", "awnC", "gold", "road", "dk", "mid", "paper", "win", "lit"]


def rgb(h): return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))
def hexc(c): return "#%02x%02x%02x" % c
def mix(a, b, t):
    A, B = rgb(a), rgb(b)
    return hexc(tuple(round(A[i] + (B[i] - A[i]) * t) for i in range(3)))
def q15(h): return tuple(round(round(v / 255 * 31) * 255 / 31) for v in rgb(h))   # 5 bits a channel


def palette(base, phase):
    p = dict(base)
    p["ban"] = "#ffd21e"	# the walker is a banana whatever the neighborhood's gold
    tint, a, sky = PHASES[phase]
    if a:
        for k in TINTED: p[k] = mix(p[k], tint, a)
    p["lights"] = phase in ("dusk", "night")
    if not p["lights"]: p["lit"] = p["win"]
    stops = sky or [p["skyH"], mix(p["skyH"], p["skyL"], .4), mix(p["skyH"], p["skyL"], .75), p["skyL"]]
    top, low = stops[0], stops[-1]
    for m in RAMPED:
        c = p[m]
        p[m + "0"], p[m + "1"], p[m + "2"] = mix(c, "#fff6c8", .5), mix(c, "#ffeeb0", .2), c
        p[m + "3"], p[m + "4"] = mix(c, "#3a1f6a", .3), mix(c, "#241050", .56)
    p["far"], p["far2"] = mix(low, top, .55), mix(mix(low, top, .7), p["dk"], .45)
    p["far3"] = mix(p["far2"], "#241050", .3)
    night = phase == "night"
    p["cloud0"], p["cloud1"], p["cloud2"] = mix(low, "#ffffff", .18 if night else .8), mix(low, "#ffffff", .1 if night else .5), mix(low, top, .35)
    p["hudA"], p["hudB"], p["shadow"] = mix(p["awnC"], p["ink"], .35), mix(p["awnC"], p["ink"], .88), "#180c30"
    p["sky"], p["phase"] = stops, phase
    return p


def hsh(s):
    h = 7
    for ch in str(s): h = (h * 31 + ord(ch)) & 0xffffffff
    return h


class Canvas:
    def __init__(self, w, h, p):
        self.im, self.p = Image.new("RGBA", (w, h), (0, 0, 0, 0)), p

    def col(self, c): return q15(self.p.get(c, c) if not c.startswith("#") else c)

    def R(self, x, y, w, h, c, a=1.0):
        x, y, w, h = int(x), int(y), int(w), int(h)
        x0, y0, x1, y1 = max(0, x), max(0, y), min(self.im.width, x + w), min(self.im.height, y + h)
        if x1 <= x0 or y1 <= y0: return
        rgbv = self.col(c)
        if a >= 1:
            self.im.paste(rgbv + (255,), (x0, y0, x1, y1))
        else:
            over = Image.new("RGBA", (x1 - x0, y1 - y0), rgbv + (round(a * 255),))
            self.im.alpha_composite(over, (x0, y0))

    def shade(self, x, y, w, h, m):
        self.R(x, y, w, h, m + "2"); self.R(x, y, w, 1, m + "0"); self.R(x, y, 1, h, m + "1")
        self.R(x, y + h - 1, w, 1, m + "4"); self.R(x + w - 1, y, 1, h, m + "3")

    def vgrad(self, x, y, w, h, a, b, alpha=1.0):
        A, B = self.p.get(a, a), self.p.get(b, b)
        for i in range(h): self.R(x, y + i, w, 1, mix(A, B, i / max(1, h - 1)), alpha)

    def disc(self, cx, cy, r, c, a=1.0):
        for y in range(-r, r + 1):
            hw = int(math.sqrt(r * r - y * y))
            self.R(cx - hw, cy + y, hw * 2 + 1, 1, c, a)

    def glow(self, cx, cy, r, c):
        for i in range(r, 0, -3): self.disc(cx, cy, i, c, .09)

    def bricks(self, x, y, w, h, m):
        self.R(x, y, w, h, m + "4")
        r, yy = 0, y
        while yy < y + h:
            xx = x - (r % 2) * 5
            while xx < x + w:
                k = hsh("%d:%d" % (xx, yy)) % 5
                c = m + ("1" if k == 0 else "3" if k == 1 else "2")
                a, b, hh = max(x, xx), min(x + w, xx + 9), min(4, y + h - yy)
                if b > a:
                    self.R(a, yy, b - a, hh, c); self.R(a, yy, b - a, 1, m + ("2" if k == 1 else "1"))
                xx += 10
            yy += 5; r += 1

    def planks(self, x, y, w, h, m):
        """Clapboard: overlapping boards 4 high, each lit on top and shadowed under, a joint here and there."""
        self.R(x, y, w, h, m + "2")
        for r, yy in enumerate(range(y, y + h, 4)):
            hh = min(4, y + h - yy)
            self.R(x, yy, w, 1, m + "1")
            if hh == 4: self.R(x, yy + 3, w, 1, m + "3")
            for xx in range(x + (hsh("pk%d" % r) % 23), x + w, 23 + hsh("pj%d" % r) % 9): self.R(xx, yy + 1, 1, max(1, hh - 1), m + "4")

    def plaster(self, x, y, w, h, m, bands=0):
        """Smooth render, a few flecks; bands: a scored line every that many rows (stucco)."""
        self.R(x, y, w, h, m + "2")
        for i in range(w * h // 40):
            k = hsh("pl%d:%d:%d" % (x, y, i))
            self.R(x + k % w, y + (k >> 8) % h, 1, 1, m + ("1" if k % 3 else "3"))
        if bands:
            for yy in range(y + bands, y + h, bands): self.R(x, yy, w, 1, m + "3"); self.R(x, yy + 1, w, 1, m + "1")

    def timber(self, x, y, w, h, m):
        """Timber framing: posts, rails and braces in dark oak over the plaster infill."""
        self.plaster(x, y, w, h, m)
        posts = list(range(x, x + w, 16)) + [x + w - 3]
        for px in posts: self.R(px, y, 3, h, "dk3"); self.R(px, y, 1, h, "dk2")
        for ry in range(y, y + h, 22): self.R(x, ry, w, 3, "dk3"); self.R(x, ry, w, 1, "dk2")
        for i, px in enumerate(posts[:-1]):
            for ry in range(y + 3, y + h - 19, 22):
                for k in range(19):
                    bx = px + 3 + (k * 13 // 19 if i % 2 else 12 - k * 13 // 19)
                    if bx < x + w - 3: self.R(bx, ry + k, 2, 1, "dk3")

    def wall(self, kind, x, y, w, h, m):
        if kind == "planks": self.planks(x, y, w, h, m)
        elif kind == "plaster": self.plaster(x, y, w, h, m)
        elif kind == "stucco": self.plaster(x, y, w, h, m, 12)
        elif kind == "timber": self.timber(x, y, w, h, m)
        else: self.bricks(x, y, w, h, m)

    def shine(self, x, y, w, h):
        for i in range(h):
            for dx, ww, a in ((0, 5, .34), (8, 2, .24)):
                sx = int(x + w * .55 - i * .6 + dx)
                a0, a1 = max(x, sx), min(x + w, sx + ww)
                if a1 > a0: self.R(a0, y + i, a1 - a0, 1, "paper0", a)

    def save(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.im.save(path, optimize=True)


# ---- per time of day -------------------------------------------------------------------------------------------
def sky(p):
    c = Canvas(310, 184, p)
    st, n = p["sky"], len(p["sky"]) - 1
    for y in range(184):
        t = min(.9999, y / 176) * n
        i = int(t)
        c.R(0, y, 310, 1, mix(st[i], st[i + 1], t - i))
    if p["phase"] == "night":
        for i in range(110):
            h = hsh("s%d" % i)
            c.R(h % 310, (h >> 8) % 130, 1, 1, "paper0" if i % 7 == 0 else "mid1" if i % 2 else "mid3")
    moon = p["phase"] == "night"
    cx = {"night": 244, "dawn": 62, "dusk": 248}.get(p["phase"], 226)
    cy = {"dusk": 98, "dawn": 88}.get(p["phase"], 46)
    r = 11 if moon else 14
    c.glow(cx, cy, r + 18, "mid1" if moon else "gold1")
    c.disc(cx, cy, r, "paper1" if moon else "gold0")
    if moon:
        c.disc(cx - 3, cy - 3, 3, "paper3"); c.disc(cx + 4, cy + 3, 2, "paper3"); c.disc(cx + 3, cy - 5, 1, "paper3")
    else:
        c.disc(cx - 3, cy - 3, r - 6, "paper0")
    return c


def repeat(c, period=320):
    """A layer drawn in its first `period` columns, copied across: it can scroll by `period` and wrap seamlessly."""
    tile = c.im.crop((0, 0, period, c.im.height))
    for x in range(period, c.im.width, period): c.im.paste(tile, (x, 0))
    return c


def clouds(p):
    c = Canvas(640, 110, p)
    for i in range(3 if p["phase"] == "night" else 6):	# fewer at night, so the moon shows
        h = hsh("c%d" % i)
        x, y, n = i * 54 + h % 20, (12 + (h >> 6) % 30) if p["back"] == "sea" else 22 + (h >> 6) % 54, 3 + h % 3	# over the sky, not the sea
        for role, dy, sh in (("cloud2", 3, 0), ("cloud1", 1, 1), ("cloud0", -1, 2)):
            for k in range(n):
                for off in (0, 320, -320):
                    c.disc(x + k * 11 + off, y + dy - (k % 2) * 3, 7 + ((h >> k) % 6) - sh, role)
    return repeat(c)


def far(p):
    if p["back"] == "sea": return far_sea(p)
    if p["back"] == "towers": return far_towers(p)
    c = Canvas(640, 184, p)
    x, i = 0, 0
    while x < 320:
        h = hsh("b%d" % i)
        w, ht = 20 + h % 30, 70 + (h >> 5) % 60
        w = min(w, 320 - x)
        c.R(x, 184 - ht, w, ht, "far")
        if i % 3 == 0: c.R(x + w // 2, 184 - ht - 10, 1, 10, "far")
        x += w; i += 1
    return repeat(c)


def mid(p):
    if p["back"] == "sea": return mid_masts(p)
    if p["back"] == "towers": return mid_gables(p)
    c = Canvas(640, 184, p)
    x, i = 0, 0
    while x < 320:
        h = hsh("m%d" % i)
        w, ht = 34 + h % 36, 92 + (h >> 5) % 34
        w = min(w, 320 - x)
        top = 184 - ht
        c.R(x, top, w, ht, "far2"); c.R(x, top, w, 1, "far"); c.R(x + w - 3, top, 3, ht, "far3")
        for yy in range(top + 6, 170, 9):
            for xx in range(x + 5, x + w - 7, 8):
                if hsh("%d:%d" % (xx, yy)) % 3 == 0:
                    c.R(xx, yy, 3, 5, "lit1" if p["lights"] else "far")
                    if p["lights"]: c.R(xx, yy, 3, 1, "lit0")
        x += w + (0 if h % 3 else 6); i += 1
    if p["back"] == "trees": cherry_trees(c, p)
    if p["back"] == "palms": palms(c, p)
    return repeat(c)


def far_sea(p):
    """Harbor's far layer: the sea from a high horizon (the street looks down on the water), stepped one scanline band
    at a time with glints, and a lighthouse on a spit."""
    c = Canvas(640, 184, p)
    sea = 62
    for y in range(sea, 184):
        t = (y - sea) / (184 - sea)
        c.R(0, y, 640, 1, mix(mix(p["win"], p["far"], .5), p["dk"], min(1, t * 1.4)))
    c.R(0, sea, 640, 1, "win0")
    for i in range(90):
        k = hsh("w%d" % i)
        y = sea + 3 + (k >> 4) % 110
        c.R(k % 320, y, 3 + k % 6, 1, "win0" if y < sea + 30 else "win2")
    c.R(232, sea - 4, 70, 5, "far3")
    c.R(266, sea - 30, 8, 27, "paper1"); c.R(266, sea - 23, 8, 4, "awnA2"); c.R(266, sea - 12, 8, 4, "awnA2")
    c.R(264, sea - 37, 12, 7, "ink"); c.R(266, sea - 35, 8, 4, "lit0" if p["lights"] else "paper1"); c.R(267, sea - 40, 6, 3, "ink")
    if p["lights"]: c.glow(270, sea - 33, 18, "lit1")
    return repeat(c)


def far_towers(p):
    """Old Town's far layer: a castle on the hill, towers with crenellations and pointed roofs."""
    c = Canvas(640, 184, p)
    c.R(0, 120, 320, 64, "far")
    for k in range(16):
        c.disc(k * 22 + 8, 124 - (hsh("h%d" % k) % 9), 16, "far")
    roof = mix(p["far2"], p["awnA"], .3)
    for tx, tw, th, cone in ((36, 18, 84, True), (60, 46, 54, False), (110, 14, 104, True), (128, 30, 60, False), (162, 16, 76, True),
                              (230, 22, 68, True), (256, 34, 46, False)):
        top = 124 - th
        c.R(tx, top, tw, th, "far"); c.R(tx + tw - 2, top, 2, th, "far2")
        if cone:
            for k in range(tw // 2 + 2):
                c.R(tx - 2 + k, top - 2 * k, tw + 4 - 2 * k, 2, roof)
            c.R(tx + tw // 2, top - tw - 8, 1, 6, "far2")
        else:
            for kx in range(tx, tx + tw, 6): c.R(kx, top - 4, 3, 4, "far")
        for wy in range(top + 10, 116, 16):
            c.R(tx + tw // 2 - 1, wy, 2, 4, "lit1" if p["lights"] else "far2")
    return repeat(c)


def mid_gables(p):
    """Old Town's town houses behind the shops: steep roofs in tile, a lit dormer each."""
    c = Canvas(640, 184, p)
    x, i = 0, 0
    roof, roof2 = mix(p["far2"], p["awnA"], .45), mix(p["far3"], p["awnA"], .3)
    while x < 320:
        h = hsh("g%d" % i)
        w, ht = 26 + h % 24, 52 + (h >> 5) % 22
        w = min(w, 320 - x)
        top = 184 - ht
        c.R(x, top, w, ht, "far2"); c.R(x + w - 3, top, 3, ht, "far3")
        half = w // 2 + 1
        for k in range(half):
            c.R(x - 1 + k, top - (k + 1) * 2, w + 2 - 2 * k, 2, roof2 if k == 0 or k % 3 == 0 else roof)
        c.R(x + w // 2 - 2, top - w // 2 + 4, 4, 5, "lit1" if p["lights"] else "far3")
        for yy in range(top + 8, 170, 12):
            for xx in range(x + 5, x + w - 6, 9):
                if hsh("%d;%d" % (xx, yy)) % 3 == 0: c.R(xx, yy, 3, 5, "lit1" if p["lights"] else "far")
        x += w + 3; i += 1
    return repeat(c)


def mid_masts(p):
    """Harbor's middle layer: moored boats' masts and furled sails above the quay, buoys between."""
    c = Canvas(640, 184, p)
    c.R(0, 150, 320, 34, "far3"); c.R(0, 150, 320, 1, "far2")
    for i in range(8):
        h = hsh("bt%d" % i)
        bx, mh = i * 40 + h % 14, 70 + (h >> 4) % 50
        hull = 150 - 6
        for k in range(6): c.R(bx - 14 + k, hull + k, 30 - 2 * k, 1, "far2" if k else "paper2")
        c.R(bx, hull - mh, 2, mh, "far2"); c.R(bx - 10, hull - mh + 14, 22, 1, "far2")
        sail = (mh - 20) // 2	# furled: half the mast
        for k in range(sail):
            c.R(bx + 2, hull - mh + 16 + k, max(1, (sail - k) // 3), 1, "paper2" if k % 9 else "paper3")
        if h % 3 == 0: c.R(bx - 1, hull - mh - 4, 4, 4, "awnA2")
        if p["lights"]: c.R(bx, hull - mh - 1, 2, 2, "lit0")
    return repeat(c)


def cherry_trees(c, p):
    """Blossom Row: cherry trees along the street behind the shops, wide clumped canopies, petals drifting."""
    for i in range(6):
        h = hsh("t%d" % i)
        tx, ty = i * 54 + h % 20, 70 + (h >> 5) % 24
        for off in (0, 320, -320):
            x0 = tx + off
            c.R(x0 - 2, ty, 5, 184 - ty, "dk3"); c.R(x0 - 2, ty, 1, 184 - ty, "dk2")
            c.R(x0 - 9, ty + 4, 8, 2, "dk3"); c.R(x0 + 2, ty + 1, 9, 2, "dk3")
            clumps = [(hsh("tc%d:%d" % (i, k)) % 50 - 25, -(hsh("td%d:%d" % (i, k)) % 26), 9 + hsh("tr%d:%d" % (i, k)) % 7) for k in range(11)]
            for dx, dy, r in clumps: c.disc(x0 + dx, ty + dy, r, "gold4")
            for dx, dy, r in clumps: c.disc(x0 + dx, ty + dy - 1, r - 1, "gold3")
            for dx, dy, r in clumps: c.disc(x0 + dx - 1, ty + dy - 3, r - 3, "gold2")
            for dx, dy, r in clumps: c.disc(x0 + dx - 2, ty + dy - 5, max(2, r - 7), "gold1")
    for i in range(80):
        k = hsh("pt%d" % i)
        c.R(k % 320, 30 + (k >> 9) % 150, 2, 1, "gold0")


def palms(c, p):
    """Sundown Strip: palm silhouettes against the low sun, fronds drooping from the crown."""
    for i in range(5):
        h = hsh("pa%d" % i)
        tx, ty, side = i * 64 + h % 24, 34 + (h >> 5) % 26, 1 if i % 2 else -1
        for off in (0, 320, -320):
            x0 = tx + off
            for y in range(ty, 184):
                lean = int((184 - y) ** 1.3 * .02) * side
                c.R(x0 + lean - 1, y, 3, 1, "ink")
                if y % 6 == 0: c.R(x0 + lean - 2, y, 5, 1, "ink")
            cx = x0 + int((184 - ty) ** 1.3 * .02) * side
            for a in (-170, -140, -110, -70, -40, -10, 200, -90):
                ang = math.radians(a)
                n = 22 if a not in (-90,) else 10
                for r in range(n):
                    px = cx + math.cos(ang) * r
                    py = ty + math.sin(ang) * r * .7 + (r * r) / 28
                    c.R(int(px), int(py), 2, 2, "ink")
                    if r > 4 and r % 3 == 0: c.R(int(px), int(py) + 2, 1, 2 + r // 8, "ink")
            c.disc(cx, ty, 3, "ink")


BANDS = [(7, 7), (10, 9), (13, 12), (17, 15)]     # (height, cobble half-width): wider toward the viewer


def band(p, i):
    h, bh = BANDS[i]
    bw = bh * 2
    tot = bw * math.ceil(384 / bw)
    c = Canvas(tot, h, p)
    c.R(0, 0, tot, h, "road4")
    for k, x in enumerate(range(0, tot, bw)):
        m = ("road1", "road2", "road3")[hsh("%db%d" % (i, k)) % 3]
        c.R(x + 1, 1, bw - 2, h - 2, m); c.R(x + 1, 1, bw - 2, 1, "road0"); c.R(x + 1, h - 2, bw - 2, 1, "road4")
    if i == 2: c.R(0, 5, tot, 2, "mid0"); c.R(0, 7, tot, 1, "ink")
    return c


HEIGHTS = [112, 128, 104, 120]


def shop(p, t, awning):
    """A shopfront in item coordinates shifted by (8, 12): the theme draws it at x - 8, y 12; the base is y 166."""
    c = Canvas(112, 170, p)
    x, base, w = 8, 166, 96
    h = HEIGHTS[t]
    top = base - h
    wall, w2 = ("wallB", "wallA") if t % 2 else ("wallA", "wallB")
    if t == 1:
        c.R(x + 18, top - 15, 60, 16, "ink"); c.wall(p["wall"], x + 19, top - 14, 58, 15, w2)
        c.R(x + 33, top - 25, 30, 11, "ink"); c.wall(p["wall"], x + 34, top - 24, 28, 10, w2)
        c.disc(x + 48, top - 7, 5, "ink"); c.disc(x + 48, top - 7, 4, "lit1"); c.R(x + 47, top - 11, 2, 8, "ink"); c.R(x + 44, top - 8, 8, 2, "ink")
    if t == 2:
        for i in range(7):
            yy, xx, ww = top - 3 - i * 3, x - 2 + i * 7, w + 4 - i * 14
            c.R(xx - 1, yy - 1, ww + 2, 5, "ink")
            for k in range(0, ww, 6):
                kw = min(5, ww - k)
                c.R(xx + k, yy, kw, 3, "awnC2"); c.R(xx + k, yy, kw, 1, "awnC0"); c.R(xx + k, yy + 2, kw, 1, "awnC3")
    if t == 3:
        c.R(x + 10, top - 13, 76, 15, "ink"); c.shade(x + 11, top - 12, 74, 13, "awnC")
        for i in range(9):
            bx = x + 15 + i * 8
            c.disc(bx + 1, top - 6, 2, "ink"); c.disc(bx + 1, top - 6, 1, "gold0" if (p["lights"] or i % 2) else "gold3")
    c.R(x, top, w, h, "ink"); c.wall(p["wall"], x + 1, top + 1, w - 2, h - 2, wall)
    c.R(x + w - 14, top + 1, 13, h - 2, "shadow", .28); c.R(x + w - 5, top + 1, 4, h - 2, "shadow", .3)
    if t == 0:
        c.R(x - 3, top - 6, w + 6, 8, "ink"); c.shade(x - 2, top - 5, w + 4, 6, w2)
        for i in range(24): c.R(x + i * 4, top + 1, 2, 2, w2 + "3")
    # the sign plate (the theme writes the name on it; the selected shop's is gold, signsel.png)
    sy = top + 8
    c.R(x + 4, sy - 1, w - 8, 26, "ink"); c.vgrad(x + 5, sy, w - 10, 24, "paper0", "paper3")
    c.R(x + 5, sy + 24, w - 10, 3, "shadow", .4)
    upper = base - 70 - (sy + 28)
    if upper > 14:
        for wx in (x + 13, x + 40, x + 67):
            hh = min(upper - 3, 21)
            c.R(wx - 1, sy + 28, 18, hh + 2, "ink"); c.shade(wx, sy + 29, 16, hh, "paper")
            c.vgrad(wx + 2, sy + 31, 12, hh - 4, "lit0", "lit3"); c.R(wx + 7, sy + 31, 2, hh - 4, "paper2"); c.shine(wx + 2, sy + 31, 12, hh - 4)
            c.R(wx - 2, sy + 29 + hh + 1, 20, 2, "ink"); c.R(wx - 1, sy + 29 + hh + 1, 18, 1, "paper1")
    # the ground floor: the display window (the theme puts the system's icon and shine.png in it) and the door
    c.R(x + 3, base - 59, w - 6, 59, "ink"); c.shade(x + 4, base - 58, w - 8, 58, "dk")
    c.R(x + 6, base - 53, 46, 47, "ink"); c.shade(x + 7, base - 52, 44, 45, "paper"); c.vgrad(x + 9, base - 50, 40, 41, "lit0", "lit3")
    c.R(x + 57, base - 53, 34, 53, "ink"); c.shade(x + 58, base - 52, 32, 52, "wallB")
    c.R(x + 62, base - 48, 24, 20, "ink"); c.vgrad(x + 63, base - 47, 22, 18, "lit0", "lit3"); c.shine(x + 63, base - 47, 22, 18)
    c.R(x + 62, base - 25, 24, 20, "wallB4"); c.shade(x + 63, base - 24, 22, 18, "wallB")
    c.R(x + 84, base - 29, 3, 3, "gold0"); c.R(x + 85, base - 28, 1, 1, "goldLo")
    # the awning
    ac = ("awnA", "awnB", "awnC")[awning]
    c.R(x - 1, base - 69, w + 2, 2, "ink")
    for i in range(12):
        m = "paper" if i % 2 else ac
        for r in range(11): c.R(x + i * 8, base - 67 + r, 8, 1, m + ("0" if r < 2 else "1" if r < 6 else "2" if r < 9 else "3"))
        c.R(x + i * 8 + 1, base - 56, 6, 2, m + "3"); c.R(x + i * 8 + 2, base - 54, 4, 1, "ink")
        c.R(x + i * 8, base - 56, 1, 2, "ink"); c.R(x + i * 8 + 7, base - 56, 1, 2, "ink")
    c.R(x + 4, base - 53, w - 8, 6, "shadow", .45)
    if t != 1:
        cx = x + 60
        c.R(cx - 49, base - 5, 12, 5, "ink"); c.shade(cx - 48, base - 4, 10, 4, "wallB"); c.disc(cx - 44, base - 7, 2, "gold1")
    return c


def signsel(p):
    c = Canvas(86, 24, p)
    c.vgrad(0, 0, 86, 24, "gold0", "gold2"); c.R(0, 0, 86, 1, "paper0")
    return c


def shine(p):
    c = Canvas(40, 41, p)
    c.shine(0, 0, 40, 41)
    return c


def closed(p):
    """The roller shutter over the ground floor of a shop with no games (item coordinates 5, 123)."""
    c = Canvas(86, 55, p)
    for i, y in enumerate(range(0, 55, 5)):
        c.R(0, y, 86, 5, "road4"); c.R(0, y, 86, 1, "mid0"); c.R(0, y + 1, 86, 3, "mid2"); c.R(0, y + 3, 86, 1, "mid3")
    c.R(0, 0, 86, 55, "shadow", .2)
    return c


def lamp(p):
    """The street lamp between two shops: 24 x 112, the post at x 11 (the theme draws it at item x 88, y 66)."""
    c = Canvas(24, 112, p)
    if p["lights"]: c.glow(12, 26, 22, "lit1")
    x, b = 11, 112
    c.R(x - 1, b - 82, 4, 82, "ink"); c.R(x, b - 82, 1, 82, "mid1"); c.R(x + 1, b - 82, 1, 82, "mid3")
    c.R(x - 3, b - 91, 8, 10, "ink"); c.R(x - 2, b - 90, 6, 8, "lit0" if p["lights"] else "mid1"); c.R(x - 2, b - 90, 6, 1, "paper0")
    c.R(x - 4, b - 92, 10, 2, "ink")
    return c


# The walker's poses: (body up, glove offsets, leg 1 and leg 2 as (sideways, lift)). 0 stands; 1-4 are the walk
# (contact, passing, the other contact, passing), one cycle a shop; 5 is the idle bob, the body a pixel up.
POSES = [
    (0, (0, 0), ((0, 0), (0, 0))),
    (0, (1, -1), ((-2, 0), (2, 0))),
    (1, (0, 0), ((0, 0), (0, 1))),
    (0, (-1, 1), ((1, 1), (-1, 0))),
    (1, (0, 0), ((0, 1), (0, 0))),
    (1, (0, 0), ((0, 0), (0, 0))),
]


def mascot(p, pose=0, left=False):
    """The walker: a banana with a face, gloves and red shoes (original art), 28 x 41, facing right; left is the
    same sprite flipped, as the Super NES flipped its sprites."""
    up, gloves, legs = POSES[pose]
    c = mascot_body(p, up, gloves, legs)
    if left: c.im = c.im.transpose(Image.FLIP_LEFT_RIGHT)
    return c


def mascot_body(p, up, gloves, legs):
    Wm, Hm = 28, 31
    g = [[-1] * Wm for _ in range(Hm)]
    for j in range(Hm):
        t = j / (Hm - 1)
        cx = 10 + 6.5 * math.sin(math.pi * t)
        hw = 1.6 if j < 4 else 1.5 + 7.4 * math.sin(math.pi * min(1, (t - .08) / .94)) ** .55 if t > .08 else 1.5
        for x in range(Wm):
            d = (x + .5 - cx) / hw
            if abs(d) > 1: continue
            g[j][x] = 5 if j < 4 else 0 if d < -.6 else 1 if d < -.12 else 2 if d < .42 else 3 if d < .78 else 4
    c = Canvas(28, 41, p)	# a row of headroom for the poses a pixel up
    ramp = ["ban0", "ban1", "ban2", "#d08a14", "ban4", "dk2"]
    # the legs first, so the body's outline sits over their tops; the feet stay on the ground unless lifted
    for (lx, (dx, lift)) in zip((10, 17), legs):
        fx, fy = lx + dx, 36 - lift
        top = 31 - up
        c.R(lx, top, 3, fy + 1 - top, "ink"); c.R(lx + 1, top, 1, fy - top, "dk1")
        if dx: c.R(min(lx, fx), fy - 2, abs(dx) + 3, 3, "ink"); c.R(min(lx, fx) + 1, fy - 2, abs(dx) + 1, 2, "dk1")
        c.R(fx - 2, fy, 8, 4, "ink"); c.R(fx - 1, fy + 1, 6, 2, "awnA1"); c.R(fx - 1, fy + 1, 3, 1, "awnA0")
    y0 = 1 - up
    for j in range(Hm):
        for x in range(Wm):
            if g[j][x] >= 0: c.R(x, j + y0, 1, 1, ramp[g[j][x]])
            elif any(0 <= x + dx < Wm and 0 <= j + dy < Hm and g[j + dy][x + dx] >= 0 for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                c.R(x, j + y0, 1, 1, "ink")
    ex = 13
    for o in (0, 6):
        c.R(ex + o, 11 + y0, 4, 6, "ink"); c.R(ex + o, 12 + y0, 3, 4, "paper0"); c.R(ex + o + 1, 13 + y0, 2, 3, "ink"); c.R(ex + o + 1, 13 + y0, 1, 1, "paper0")
    c.R(ex + 2, 20 + y0, 6, 1, "ink"); c.R(ex + 1, 19 + y0, 1, 1, "ink"); c.R(ex + 8, 19 + y0, 1, 1, "ink"); c.R(ex + 3, 21 + y0, 4, 1, "awnA1")
    c.R(ex - 3, 18 + y0, 3, 2, "awnA1", .6); c.R(ex + 10, 18 + y0, 2, 2, "awnA1", .6)
    for (ax, ay), gy in zip(((4, 20), (24, 20)), gloves):
        ay += y0 + gy
        c.R(ax - 1, ay - 1, 5, 5, "ink"); c.R(ax, ay, 3, 3, "paper0"); c.R(ax + 2, ay + 2, 1, 1, "paper3")
    return c


# ---- per palette: the interface ----------------------------------------------------------------------------------
def win(c, x, y, w, h, alpha=.92):
    c.vgrad(x + 2, y + 2, w - 4, h - 4, "hudA", "hudB", alpha)
    c.R(x + 1, y, w - 2, 1, "ink"); c.R(x + 1, y + h - 1, w - 2, 1, "ink"); c.R(x, y + 1, 1, h - 2, "ink"); c.R(x + w - 1, y + 1, 1, h - 2, "ink")
    c.R(x + 1, y + 1, w - 2, 1, "gold0"); c.R(x + 1, y + 2, 1, h - 3, "gold1"); c.R(x + 2, y + h - 2, w - 3, 1, "gold3"); c.R(x + w - 2, y + 2, 1, h - 4, "gold3")
    for a, b in ((x + 3, y + 3), (x + w - 4, y + 3), (x + 3, y + h - 4), (x + w - 4, y + h - 4)): c.R(a, b, 1, 1, "gold0")


def panel(p, w, h):
    c = Canvas(w, h, p)
    win(c, 0, 0, w, h)
    return c


def interior(p):
    c = Canvas(310, 270, p)
    c.vgrad(0, 0, 310, 250, "wallB1", "wallB4")
    for y in range(24, 240, 12):
        for x in range(0 if y % 24 else 8, 310, 16):
            c.R(x + 3, y, 2, 2, "wallB0"); c.R(x + 2, y + 2, 4, 1, "wallB3")
    c.R(0, 240, 310, 10, "dk3"); c.R(0, 240, 310, 1, "dk0")
    win(c, 0, 250, 310, 20, 1)
    win(c, 0, 0, 310, 22, 1)
    # the counter's box frame and the receipt
    c.R(13, 33, 86, 108, "shadow", .5); c.R(10, 30, 86, 108, "ink")
    c.R(11, 147, 90, 92, "shadow", .5); c.vgrad(8, 144, 90, 92, "paper0", "paper2")
    for i in range(0, 90, 4): c.R(8 + i, 236, 2, 2, "paper2")
    for i in range(6, 84, 3): c.R(8 + i, 162, 2, 1, "ink")
    # the shelf's planks, one under each row (18 high, 2 apart, from y 27)
    for k in range(11):
        y = 27 + k * 20 + 17
        c.R(104, y, 202, 3, "dk4"); c.R(104, y, 202, 1, "dk1")
    return c


def workshop(p):
    """Settings in the SNES style: a dark wall of pegboard, the header and the button bar."""
    c = Canvas(310, 270, p)
    c.vgrad(0, 0, 310, 250, "dk1", "dk4")
    for y in range(26, 250, 10):
        for x in range(6 + (5 if y % 20 else 0), 310, 10): c.R(x, y, 2, 2, "dk4"); c.R(x, y, 1, 1, "ink")
    win(c, 0, 0, 310, 22, 1)
    win(c, 0, 250, 310, 20, 1)
    return c


def drawer(p, on):
    """A settings group: a drawer front with its handle; the open one longer and gold."""
    w, h, m = (124 if on else 112), 26, ("gold" if on else "wallA")
    c = Canvas(w + 3, h + 3, p)
    c.R(3, 3, w, h, "shadow", .5)
    c.R(0, 0, w + 1, h, "ink")
    for r in range(h - 2): c.R(0, 1 + r, w, 1, m + ("0" if r < 2 else "1" if r < 11 else "2" if r < 19 else "3"))
    if not on:
        for k in (4, 21): c.R(0, k, w, 1, m + "3")
    c.R(w - 40, h - 7, 28, 3, "ink"); c.R(w - 39, h - 7, 26, 1, "paper0")
    return c


def sheet(p):
    """The open group's rows: a sheet of paper on the pegboard."""
    c = Canvas(174, 224, p)
    c.R(3, 3, 170, 220, "shadow", .5)
    c.vgrad(0, 0, 170, 220, "paper0", "paper2")
    for i in range(8, 162, 3): c.R(i, 19, 2, 1, "ink")
    for i in range(8, 162, 3): c.R(i, 183, 2, 1, "dk2")
    return c


def book(p):
    """Search and typing in the SNES style: the phone book, open on the pegboard; a red cover round a ruled page."""
    w, h = 302, 224
    c = Canvas(w + 3, h + 3, p)
    c.R(3, 3, w, h, "shadow", .5)
    c.R(0, 0, w, h, "ink")
    for r in range(h - 2): c.R(1, 1 + r, w - 2, 1, "awnA" + ("1" if r < 2 else "2" if r < h - 6 else "3"))
    c.R(1, 1, w - 2, 1, "awnA0")
    c.vgrad(5, 4, w - 10, h - 9, "paper0", "paper2")
    c.R(5, h - 5, w - 10, 1, "paper3")
    for y in range(32, h - 8, 26): c.R(8, y, w - 16, 1, "paper2")
    for i in range(8, w - 8, 3): c.R(i, 27, 2, 1, "ink")
    return c


def ticket(p):
    """The pause menu in the SNES style: a ticket. The stub's rows on the left, a perforation, the picture on the right;
    a red band with the game across the top and a dark one for the buttons along the bottom."""
    w, h, cut = 302, 262, 184
    c = Canvas(w, h, p)
    c.R(0, 0, w, h, "ink")
    c.vgrad(1, 1, w - 2, h - 2, "paper0", "paper2")
    for r in range(22): c.R(1, 1 + r, w - 2, 1, "awnA" + ("0" if r < 1 else "1" if r < 3 else "2" if r < 19 else "3"))
    c.R(1, 23, w - 2, 1, "ink")
    c.R(1, h - 22, w - 2, 1, "ink")
    c.vgrad(1, h - 21, w - 2, 20, "dk2", "dk4")
    for i in range(4, w - 4, 4): c.R(i, 26, 2, 1, "awnA2"); c.R(i, h - 25, 2, 1, "awnA2")
    for y in range(30, h - 28, 4): c.R(cut, y, 1, 2, "dk2")
    for k in range(4):	# the punched notches where the perforation meets the edges
        hw = (4, 4, 3, 2)[k]
        c.R(cut - hw + 1, k, hw * 2 - 1, 1, "ink"); c.R(cut - hw + 1, h - 1 - k, hw * 2 - 1, 1, "ink")
    # the picture's frame
    c.R(191, 31, 104, 80, "ink"); c.R(192, 32, 102, 78, "dk3")
    return c


def row(p, sel):
    w, h = (198, 17) if sel else (192, 15)
    c = Canvas(w, h, p)
    c.R(0, 0, w, h, "ink")
    if sel: c.vgrad(1, 1, w - 2, h - 2, "gold0", "gold2"); c.R(1, 1, w - 2, 1, "paper0")
    else: c.vgrad(1, 1, w - 2, h - 2, "dk1", "dk3")
    return c


def theme_text(root, name):
    """A neighborhood's theme: street.json with its name, art folder and colours (and Sundown Strip's fixed dusk, Old
    Town's icon set). street.json is the one to edit; these are rewritten from it."""
    import json, re
    hood, base = HOODS[name], PALETTES[name]
    s = open(os.path.join(root, "street.json"), encoding="utf-8").read()
    p = palette(base, (hood["phases"] or ["day"])[0])
    v = {"ink": base["ink"], "dk": base["dk"], "paper": base["paper"], "paperLo": mix(base["paper"], base["mid"], .45),
         "awnA": base["awnA"], "awnB": base["awnB"], "awnC": base["awnC"], "gold": base["gold"], "goldLo": base["goldLo"],
         "hudLo": mix(base["ink"], "#000000", .35), "goldTop": mix(base["gold"], "#ffffff", .75),
         "goldHi": mix(base["gold"], "#ffffff", .4), "goldDeep": mix(base["goldLo"], base["ink"], .45)}
    for k, c in v.items():
        s, n = re.subn(r'("%s": )"#[0-9a-fA-F]{6}"' % k, r'\1"%s"' % hexc(q15(c)), s, count=1)
        assert n == 1, k
    s = s.replace('"accent": "#ffd21e"', '"accent": "%s"' % hexc(q15(base["gold"])))
    s = s.replace('"street/banana"', '"street/%s"' % name)
    s = s.replace('"name": "Banana Street"', '"name": "%s"' % hood["name"])
    if hood["phases"]: s = s.replace("{phase}", hood["phases"][0])
    if hood.get("icons"):
        icons = re.search(r'\n  "icons": \{.*?\n  \},?\n', open(os.path.join(root, hood["icons"]), encoding="utf-8").read(), re.S).group(0)
        s = s.replace('\n  "vars": {', icons.rstrip(",\n") + ",\n\n  \"vars\": {", 1)
    s = s.replace("{\n", "{\n  // %s: generated by tools/gen-street.py from street.json (edit that one, then run it again).\n" % hood["name"], 1)
    return s


def write_theme(root, name):
    p = os.path.join(root, "street-%s.json" % name)
    open(p, "w", encoding="utf-8").write(theme_text(root, name))
    return p


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "system", "rootfs", "usr", "share", "banana", "themes")
    n = 0
    for name, base in PALETTES.items():
        hood = HOODS[name]
        out = os.path.join(root, "street", name)
        for ph in hood["phases"] or PHASES:
            p = palette(base, ph)
            p.update(wall=hood["wall"], back=hood["back"])
            d = os.path.join(out, ph)
            files = {"sky": sky(p), "clouds": clouds(p), "far": far(p), "mid": mid(p), "signsel": signsel(p), "shine": shine(p),
                     "closed": closed(p), "lamp": lamp(p)}
            for k in range(len(POSES)):
                files["mascot-r%d" % k], files["mascot-l%d" % k] = mascot(p, k), mascot(p, k, True)
            for i in range(4): files["band%d" % i] = band(p, i)
            for t in range(4):
                for a in range(3): files["shop%d%d" % (t, a)] = shop(p, t, a)
            for k, c in files.items(): c.save(os.path.join(d, k + ".png")); n += 1
        p = palette(base, (hood["phases"] or ["day"])[0])
        p.update(wall=hood["wall"], back=hood["back"])
        for k, c in {"workshop": workshop(p), "drawer": drawer(p, False), "drawer-on": drawer(p, True), "sheet": sheet(p),
                     "book": book(p), "ticket": ticket(p),
                     "hud": panel(p, 310, 46), "chip52": panel(p, 52, 14), "chip34": panel(p, 34, 14), "interior": interior(p),
                     "row": row(p, False), "rowsel": row(p, True)}.items():
            c.save(os.path.join(out, k + ".png")); n += 1
        if name != "banana": write_theme(root, name)
    print("gen-street: %d images in %s, %d neighborhood themes" % (n, os.path.join(root, "street"), len(PALETTES) - 1))


if __name__ == "__main__":
    main()
