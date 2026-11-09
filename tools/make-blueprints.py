#!/usr/bin/env python3
"""32x32 pixel blueprints for the icon set: hard pixels, no anti-aliasing.

Draws each icon as a silhouette mask, gives it a 1px #000 outline, a 1px top
specular and 1px bottom shadow, then paints the flat details inside the
interior. Colours are those sampled from the reference icons (see PALETTE).

    python3 tools/make-blueprints.py            # -> tmp/blueprints/<name>_bp.png
"""
import os

from PIL import Image, ImageDraw

S = 32
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "tmp", "blueprints")

# sampled with `magick <icon> -format %c histogram:info:`
BLACK = (0, 0, 0, 255)
PSX_FILL, PSX_MID, PSX_DEEP, PSX_HI = (149, 146, 147, 255), (103, 103, 103, 255), (41, 41, 41, 255), (195, 195, 195, 255)
PSX_WHITE = (225, 225, 225, 255)
N64_FILL, N64_MID, N64_DEEP, N64_HI = (167, 166, 166, 255), (115, 115, 115, 255), (65, 64, 59, 255), (143, 143, 143, 255)
MD_FILL, MD_MID, MD_DEEP, MD_HI = (67, 72, 80, 255), (103, 109, 121, 255), (46, 47, 49, 255), (179, 185, 196, 255)
NES_BLACK, NES_GREY, NES_MID, NES_HI = (27, 27, 27, 255), (185, 181, 178, 255), (119, 117, 118, 255), (197, 59, 31, 255)
GB_BODY, GB_MID, GB_SHADE, GB_HI = (193, 190, 185, 255), (116, 114, 117, 255), (137, 135, 138, 255), (223, 223, 223, 255)
RED, YELLOW, GREEN, CYAN, BLUE = (238, 87, 83, 255), (247, 225, 106, 255), (44, 164, 0, 255), (68, 212, 202, 255), (34, 76, 255, 255)
PINK, ORANGE = (223, 138, 210, 255), (255, 107, 0, 255)
SCREEN = (28, 25, 54, 255)          # gba.png / gbc.png screen slate
WHITE = (255, 255, 255, 255)


def mask(draw_fn):
    m = Image.new("L", (S, S), 0)
    draw_fn(ImageDraw.Draw(m))
    return m


def shifted(m, dx, dy):
    # neighbours that are inside the canvas; outside counts as empty
    out = Image.new("L", (S, S), 0)
    out.paste(m, (dx, dy))
    return out


def erode(m):
    px = m.load()
    out = Image.new("L", (S, S), 0)
    o = out.load()
    for y in range(S):
        for x in range(S):
            if not px[x, y]:
                continue
            if x == 0 or y == 0 or x == S - 1 or y == S - 1:
                continue
            if px[x - 1, y] and px[x + 1, y] and px[x, y - 1] and px[x, y + 1]:
                o[x, y] = 255
    return out


def render(m, fill, hi, lo):
    """outline + flat fill + a 1px specular on the top inner edge and shadow on the bottom."""
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    inner = erode(m)
    ip = inner.load()
    p = img.load()
    for y in range(S):
        for x in range(S):
            if ip[x, y]:
                p[x, y] = hi if not ip[x, y - 1] else lo if not ip[x, y + 1] else fill
            elif m.getpixel((x, y)):
                p[x, y] = BLACK
    return img, inner


def paint(img, inner, draw_fn):
    """draw details on a layer, clipped to the interior (never over the outline/transparent)"""
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(layer))
    px = inner.load()
    lp = layer.load()
    for y in range(S):
        for x in range(S):
            if px[x, y] and lp[x, y][3]:
                img.putpixel((x, y), lp[x, y])


def panel(img, inner, box, fill, edge=BLACK):
    """a recessed 1px-edged rectangle inside the body (screen, badge plate, d-pad pad...)"""
    x0, y0, x1, y1 = box
    paint(img, inner, lambda d: (d.rectangle(box, fill=edge), d.rectangle((x0 + 1, y0 + 1, x1 - 1, y1 - 1), fill=fill)))


