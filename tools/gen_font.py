#!/usr/bin/env python3
"""
Remote PKG Installer NextGen — bitmap font generator.

Renders ASCII 32..126 into a packed 1-bit-per-pixel glyph table
(cell 12x21, font size 15, monospace) and emits src/font_data.h.

The runtime (src/font.c) expands the packed bits into an ARGB8888
atlas texture once at startup; glyphs are tinted via SDL_SetTextureColorMod.

Usage: python3 tools/gen_font.py
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "font_data.h"

CELL_W = 12
CELL_H = 21
FONT_SIZE = 15
FIRST = 32
LAST = 126

FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
]


def load_font():
    for path in FONT_CANDIDATES:
        try:
            return ImageFont.truetype(path, FONT_SIZE)
        except OSError:
            continue
    raise SystemExit("no usable font found (tried %s)" % FONT_CANDIDATES)


def main():
    font = load_font()
    glyphs = []

    for code in range(FIRST, LAST + 1):
        img = Image.new("1", (CELL_W, CELL_H), 0)
        draw = ImageDraw.Draw(img)
        ch = chr(code)
        bbox = draw.textbbox((0, 0), ch, font=font)
        w = bbox[2] - bbox[0]
        h = bbox[3] - bbox[1]
        x = (CELL_W - w) // 2 - bbox[0]
        y = (CELL_H - h) // 2 - bbox[1]
        draw.text((x, y), ch, font=font, fill=1)

        rows = []
        for py in range(CELL_H):
            bits = 0
            for px in range(CELL_W):
                if img.getpixel((px, py)):
                    bits |= 1 << (15 - px)
            rows.append(bits)
        glyphs.append(rows)

    lines = []
    lines.append("/*")
    lines.append(" * Remote PKG Installer NextGen")
    lines.append(" * Copyright (c) 2025-present AldySan and contributors.")
    lines.append(" * Licensed under the GNU General Public License v3.0. See LICENSE for details.")
    lines.append(" *")
    lines.append(" * GENERATED FILE — bitmap font (ASCII 32..126, 12x21 cells, 1bpp).")
    lines.append(" * Regenerate with: python3 tools/gen_font.py")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#define FONT_FIRST 0x20")
    lines.append("#define FONT_LAST 0x7E")
    lines.append("#define FONT_CELL_W %d" % CELL_W)
    lines.append("#define FONT_CELL_H %d" % CELL_H)
    lines.append("")
    lines.append("/* Rows are 16-bit big-endian-in-int bitmaps (bit 15 = leftmost pixel). */")
    lines.append("static const unsigned short font_glyphs[%d][%d] = {"
                 % (LAST - FIRST + 1, CELL_H))
    for rows in glyphs:
        lines.append("    { " + ", ".join("0x%04X" % r for r in rows) + " },")
    lines.append("};")
    lines.append("")

    OUT.write_text("\n".join(lines))
    print("wrote %s (%d glyphs)" % (OUT, len(glyphs)))


if __name__ == "__main__":
    main()
