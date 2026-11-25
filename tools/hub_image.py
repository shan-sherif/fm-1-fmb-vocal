#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Shan-Sherif
"""The Firmware Hub's card image (fmhub.json "image"): app/site/img/fmhub-square.png, 512 x 512. The name on a black
ground with a faint grid, the knobs' four colours (K1 orange, K2 yellow, K3 violet, K4 cyan), and the home screen
from the simulator (app/docs/img/home-words.png: run tools/manual_shots.sh first) in a frame. Run again after a
change to the screen:
  .venv/bin/python tools/hub_image.py"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "base" / "assets" / "fonts"
SHOT = ROOT / "app" / "docs" / "img" / "home-words.png"
OUT = ROOT / "app" / "site" / "img" / "fmhub-square.png"

BLACK, GRID, FRAME = (0, 0, 0), (20, 20, 20), (60, 60, 60)
WHITE, GREY = (255, 255, 255), (158, 158, 158)
KNOBS = [(0xFF, 0x9A, 0x3C), (0xFF, 0xD2, 0x3F), (0xB4, 0x8C, 0xFF), (0x3F, 0xD0, 0xE0)]   # app/app.c T_K1..T_K4
S = 512


def spaced(d, cx, y, text, font, fill, gap):
    """text centred at cx, its letters `gap` px apart"""
    widths = [d.textlength(c, font=font) for c in text]
    x = cx - (sum(widths) + gap * (len(text) - 1)) / 2
    for c, w in zip(text, widths):
        d.text((x, y), c, font=font, fill=fill)
        x += w + gap


def main():
    img = Image.new("RGB", (S, S), BLACK)
    d = ImageDraw.Draw(img)
    for v in range(0, S, 32):                       # the faint grid
        d.line([(v, 0), (v, S)], fill=GRID)
        d.line([(0, v), (S, v)], fill=GRID)
    bold = lambda px: ImageFont.truetype(str(FONTS / "BarlowSemiCondensed-Bold.ttf"), px)
    semi = lambda px: ImageFont.truetype(str(FONTS / "BarlowSemiCondensed-SemiBold.ttf"), px)
    spaced(d, S / 2, 26, "FM-1", semi(30), GREY, 6)
    spaced(d, S / 2, 52, "FMB", bold(96), WHITE, 8)
    spaced(d, S / 2, 150, "VOCAL", semi(40), GREY, 14)
    for i, c in enumerate(KNOBS):                   # the knobs' colours
        x = S / 2 - 2 * 46 + i * 46 + 4
        d.rectangle([x, 208, x + 38, 211], fill=c)
    # the home screen, framed
    shot = Image.open(SHOT).convert("RGB").resize((264, 264), Image.LANCZOS)
    x0, y0 = (S - 264) // 2, 228
    d.rounded_rectangle([x0 - 8, y0 - 8, x0 + 264 + 7, y0 + 264 + 7], radius=8, fill=BLACK, outline=FRAME, width=2)
    img.paste(shot, (x0, y0))
    img.save(OUT, optimize=True)
    print(f"{OUT.relative_to(ROOT)}: {OUT.stat().st_size} B")


if __name__ == "__main__":
    main()