def dot(img, inner, x, y, colour, size=2, ring=True):
    """a vibrant 1x1/2x2 face button, ringed in black so it reads on light plastic"""
    pad = 1 if ring else 0
    def go(d):
        if ring:
            d.rectangle((x - 1, y - 1, x + size, y + size), fill=BLACK)
        d.rectangle((x, y, x + size - 1, y + size - 1), fill=colour)
    paint(img, inner, go)


def cross(img, inner, x, y, fill, hi, size=6, thick=2):
    """a d-pad: a plus in `fill`, its top face lit by `hi`"""
    def go(d):
        d.rectangle((x, y + (size - thick) // 2, x + size - 1, y + (size + thick) // 2 - 1), fill=fill)
        d.rectangle((x + (size - thick) // 2, y, x + (size + thick) // 2 - 1, y + size - 1), fill=fill)
        d.rectangle((x + (size - thick) // 2, y, x + (size + thick) // 2 - 1, y + 1), fill=hi)
        d.rectangle((x, y + (size - thick) // 2, x + 2, y + (size - thick) // 2 + 1), fill=hi)
    paint(img, inner, go)


def save(name, img):
    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, name + "_bp.png"))


# --- consoles --------------------------------------------------------------

def dreamcast():
    # wide shield body + two short bottom prongs with the notch between them
    m = mask(lambda d: (d.ellipse((2, 3, 29, 21), fill=255),
                        d.rounded_rectangle((3, 15, 12, 29), 4, fill=255),
                        d.rounded_rectangle((19, 15, 28, 29), 4, fill=255)))
    img, inner = render(m, N64_FILL, PSX_WHITE, N64_MID)
    panel(img, inner, (11, 6, 18, 10), N64_DEEP)                       # VMU window
    paint(img, inner, lambda d: d.polygon([(13, 11), (16, 11), (14, 13)], fill=ORANGE))
    cross(img, inner, 5, 12, N64_DEEP, PSX_MID, size=6)
    dot(img, inner, 23, 7, BLUE)                                       # face buttons
    dot(img, inner, 26, 10, YELLOW)
    dot(img, inner, 23, 13, RED)
    dot(img, inner, 20, 10, GREEN)
    save("dreamcast", img)


def saturn():
    m = mask(lambda d: (d.rounded_rectangle((2, 7, 29, 24), 8, fill=255),
                        d.rounded_rectangle((3, 16, 12, 29), 4, fill=255),
                        d.rounded_rectangle((19, 16, 28, 29), 4, fill=255)))
    img, inner = render(m, MD_FILL, MD_MID, MD_DEEP)
    # circular d-pad on the left
    paint(img, inner, lambda d: (d.ellipse((4, 10, 12, 18), fill=BLACK), d.ellipse((5, 11, 11, 17), fill=MD_MID)))
    cross(img, inner, 5, 11, MD_DEEP, MD_MID, size=5)
    # six face buttons: XYZ over ABC
    for i, c in enumerate((BLUE, YELLOW, WHITE)):
        dot(img, inner, 18 + i * 4, 9, c)
    for i, c in enumerate((RED, GREEN, CYAN)):
        dot(img, inner, 18 + i * 4, 14, c)
    save("saturn", img)


def mastersystem():
    m = mask(lambda d: d.rounded_rectangle((2, 9, 29, 24), 3, fill=255))
    img, inner = render(m, NES_GREY, GB_HI, NES_MID)
    panel(img, inner, (3, 10, 28, 23), NES_BLACK)                      # black face
    cross(img, inner, 4, 13, GB_HI, PSX_WHITE, size=7, thick=3)       # rectangular d-pad
    for x in (21, 25):
        dot(img, inner, x, 15, NES_HI, size=2)
    save("mastersystem", img)


# --- handhelds -------------------------------------------------------------

def gamegear():
    m = mask(lambda d: d.rounded_rectangle((1, 8, 30, 25), 4, fill=255))
    img, inner = render(m, MD_FILL, MD_MID, MD_DEEP)
    panel(img, inner, (10, 10, 21, 22), SCREEN)                        # square-ish screen
    cross(img, inner, 3, 14, MD_MID, MD_HI, size=6)
    dot(img, inner, 23, 12, NES_HI)
    dot(img, inner, 23, 17, MD_HI)
    paint(img, inner, lambda d: (d.point((3, 11), fill=RED), d.point((4, 11), fill=GREEN), d.point((5, 11), fill=BLUE)))
    save("gamegear", img)


def psp():
    m = mask(lambda d: d.rounded_rectangle((1, 9, 30, 24), 5, fill=255))
    img, inner = render(m, PSX_DEEP, PSX_FILL, BLACK)
    panel(img, inner, (8, 12, 21, 19), SCREEN)                         # 16:9-ish screen
    paint(img, inner, lambda d: (d.line((3, 12, 3, 17), fill=PSX_HI), d.line((28, 12, 28, 17), fill=PSX_HI)))
    cross(img, inner, 4, 13, PSX_MID, PSX_HI, size=6, thick=2)
    dot(img, inner, 7, 20, PSX_HI, size=1, ring=False)                 # analog nub
    dot(img, inner, 25, 12, GREEN, size=1, ring=False)
    dot(img, inner, 28, 16, RED, size=1, ring=False)
    dot(img, inner, 25, 20, BLUE, size=1, ring=False)
    dot(img, inner, 22, 16, PINK, size=1, ring=False)
    save("psp", img)


def wii():
    m = mask(lambda d: d.rounded_rectangle((10, 2, 21, 29), 3, fill=255))
    img, inner = render(m, GB_HI, WHITE, GB_MID)
    cross(img, inner, 13, 5, GB_MID, GB_SHADE, size=6, thick=1)        # d-pad
    paint(img, inner, lambda d: (d.ellipse((14, 11, 17, 14), fill=GB_MID)))  # A
    paint(img, inner, lambda d: (d.point((12, 16), fill=GB_MID), d.point((19, 16), fill=GB_MID)))  # - / +
    paint(img, inner, lambda d: (d.point((13, 21), fill=GB_MID), d.point((18, 21), fill=GB_MID)))  # 1 / 2
    paint(img, inner, lambda d: [d.point((x, 26), fill=CYAN) for x in (12, 14, 16, 18)])
    save("wii", img)


# --- microcomputers --------------------------------------------------------

def zxspectrum():
    m = mask(lambda d: d.rounded_rectangle((2, 8, 29, 25), 2, fill=255))
    img, inner = render(m, NES_BLACK, PSX_DEEP, BLACK)
    def keys(d):
        for y in range(11, 23, 3):
            for x in range(5, 27, 3):
                d.point((x, y), fill=PSX_DEEP)
    paint(img, inner, keys)
    # the rainbow flash, five 1px diagonal stripes off the bottom-right corner
    def rainbow(d):
        for i, c in enumerate((RED, YELLOW, GREEN, CYAN, BLUE)):
            d.line((17 + i, 25, 28, 14 + i), fill=c)
    paint(img, inner, rainbow)
    save("zxspectrum", img)


def c64():
    m = mask(lambda d: d.rounded_rectangle((2, 9, 29, 24), 3, fill=255))
    img, inner = render(m, GB_BODY, GB_HI, GB_MID)
    def keys(d):
        for y in range(12, 22, 3):
            for x in range(5, 17, 3):
                d.rectangle((x, y, x + 1, y), fill=N64_DEEP)           # key rows
        for y in range(12, 22, 2):
            for x in (21, 25):                                         # function keys, right
                d.rectangle((x, y, x + 1, y), fill=N64_DEEP)
    paint(img, inner, keys)
    paint(img, inner, lambda d: (d.point((6, 10), fill=RED), d.point((8, 10), fill=YELLOW)))
    save("c64", img)


def check(name):
    """the rules the icons are judged by: 32x32, binary alpha, and a black ring only on the edge"""
    im = Image.open(os.path.join(OUT, name + "_bp.png")).convert("RGBA")
    assert im.size == (S, S), (name, im.size)
    px = im.load()
    for y in range(S):
        for x in range(S):
            a = px[x, y][3]
            assert a in (0, 255), (name, x, y, "semi-transparent", a)
            if a and any(not (0 <= x + dx < S and 0 <= y + dy < S) or px[x + dx, y + dy][3] == 0
                         for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                assert px[x, y][:3] == (0, 0, 0), (name, x, y, "edge is not black", px[x, y])


for fn in (dreamcast, saturn, mastersystem, gamegear, psp, wii, zxspectrum, c64):
    fn()
    check(fn.__name__)
    print("wrote", fn.__name__ + "_bp.png")
