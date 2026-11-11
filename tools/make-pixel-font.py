#!/usr/bin/env python3
"""Banana's pixel fonts, drawn here as text and built into TrueType outlines of whole grid squares.

  tools/make-pixel-font.py [outdir]     writes BananaPixel.ttf and BananaTitle.ttf (default ui/assets/banana/fonts)

Every art pixel is a square of 100 font units, so a font drawn at a size where 100 units is a whole number of device
pixels comes out hard-edged with no grey: the menu draws glyphs unhinted at whole-pixel origins (raster.cpp).
  banana        unitsPerEm 1200 (12 cells): "size": 12 on a 310-wide canvas is 4 device px per art pixel
  banana-title  the same glyphs doubled with EPX (rounded diagonals), unitsPerEm 2400: "size": 24
Rows: 0-1 accents, 2-8 capitals (7 high), 9-10 descenders; the baseline is under row 8. Glyphs below are given from
row 2 down, rows separated by "/"; "#" is ink. Accented letters are composed from a base and a mark.
"""
import os, sys
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

ROWS, ASC = 11, 9          # rows in the frame; cells above the baseline (rows 0-8)
G = {
 " ": "...", "!": "#/#/#/#/#/./#", '"': "#.#/#.#", "#": "...../.#.#./#####/.#.#./#####/.#.#.",
 "$": "..#../.####/#.#../.###./..#.#/####./..#..", "%": "##..#/##..#/...#./..#../.#.../#..##/#..##",
 "&": ".##../#..#./#.#../.#.../#.#.#/#..#./.##.#", "'": "#/#", "(": ".#/#./#./#./#./#./.#", ")": "#./.#/.#/.#/.#/.#/#.",
 "*": "...../#.#.#/.###./#####/.###./#.#.#", "+": "...../..#../..#../#####/..#../..#..", ",": "../../../../../../.#/#.",
 "-": ".../.../.../###", ".": "././././././#", "/": "....#/....#/...#./..#../.#.../#..../#....",
 "0": ".###./#...#/#..##/#.#.#/##..#/#...#/.###.", "1": ".#./##./.#./.#./.#./.#./###", "2": ".###./#...#/....#/...#./..#../.#.../#####",
 "3": "####./....#/....#/.###./....#/....#/####.", "4": "...#./..##./.#.#./#..#./#####/...#./...#.", "5": "#####/#..../####./....#/....#/#...#/.###.",
 "6": ".###./#..../#..../####./#...#/#...#/.###.", "7": "#####/....#/...#./..#../.#.../.#.../.#...", "8": ".###./#...#/#...#/.###./#...#/#...#/.###.",
 "9": ".###./#...#/#...#/.####/....#/....#/.###.", ":": "./././#/././#", ";": "../../../.#/../../.#/#.",
 "<": "...#/..#./.#../#.../.#../..#./...#", "=": "...../...../#####/...../#####", ">": "#.../.#../..#./...#/..#./.#../#...",
 "?": ".###./#...#/....#/...#./..#../...../..#..", "@": ".###./#...#/#.###/#.#.#/#.###/#..../.####",
 "A": ".###./#...#/#...#/#####/#...#/#...#/#...#", "B": "####./#...#/#...#/####./#...#/#...#/####.", "C": ".###./#...#/#..../#..../#..../#...#/.###.",
 "D": "####./#...#/#...#/#...#/#...#/#...#/####.", "E": "#####/#..../#..../####./#..../#..../#####", "F": "#####/#..../#..../####./#..../#..../#....",
 "G": ".###./#...#/#..../#.###/#...#/#...#/.####", "H": "#...#/#...#/#...#/#####/#...#/#...#/#...#", "I": "###/.#./.#./.#./.#./.#./###",
 "J": "..###/...#./...#./...#./#..#./#..#./.##..", "K": "#...#/#..#./#.#../##.../#.#../#..#./#...#", "L": "#..../#..../#..../#..../#..../#..../#####",
 "M": "#...#/##.##/#.#.#/#.#.#/#...#/#...#/#...#", "N": "#...#/##..#/##..#/#.#.#/#..##/#..##/#...#", "O": ".###./#...#/#...#/#...#/#...#/#...#/.###.",
 "P": "####./#...#/#...#/####./#..../#..../#....", "Q": ".###./#...#/#...#/#...#/#.#.#/#..#./.##.#", "R": "####./#...#/#...#/####./#.#../#..#./#...#",
 "S": ".####/#..../#..../.###./....#/....#/####.", "T": "#####/..#../..#../..#../..#../..#../..#..", "U": "#...#/#...#/#...#/#...#/#...#/#...#/.###.",
 "V": "#...#/#...#/#...#/#...#/.#.#./.#.#./..#..", "W": "#...#/#...#/#...#/#.#.#/#.#.#/##.##/#...#", "X": "#...#/#...#/.#.#./..#../.#.#./#...#/#...#",
 "Y": "#...#/#...#/.#.#./..#../..#../..#../..#..", "Z": "#####/....#/...#./..#../.#.../#..../#####", "[": "##/#./#./#./#./#./##",
 "\\": "#..../#..../.#.../..#../...#./....#/....#", "]": "##/.#/.#/.#/.#/.#/##", "^": ".#./#.#", "_": "...../...../...../...../...../...../...../#####",
 "`": "#./.#",
 "a": "..../..../.##./...#/.###/#..#/.###", "b": "#.../#.../###./#..#/#..#/#..#/###.", "c": "..../..../.###/#.../#.../#.../.###",
 "d": "...#/...#/.###/#..#/#..#/#..#/.###", "e": "..../..../.##./#..#/####/#.../.###", "f": "..##/.#../####/.#../.#../.#../.#..",
 "g": "..../..../.###/#..#/#..#/#..#/.###/...#/###.", "h": "#.../#.../###./#..#/#..#/#..#/#..#", "i": "#/./#/#/#/#/#",
 "j": ".#/../.#/.#/.#/.#/.#/.#/#.", "k": "#.../#.../#..#/#.#./##../#.#./#..#", "l": "#./#./#./#./#./#./.#",
 "m": "...../...../####./#.#.#/#.#.#/#.#.#/#.#.#", "n": "..../..../###./#..#/#..#/#..#/#..#", "o": "..../..../.##./#..#/#..#/#..#/.##.",
 "p": "..../..../###./#..#/#..#/#..#/###./#.../#...", "q": "..../..../.###/#..#/#..#/#..#/.###/...#/...#", "r": "..../..../#.##/##../#.../#.../#...",
 "s": "..../..../.###/#.../.##./...#/###.", "t": ".#../.#../####/.#../.#../.#../..##", "u": "..../..../#..#/#..#/#..#/#..#/.###",
 "v": "...../...../#...#/#...#/.#.#./.#.#./..#..", "w": "...../...../#...#/#.#.#/#.#.#/#.#.#/.#.#.", "x": "..../..../#..#/#..#/.##./#..#/#..#",
 "y": "..../..../#..#/#..#/#..#/#..#/.###/...#/.##.", "z": "..../..../####/...#/.##./#.../####", "{": "..#/.#./.#./#../.#./.#./..#",
 "|": "#/#/#/#/#/#/#/#/#", "}": "#../.#./.#./..#/.#./.#./#..", "~": "...../...../...../.##.#/#.##.",
 # Latin-1 and a few typographic marks the translations use
 "¡": "././#/./#/#/#/#/#", "¿": "..#../...../..#../.#.../#..../#...#/.###.", "«": "...../...../..#.#/.#.#./#.#../.#.#./..#.#",
 "»": "...../...../#.#../.#.#./..#.#/.#.#./#.#..", "°": ".#./#.#/.#.", "·": "./././#", "×": "...../...../#...#/.#.#./..#../.#.#./#...#",
 "÷": "...../..#../...../#####/...../..#..", "Æ": ".####/#.#../#.#../#####/#.#../#.#../#.###", "æ": "...../...../##.#./..###/.####/#.#../.##.#",
 "ß": ".##./#..#/#..#/#.#./#..#/#..#/#.#.", "Ø": ".###./#..##/#..##/#.#.#/##..#/##..#/.###.", "ø": "...../...../.###./#..##/#.#.#/##..#/.###.",
 "Ð": ".###./.#..#/.#..#/###.#/.#..#/.#..#/.###.", "ð": ".#.#/..#./.###/#..#/#..#/#..#/.##.", "Þ": "#..../####./#...#/#...#/####./#..../#....",
 "þ": "#.../#.../###./#..#/#..#/#..#/###./#.../#...", "ı": "././#/#/#/#/#",
 "‘": ".#/#.", "’": "#/#", "“": ".#.#/#.#.", "”": "#.#/#.#", "–": "..../..../..../####", "—": "....../....../....../######",
 "…": "...../...../...../...../...../...../#.#.#", "€": "..###/.#.../####./.#.../####./.#.../..###",
}
MARKS = {"grave": ["#.", ".#"], "acute": [".#", "#."], "circ": [".#.", "#.#"], "tilde": [".#.#", "#.#."], "diaer": ["#.#"],
         "ring": ["###", "#.#"], "cedil": [".#", "##"]}
