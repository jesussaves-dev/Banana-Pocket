#!/usr/bin/env python3
"""The README's pictures, in the menu's own style: the banner (Banana Street with the title in Banana's title face),
the download button, and real renders of the menu from the headless build (the screens, a walk-and-launch GIF, the
street at four times of day, the five neighborhoods).

  tools/gen-readme-art.py        writes docs/readme/*.png and street.gif

Needs the host build and the test library: `make test` first (ui/banana-ui and ui/tests/out/fixture). The art comes
from the baked street (tools/gen-street.py) and the menu itself, so the pictures follow the UI when it changes.
"""
import os, re, shutil, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THEMES = os.path.join(HERE, "system", "rootfs", "usr", "share", "banana", "themes")
ART = os.path.join(THEMES, "street", "banana")
FONTS = os.path.join(HERE, "ui", "assets", "banana", "fonts")
ICONS = os.path.join(HERE, "ui", "assets", "rnds", "icons")
OUT = os.path.join(HERE, "docs", "readme")
UI = os.path.join(HERE, "ui", "banana-ui")
FX = os.path.join(HERE, "ui", "tests", "out", "fixture")


def hexc(h): return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


VARS = {"ink": "#2b1a33", "paper": "#fff3cf", "hudLo": "#1a1230", "gold": "#ffd21e", "goldLo": "#d08a14",
        "goldTop": "#fff6c8", "goldHi": "#ffe873", "goldDeep": "#8a4a10", "awnB": "#3fa7a0"}
C = {k: hexc(v) for k, v in VARS.items()}


def img(path): return Image.open(path).convert("RGBA")


def font(name, size):
    f = ImageFont.truetype(os.path.join(FONTS, name), size)
    return f


def text_mask(t, f):
    """t as a hard-edged 1-bit mask (no anti-aliasing: the faces are drawn on the pixel grid)."""
    l, top, r, b = f.getbbox(t)
    m = Image.new("L", (r + 2, b + 2), 0)
    d = ImageDraw.Draw(m)
    d.fontmode = "1"
    d.text((0, 0), t, font=f, fill=255)
    return m


