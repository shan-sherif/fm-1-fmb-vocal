#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Shan-Sherif
"""The README's wide header (app/site/img/banner.png, 2560 x 800: 1280 x 400 at twice the pixels), in the look of the
hub's card (tools/hub_image.py): the name on a black ground with a faint grid and the knobs' four colours, beside
three of the FM-1's screens from the simulator in frames: the Bestiary (the robot), the sequencer (the beatbox) and
the voice (WORDS). Run tools/manual_shots.sh first, and again after a change to a screen:
  .venv/bin/python tools/readme_banner.py"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "base" / "assets" / "fonts"
SHOTS = ROOT / "app" / "docs" / "img"
OUT = ROOT / "app" / "site" / "img" / "banner.png"

BLACK, GRID, FRAME = (0, 0, 0), (20, 20, 20), (60, 60, 60)
WHITE, GREY, DIM = (255, 255, 255), (158, 158, 158), (96, 96, 96)
KNOBS = [(0xFF, 0x9A, 0x3C), (0xFF, 0xD2, 0x3F), (0xB4, 0x8C, 0xFF), (0x3F, 0xD0, 0xE0)]   # app/app.c T_K1..T_K4
X = 2                                               # (drawn at twice the size: sharp on high-density screens)
W, H = 1280, 400
SCREENS = [("robot.png", "BESTIARY"), ("beat.png", "SEQUENCER"), ("home-words.png", "VOICE")]
SIDE = 264                                          # a screen's side (the hub card's)


def spaced(d, cx, y, text, font, fill, gap):
    """text centred at cx, its letters `gap` px apart"""
    widths = [d.textlength(c, font=font) for c in text]
    x = cx - (sum(widths) + gap * (len(text) - 1)) / 2
    for c, w in zip(text, widths):
        d.text((x, y), c, font=font, fill=fill)
        x += w + gap


def main():
    img = Image.new("RGB", (W * X, H * X), BLACK)
    d = ImageDraw.Draw(img)
    for v in range(0, W, 32):                       # the faint grid
        d.line([(v * X, 0), (v * X, H * X)], fill=GRID, width=X)
    for v in range(0, H, 32):
        d.line([(0, v * X), (W * X, v * X)], fill=GRID, width=X)
    bold = lambda px: ImageFont.truetype(str(FONTS / "BarlowSemiCondensed-Bold.ttf"), px * X)
    semi = lambda px: ImageFont.truetype(str(FONTS / "BarlowSemiCondensed-SemiBold.ttf"), px * X)
    # the name, on the left
    cx = 196
    spaced(d, cx * X, 74 * X, "FM-1", semi(30), GREY, 6 * X)
    spaced(d, cx * X, 100 * X, "FMB", bold(112), WHITE, 9 * X)
    spaced(d, cx * X, 214 * X, "VOCAL", semi(44), GREY, 15 * X)
    for i, c in enumerate(KNOBS):                   # the knobs' colours
        x = cx - 2 * 50 + i * 50 + 4
        d.rectangle([x * X, 280 * X, (x + 42) * X - 1, 284 * X - 1], fill=c)
    spaced(d, cx * X, 304 * X, "A SIMULATED VOICE", semi(16), DIM, 3 * X)
    spaced(d, cx * X, 326 * X, "AND ITS BESTIARY", semi(16), DIM, 3 * X)
    # the screens, framed, on the right, each named under it
    x0 = 400
    gap = (W - x0 - 3 * SIDE - 24) // 3
    for i, (name, label) in enumerate(SCREENS):
        x = x0 + i * (SIDE + gap)
        y = 52
        shot = Image.open(SHOTS / name).convert("RGB").resize((SIDE * X, SIDE * X), Image.LANCZOS)
        d.rounded_rectangle([(x - 8) * X, (y - 8) * X, (x + SIDE + 8) * X - 1, (y + SIDE + 8) * X - 1], radius=8 * X,
                            fill=BLACK, outline=FRAME, width=2 * X)
        img.paste(shot, (x * X, y * X))
        spaced(d, (x + SIDE / 2) * X, (y + SIDE + 22) * X, label, semi(16), GREY, 4 * X)
    img.save(OUT, optimize=True)
    print(f"{OUT.relative_to(ROOT)}: {img.size[0]} x {img.size[1]}, {OUT.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