COMPOSE = {}
for base, cap, marks in (("A", 1, "ÀÁÂÃÄÅ"), ("a", 0, "àáâãäå"),
                         ("E", 1, "ÈÉÊË"), ("e", 0, "èéêë"), ("I", 1, "ÌÍÎÏ"),
                         ("ı", 0, "ìíîï"), ("O", 1, "ÒÓÔÕÖ"), ("o", 0, "òóôõö"),
                         ("U", 1, "ÙÚÛÜ"), ("u", 0, "ùúûü")):
    names = ["grave", "acute", "circ", "tilde", "diaer", "ring"] if len(marks) == 6 else \
            ["grave", "acute", "circ", "tilde", "diaer"] if len(marks) == 5 else ["grave", "acute", "circ", "diaer"]
    for ch, m in zip(marks, names): COMPOSE[ch] = (base, m, cap)
COMPOSE.update({"Ñ": ("N", "tilde", 1), "ñ": ("n", "tilde", 0), "Ý": ("Y", "acute", 1), "ý": ("y", "acute", 0),
                "ÿ": ("y", "diaer", 0), "Ç": ("C", "cedil", 1), "ç": ("c", "cedil", 0)})


def bitmap(ch):
    """The glyph as ROWS lists of 0/1, its width in cells."""
    if ch in COMPOSE:
        base, m, cap = COMPOSE[ch]
        rows, w = bitmap(base)
        mark = MARKS[m]
        if m == "cedil":
            top = 9
        else:
            top = (2 - len(mark)) if cap else (3 - len(mark))
        if m == "diaer" and not cap: top = 2
        if len(mark[0]) > w:	# a mark wider than its letter (the dotless i): the letter widens, centred under it
            pad = len(mark[0]) - w
            rows = [[0] * (pad // 2) + r + [0] * (pad - pad // 2) for r in rows]
            w = len(mark[0])
        x0 = (w - len(mark[0])) // 2
        for j, r in enumerate(mark):
            for i, c in enumerate(r):
                if c == "#" and 0 <= x0 + i < w: rows[top + j][x0 + i] = 1
        return rows, w
    lines = G[ch].split("/")
    w = max(len(r) for r in lines)
    rows = [[0] * w for _ in range(ROWS)]
    for j, r in enumerate(lines):
        for i, c in enumerate(r):
            if c == "#": rows[2 + j][i] = 1
    return rows, w


def epx(rows, w):
    """Scale2x/EPX: every pixel to four, corners filled along diagonals."""
    H = len(rows)
    at = lambda x, y: rows[y][x] if 0 <= x < w and 0 <= y < H else 0
    out = [[0] * (w * 2) for _ in range(H * 2)]
    for y in range(H):
        for x in range(w):
            p, A, B, C, D = at(x, y), at(x, y - 1), at(x + 1, y), at(x - 1, y), at(x, y + 1)
            q = [p, p, p, p]
            if C == A and C != D and A != B: q[0] = A
            if A == B and A != C and B != D: q[1] = B
            if D == C and D != B and C != A: q[2] = C
            if B == D and B != A and D != C: q[3] = D
            out[2 * y][2 * x], out[2 * y][2 * x + 1], out[2 * y + 1][2 * x], out[2 * y + 1][2 * x + 1] = q
    return out, w * 2


def outline(rows, asc, cell=100):
    """One rectangle per horizontal run of ink, clockwise (TrueType's direction for outer contours)."""
    pen = TTGlyphPen(None)
    for j, r in enumerate(rows):
        i = 0
        while i < len(r):
            if not r[i]: i += 1; continue
            k = i
            while k < len(r) and r[k]: k += 1
            x0, x1, y1 = i * cell, k * cell, (asc - j) * cell
            y0 = y1 - cell
            pen.moveTo((x0, y0)); pen.lineTo((x0, y1)); pen.lineTo((x1, y1)); pen.lineTo((x1, y0)); pen.closePath()
            i = k
    return pen.glyph()


def build(path, family, scale):
    chars = sorted(set(G) | set(COMPOSE))
    upm, asc, desc = 1200 * scale, ASC * scale, (ROWS + 1 - ASC) * scale
    order, glyphs, metrics, cmap = [".notdef"], {}, {}, {}
    box = [[1] * 4] + [[1, 0, 0, 1]] * 5 + [[1] * 4]
    nd = [[0] * 4 for _ in range(2)] + [list(r) for r in box] + [[0] * 4 for _ in range(2)]
    if scale == 2: nd, _ = epx(nd, 4)
    glyphs[".notdef"] = outline(nd, asc); metrics[".notdef"] = ((4 * scale + scale) * 100, 0)
    for ch in chars:
        rows, w = bitmap(ch)
        if scale == 2: rows, w = epx(rows, w)
        name = "uni%04X" % ord(ch)
        order.append(name); cmap[ord(ch)] = name
        glyphs[name] = outline(rows, asc)
        metrics[name] = ((w + scale) * 100, 0)
    fb = FontBuilder(upm, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=asc * 100, descent=-desc * 100)
    fb.setupNameTable({"familyName": family, "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=asc * 100, sTypoDescender=-desc * 100, sTypoLineGap=0, usWinAscent=asc * 100, usWinDescent=desc * 100)
    fb.setupPost()
    fb.save(path)
    return len(chars)


if __name__ == "__main__":
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "ui", "assets", "banana", "fonts")
    os.makedirs(out, exist_ok=True)
    n = build(os.path.join(out, "BananaPixel.ttf"), "Banana Pixel", 1)
    build(os.path.join(out, "BananaTitle.ttf"), "Banana Title", 2)
    print("make-pixel-font: %d glyphs each in %s (BananaPixel.ttf, BananaTitle.ttf)" % (n, out))