def paste_text(base, t, f, x, y, color, shadow=None, outline=None, stops=None):
    m = text_mask(t, f)
    if outline:
        layer = Image.new("RGBA", m.size, outline + (255,))
        for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (1, 1), (-1, 1), (1, -1)):
            base.paste(layer, (x + dx, y + dy), m)
    if shadow:
        base.paste(Image.new("RGBA", m.size, shadow + (255,)), (x + 1, y + 1), m)
    fill = Image.new("RGBA", m.size, color + (255,))
    if stops:	# the title's gold: a band of colour per row, top to bottom of the line, as the menu draws it
        _, t0, _, b0 = m.getbbox() or (0, 0, 0, m.height)
        d = ImageDraw.Draw(fill)
        for yy in range(t0, b0):
            k = min(len(stops) - 1, (yy - t0) * len(stops) // max(1, b0 - t0))
            d.line([(0, yy), (m.width, yy)], fill=stops[k] + (255,))
    base.paste(fill, (x, y), m)
    return m.getbbox()[2] if m.getbbox() else 0


def scale(im, k): return im.resize((im.width * k, im.height * k), Image.NEAREST)


# ---- the banner: the street, wider than the menu's, with the title over its sky ------------------------------------
def banner(phase="day"):
    W, H = 480, 226
    d = os.path.join(ART, phase)
    b = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    sky = img(os.path.join(d, "sky.png"))
    for x in range(0, W - sky.width + 1): b.paste(sky.crop((0, 0, 1, sky.height)), (x, 0))
    b.alpha_composite(sky, (W - sky.width, 0))	# the sun on the right, over the same stepped gradient
    for name, dx in (("clouds", 30), ("far", 60), ("mid", 140)):
        layer = img(os.path.join(d, name + ".png"))
        b.alpha_composite(layer.crop((dx, 0, dx + W, layer.height)), (0, 0))
    small = font("BananaPixel.ttf", 12)
    shops = [("gba", "Game Boy Advance", 152), ("snes", "Super Nintendo", 86), ("md", "Mega Drive", 64)]
    for k, (icon, name, n) in enumerate(shops):
        x, t, a = 196 + k * 104, (k + 1) % 4, k % 3
        b.alpha_composite(img(os.path.join(d, "shop%d%d.png" % (t, a))), (x - 8, 12))
        sy = (74, 58, 82, 66)[t]
        if k == 1: b.alpha_composite(img(os.path.join(d, "signsel.png")), (x + 5, sy))
        lines = [name] if small.getlength(name) <= 82 else name.split(" ", 1)
        for li, line in enumerate(lines):
            w = int(small.getlength(line))
            paste_text(b, line, small, x + 6 + (84 - w) // 2, sy + (6 if len(lines) == 1 else 1 + li * 10) - 1, C["ink"])
        ic = img(os.path.join(ICONS, icon + ".png")).resize((32, 32), Image.NEAREST)
        b.alpha_composite(ic, (x + 13, 132))
        b.alpha_composite(img(os.path.join(d, "shine.png")), (x + 9, 128))
        dr = ImageDraw.Draw(b)
        dr.rectangle([x + 24, 157, x + 50, 168], fill=C["paper"] + (255,), outline=C["ink"] + (255,))
        s = str(n)
        paste_text(b, s, small, x + 24 + (27 - int(small.getlength(s))) // 2, 157, C["ink"])
        b.alpha_composite(img(os.path.join(d, "lamp.png")), (x + 88, 66))
    dr = ImageDraw.Draw(b)
    dr.line([(0, 178), (W, 178)], fill=C["paper"] + (255,))
    for n, top, bh in ((0, 179, 7), (1, 186, 10), (2, 196, 13), (3, 209, 17)):
        band = img(os.path.join(d, "band%d.png" % n))
        for x in range(0, W, band.width): b.alpha_composite(band, (x, top))
    b.alpha_composite(img(os.path.join(d, "mascot-r0.png")), (196 + 104 + 60, 147))
    # the title, as the status panel draws a system's name: gold in eight bands, an outline, a pixel shadow
    # (at 48, twice the menu's 24: the face is drawn on the pixel grid, so it stays hard-edged at a whole multiple)
    title = font("BananaTitle.ttf", 48)
    stops = [C["goldTop"], C["goldTop"], C["goldHi"], C["gold"], C["gold"], C["goldLo"], C["goldDeep"], C["goldDeep"]]
    paste_text(b, "Banana", title, 12, 14, C["gold"], shadow=C["hudLo"], outline=C["hudLo"], stops=stops)
    paste_text(b, "Pocket", title, 12, 60, C["gold"], shadow=C["hudLo"], outline=C["hudLo"], stops=stops)
    for i, line in enumerate(("A tiny OS for the", "Retroid Pocket Classic")):
        paste_text(b, line, small, 14, 118 + i * 12, C["paper"], shadow=C["hudLo"])
    return b


def button(label, key="A"):
    """A button and what it does, the way the menu's chips are drawn: the key on paper, the word beside it, on the
    status panel's gold-edged plate."""
    f = font("BananaPixel.ttf", 12)
    kw = int(f.getlength(key)) + 6
    lw = int(f.getlength(label))
    W, H = kw + lw + 22, 22
    panel = img(os.path.join(ART, "hud.png"))
    b = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    # the hud plate's frame, stretched by its edges (corners and a middle slice)
    b.paste(panel.crop((0, 0, 6, 46)).resize((6, H)), (0, 0))
    mid = panel.crop((6, 0, 304, 46)).resize((W - 12, H))
    b.paste(mid, (6, 0))
    b.paste(panel.crop((304, 0, 310, 46)).resize((6, H)), (W - 6, 0))
    dr = ImageDraw.Draw(b)
    dr.rectangle([7, 4, 7 + kw - 1, 4 + 13], fill=C["ink"] + (255,))
    dr.rectangle([8, 5, 8 + kw - 3, 4 + 12], fill=C["paper"] + (255,))
    paste_text(b, key, f, 10, 4, C["ink"])
    paste_text(b, label, f, 7 + kw + 4, 5, C["gold"], shadow=C["hudLo"])
    return b


# ---- the menu itself, headless ----------------------------------------------------------------------------------
def headless(theme, script, phase="day", prefix="r-"):
    tmp = tempfile.mkdtemp(prefix="readme-")
    data = os.path.join(tmp, "data")
    shutil.copytree(os.path.join(FX, "data"), data)
    conf = os.path.join(data, "settings.conf")
    s = open(conf).read()
    s = re.sub(r"(?m)^ *theme *=.*$", "theme = file:" + os.path.join(THEMES, theme), s) if re.search(r"(?m)^ *theme *=", s) else s + "\ntheme = file:" + os.path.join(THEMES, theme) + "\n"
    s = re.sub(r"(?m)^ *clock *=.*$", "clock = 12h", s) if re.search(r"(?m)^ *clock *=", s) else s + "clock = 12h\n"
    open(conf, "w").write(s)
    env = dict(os.environ, BANANA_PHASE=phase)
    subprocess.run([UI, "--headless", os.path.join(tmp, prefix), "--size", "1240x1080", "--assets", os.path.join(HERE, "ui", "assets", "rnds"),
                    "--banana", os.path.join(HERE, "ui", "assets", "banana"), "--systems", os.path.join(HERE, "ui", "tests", "systems.conf"),
                    "--roms", os.path.join(FX, "roms"), "--data", data, "--script", script],
                   check=True, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return tmp


def shot(tmp, name, k=2):
    """a render at the panel's 4 device px a unit, brought to k px a unit (nearest: every art pixel stays square)"""
    im = Image.open(os.path.join(tmp, "r-" + name + ".png")).convert("RGB")
    return im.resize((310 * k, 270 * k), Image.NEAREST)


def frame(im, pad=6, edge=C["ink"]):
    out = Image.new("RGB", (im.width + pad * 2, im.height + pad * 2), edge)
    out.paste(im, (pad, pad))
    return out


def main():
    if not os.path.exists(UI) or not os.path.isdir(FX):
        sys.exit("gen-readme-art: needs ui/banana-ui and ui/tests/out/fixture: run `make test` first")
    os.makedirs(OUT, exist_ok=True)
    scale(banner(), 2).save(os.path.join(OUT, "banner.png"), optimize=True)
    scale(button("Download 0.7"), 2).save(os.path.join(OUT, "download.png"), optimize=True)

    # the screens: the street, a shop, the workshop, the phone book, the ticket
    t = headless("street.json", "home 1; wait 900; shot home; press a; wait 900; press down; wait 600; shot lib; press b; wait 600;"
                 " press start; wait 300; press a; press down; press down; press down; wait 200; shot workshop; press b; press b; wait 300;"
                 " search; press a; press right; press a; press right; press a; wait 100; shot phonebook; press b; press b; press b; press b; pause ticket 1")
    for n in ("home", "lib", "workshop", "phonebook", "ticket"):
        frame(shot(t, n)).save(os.path.join(OUT, n + ".png"), optimize=True)
    shutil.rmtree(t)

    # walking down the street, into a shop, and a game starting in the mosaic
    steps = ["home 0", "wait 1200"]
    n = 0
    def grab(k, ms):
        nonlocal n
        for _ in range(k): steps.append("shot g%03d; wait %d" % (n, ms)); n += 1
    grab(6, 80)
    for _ in range(3): steps.append("press right"); grab(5, 40)
    grab(8, 80)
    steps.append("press a"); grab(8, 60)
    steps.append("press down"); grab(5, 60)
    steps.append("press a"); grab(12, 60)
    t = headless("street.json", "; ".join(steps), phase="dusk")
    frames = [shot(t, "g%03d" % i, 1).convert("P", palette=Image.ADAPTIVE, colors=255) for i in range(n)]
    big = [f.resize((620, 540), Image.NEAREST) for f in frames]
    durations = [80] * 6 + [40] * 15 + [80] * 8 + [60] * 8 + [60] * 5 + [60] * 12
    durations[-1] = 900
    big[0].save(os.path.join(OUT, "street.gif"), save_all=True, append_images=big[1:], duration=durations, loop=0, optimize=True, disposal=1)
    shutil.rmtree(t)

    # the same street at four times of day, and the five neighborhoods
    tiles = []
    for ph in ("dawn", "day", "dusk", "night"):
        t = headless("street.json", "home 1; wait 900; shot home", phase=ph)
        tiles.append(shot(t, "home", 1)); shutil.rmtree(t)
    grid = Image.new("RGB", (310 * 2 + 4, 270 * 2 + 4), C["ink"])
    for i, im in enumerate(tiles): grid.paste(im, ((i % 2) * 314, (i // 2) * 274))
    scale(grid, 2).save(os.path.join(OUT, "times.png"), optimize=True)
    for hood, f in (("banana", "street.json"), ("harbor", "street-harbor.json"), ("blossom", "street-blossom.json"),
                    ("sundown", "street-sundown.json"), ("oldtown", "street-oldtown.json")):
        t = headless(f, "home 2; wait 900; shot home", phase="day")
        frame(shot(t, "home", 1), 3).resize(((310 + 6) * 2, (270 + 6) * 2), Image.NEAREST).save(os.path.join(OUT, "hood-%s.png" % hood), optimize=True)
        shutil.rmtree(t)
    print("gen-readme-art: %s" % ", ".join(sorted(os.listdir(OUT))))


if __name__ == "__main__":
    main()
