#!/usr/bin/env python3
"""Slice 6 generated 3x3 pixel-art sprite sheets, chroma-key #00FF00 background to alpha=0,
and downscale each icon to exact 32x32 pixels using Image.Resampling.NEAREST."""

import os
from collections import deque
from PIL import Image

ARTIFACT_DIR = "/home/tom/.gemini/antigravity-cli/brain/bc81d655-a4e9-4020-ab5e-56b1ecfdd4eb"
OUTPUT_DIRS = [
    "/home/tom/banana-rpc/ui/assets/overrides/icons",
    "/home/tom/banana-sm8550/ui/assets/overrides/icons",
]

SHEETS = [
    (
        "batch_sheet_1_1794504040142.jpg",
        [
            ["apps.png", "collection.png", "gamepad.png"],
            ["moon.png", "ports.png", "steam.png"],
            ["arcade.png", "computer.png", "gba.png"],
        ],
    ),
    (
        "batch_sheet_2_1794504070369.jpg",
        [
            ["music.png", "psp.png", "sword.png"],
            ["atari2600.png", "console.png", "gbc.png"],
            ["n64.png", "psx.png", "tools.png"],
        ],
    ),
    (
        "batch_sheet_3_1794504042014.jpg",
        [
            ["atarilynx.png", "disc.png", "gb.png"],
            ["nds.png", "recent.png", "video.png"],
            ["bananatube.png", "dodakvarter.png", "gear.png"],
        ],
    ),
    (
        "batch_sheet_4_1794504061710.jpg",
        [
            ["nes.png", "saturn.png", "wonderswan.png"],
            ["bank.png", "dreamcast.png", "handheld.png"],
            ["ngp.png", "search.png", "zxspectrum.png"],
        ],
    ),
    (
        "batch_sheet_5_1794504045043.jpg",
        [
            ["book.png", "fantasy.png", "joystick.png"],
            ["pcengine.png", "sega32x.png", "c64.png"],
            ["fav.png", "mastersystem.png", "phone.png"],
        ],
    ),
    (
        "batch_sheet_6_1794504068985.jpg",
        [
            ["segacd.png", "cart.png", "gamegear.png"],
            ["md.png", "picture.png", "snes.png"],
            ["ps2.png", "gc.png", "wii.png"],
        ],
    ),
]


def is_chroma_green(r: int, g: int, b: int) -> bool:
    """Detect pure/near #00FF00 background pixels."""
    return g > 175 and (g - max(r, b)) > 85


def chroma_key_cell(cell_img: Image.Image) -> Image.Image:
    """Key out #00FF00 chroma green background while preserving interior green details
    (unless they are pure #00FF00 center holes like in disc.png or gear.png)."""
    w, h = cell_img.size
    pixels = list(cell_img.get_flattened_data()) if hasattr(cell_img, "get_flattened_data") else list(cell_img.getdata())

    # 1. Flood-fill from all 4 borders through chroma-green pixels
    bg_mask = [False] * (w * h)
    q = deque()

    def try_push(x: int, y: int):
        idx = y * w + x
        if not bg_mask[idx]:
            r, g, b, _ = pixels[idx]
            if is_chroma_green(r, g, b):
                bg_mask[idx] = True
                q.append((x, y))

    for x in range(w):
        try_push(x, 0)
        try_push(x, h - 1)
    for y in range(h):
        try_push(0, y)
        try_push(w - 1, y)

    while q:
        cx, cy = q.popleft()
        for nx, ny in ((cx - 1, cy), (cx + 1, cy), (cx, cy - 1), (cx, cy + 1)):
            if 0 <= nx < w and 0 <= ny < h:
                try_push(nx, ny)

    # 2. Also key out pure #00FF00 interior cutouts (like the center hole of a CD/gear/wrench)
    # where green > 220 and red < 50 and blue < 50.
    out = []
    for idx, (r, g, b, a) in enumerate(pixels):
        if bg_mask[idx] or (g > 220 and r < 50 and b < 50):
            out.append((0, 0, 0, 0))
        elif g > max(r, b) + 18 and r < 75 and b < 75:
            # Despill JPEG chroma-green fringe on dark outlines
            m = max(r, b)
            out.append((r, m, b, 255))
        else:
            out.append((r, g, b, 255))

    keyed = Image.new("RGBA", (w, h))
    keyed.putdata(out)
    return keyed


def main():
    for d in OUTPUT_DIRS:
        os.makedirs(d, exist_ok=True)

    processed = []
    for sheet_file, grid in SHEETS:
        sheet_path = os.path.join(ARTIFACT_DIR, sheet_file)
        sheet = Image.open(sheet_path).convert("RGBA")
        sw, sh = sheet.size

        for row_idx in range(3):
            for col_idx in range(3):
                filename = grid[row_idx][col_idx]
                x0 = (col_idx * sw) // 3
                y0 = (row_idx * sh) // 3
                x1 = ((col_idx + 1) * sw) // 3
                y1 = ((row_idx + 1) * sh) // 3

                cell = sheet.crop((x0, y0, x1, y1))
                keyed = chroma_key_cell(cell)

                bbox = keyed.getbbox()
                if bbox:
                    cropped = keyed.crop(bbox)
                    cw, ch = cropped.size
                    side = max(cw, ch)
                    pad = max(1, side // 30)
                    canvas_side = side + 2 * pad
                    square = Image.new("RGBA", (canvas_side, canvas_side), (0, 0, 0, 0))
                    square.paste(cropped, ((canvas_side - cw) // 2, (canvas_side - ch) // 2))
                else:
                    square = keyed

                # Strictly NEAREST-NEIGHBOR interpolation to 32x32
                icon32 = square.resize((32, 32), resample=Image.Resampling.NEAREST)

                for d in OUTPUT_DIRS:
                    out_path = os.path.join(d, filename)
                    icon32.save(out_path, "PNG")
                processed.append(filename)

    print(f"Successfully processed and saved {len(processed)} 32x32 RGBA icons:")
    print(", ".join(processed))


if __name__ == "__main__":
    main()
