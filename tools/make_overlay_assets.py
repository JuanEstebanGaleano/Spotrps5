#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Spotrps5 contributors
"""Generates the overlay's font and icon atlas (loader/src/overlay_assets.c and loader/include/overlay_icons.h).

  python3 tools/make_overlay_assets.py [path/to/DejaVuSans.ttf path/to/DejaVuSans-Bold.ttf]

The atlas (one byte of coverage per pixel, zlib-compressed) holds
  style 0  DejaVu Sans: ASCII, Latin-1, Latin Extended-A, common punctuation, and the four controller button symbols (codes 1-4)
  style 1  DejaVu Sans Bold: ASCII, Latin-1, common punctuation
  style 2  icons drawn here (play, pause, heart, ...), each in a square cell
Glyphs are looked up by (style << 24 | code point) with a binary search. Needs Pillow and NumPy.
DejaVu fonts: Bitstream Vera / DejaVu license (free to embed and redistribute, see CREDITS.md). The icons are original drawings.
"""
import math
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONT = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
BOLD = sys.argv[2] if len(sys.argv) > 2 else "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
SIZE = 40
ATLAS_W, ATLAS_H = 1024, 2048
ICON = 96          # icon cell, pixels
SS = 8             # icon supersampling

EXTRA = [0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026, 0x2030, 0x20AC, 0x2122, 0x2190, 0x2191, 0x2192,
         0x2193, 0x2212, 0x2605, 0x2665, 0x00B7]
REGULAR = list(range(32, 127)) + list(range(160, 256)) + list(range(0x100, 0x180)) + EXTRA
BOLD_SET = list(range(32, 127)) + list(range(160, 256)) + EXTRA
SYMBOLS = {1: "cross", 2: "circle", 3: "square", 4: "triangle"}  # the controller's face buttons, drawn as shapes

atlas = Image.new("L", (ATLAS_W, ATLAS_H), 0)
draw = ImageDraw.Draw(atlas)
x = y = 2
row_height = 0


def reserve(width, height):
    global x, y, row_height
    if x + width + 2 > ATLAS_W:
        x, y, row_height = 2, y + row_height + 2, 0
    assert y + height + 2 <= ATLAS_H, "atlas full"
    origin = (x, y)
    x += width + 2
    row_height = max(row_height, height)
    return origin


regular = ImageFont.truetype(FONT, SIZE)
bold = ImageFont.truetype(BOLD, SIZE)
ascent, descent = regular.getmetrics()
LINE = ascent + descent
records = {}  # key -> (x, y, w, h, advance, bearing)


def add_text_glyph(font, style, code):
    ch = chr(code)
    advance = font.getlength(ch)
    left, _, right, _ = font.getbbox(ch)
    width = max(1, int(math.ceil(max(right, advance) - min(left, 0))) + 2)
    ox, oy = reserve(width, LINE)
    draw.text((ox - min(left, 0) + 1, oy), ch, font=font, fill=255)
    records[(style << 24) | code] = (ox, oy, width, LINE, advance, -min(left, 0) + 1)


for code in REGULAR:
    # Skip code points the font does not have (it would draw the "missing" box).
    if code > 126 and regular.getmask(chr(code)).getbbox() is None and code not in (0xA0, 0xAD):
        continue
    add_text_glyph(regular, 0, code)
for code in BOLD_SET:
    if code > 126 and bold.getmask(chr(code)).getbbox() is None and code not in (0xA0, 0xAD):
        continue
    add_text_glyph(bold, 1, code)

for code, kind in SYMBOLS.items():
    width = advance = LINE * 3 // 4
    ox, oy = reserve(width, LINE)
    s, m = width, width // 8
    top = oy + (LINE - s) // 2
    box = (ox + m, top + m, ox + s - m, top + s - m)
    stroke = max(3, s // 9)
    if kind == "cross":
        draw.line((box[0], box[1], box[2], box[3]), fill=255, width=stroke)
        draw.line((box[0], box[3], box[2], box[1]), fill=255, width=stroke)
    elif kind == "circle":
        draw.ellipse(box, outline=255, width=stroke)
    elif kind == "square":
        draw.rectangle(box, outline=255, width=stroke)
    else:
        draw.polygon([((box[0] + box[2]) // 2, box[1]), (box[2], box[3]), (box[0], box[3])], outline=255, width=stroke)
    records[code] = (ox, oy, width, LINE, advance, 0.0)


# ---- icons --------------------------------------------------------------------------------------------------------------------------
def heart_points(cx, cy, scale, steps=120):
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        hx = 16 * math.sin(t) ** 3
        hy = -(13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t))
        pts.append((cx + hx * scale, cy + hy * scale))
    return pts


class Canvas:
    def __init__(self):
        self.size = ICON * SS
        self.image = Image.new("L", (self.size, self.size), 0)
        self.d = ImageDraw.Draw(self.image)

    def p(self, v):
        return v * self.size

    def pts(self, points):
        return [(self.p(a), self.p(b)) for a, b in points]

    def poly(self, points, fill=255):
        self.d.polygon(self.pts(points), fill=fill)

    def line(self, points, width, fill=255):
        w = int(self.p(width))
        pts = self.pts(points)
        self.d.line(pts, fill=fill, width=w, joint="curve")
        for px, py in (pts[0], pts[-1]):  # round caps
            self.d.ellipse((px - w / 2, py - w / 2, px + w / 2, py + w / 2), fill=fill)

    def rrect(self, box, radius, fill=255):
        self.d.rounded_rectangle([self.p(box[0]), self.p(box[1]), self.p(box[2]), self.p(box[3])], radius=self.p(radius), fill=fill)

    def ellipse(self, box, fill=255, outline=None, width=0):
        self.d.ellipse([self.p(box[0]), self.p(box[1]), self.p(box[2]), self.p(box[3])], fill=fill, outline=outline, width=int(self.p(width)))

    def arc(self, box, start, end, width):
        self.d.arc([self.p(box[0]), self.p(box[1]), self.p(box[2]), self.p(box[3])], start, end, fill=255, width=int(self.p(width)))

    def rounded_poly(self, points, r=0.045):
        self.poly(points)
        self.line(points + [points[0]], r * 2)

    def done(self):
        return self.image.resize((ICON, ICON), Image.LANCZOS)


def icon_play(c): c.rounded_poly([(0.30, 0.17), (0.30, 0.83), (0.84, 0.50)], 0.05)
def icon_pause(c): c.rrect((0.24, 0.17, 0.43, 0.83), 0.06); c.rrect((0.57, 0.17, 0.76, 0.83), 0.06)
def icon_prev(c): c.rrect((0.17, 0.20, 0.29, 0.80), 0.04); c.rounded_poly([(0.82, 0.20), (0.82, 0.80), (0.36, 0.50)], 0.04)
def icon_next(c): c.rrect((0.71, 0.20, 0.83, 0.80), 0.04); c.rounded_poly([(0.18, 0.20), (0.18, 0.80), (0.64, 0.50)], 0.04)


def icon_shuffle(c):
    c.line([(0.10, 0.31), (0.34, 0.31), (0.62, 0.69), (0.74, 0.69)], 0.07)
    c.line([(0.10, 0.69), (0.34, 0.69), (0.62, 0.31), (0.74, 0.31)], 0.07)
    c.poly([(0.72, 0.17), (0.72, 0.45), (0.92, 0.31)])
    c.poly([(0.72, 0.55), (0.72, 0.83), (0.92, 0.69)])


def repeat_base(c):
    c.d.rounded_rectangle([c.p(0.12), c.p(0.28), c.p(0.88), c.p(0.72)], radius=c.p(0.14), outline=255, width=int(c.p(0.07)))
    c.d.rectangle([c.p(0.58), c.p(0.20), c.p(0.80), c.p(0.38)], fill=0)
    c.d.rectangle([c.p(0.20), c.p(0.62), c.p(0.42), c.p(0.80)], fill=0)
    c.poly([(0.60, 0.14), (0.60, 0.42), (0.82, 0.28)])
    c.poly([(0.40, 0.58), (0.40, 0.86), (0.18, 0.72)])


def icon_repeat(c): repeat_base(c)


def icon_repeat_one(c):
    repeat_base(c)
    c.d.rectangle([c.p(0.41), c.p(0.40), c.p(0.59), c.p(0.60)], fill=0)
    f = ImageFont.truetype(BOLD, int(c.p(0.30)))
    c.d.text((c.p(0.5), c.p(0.5)), "1", font=f, fill=255, anchor="mm")


def speaker(c):
    c.rounded_poly([(0.12, 0.39), (0.28, 0.39), (0.47, 0.22), (0.47, 0.78), (0.28, 0.61), (0.12, 0.61)], 0.03)


def icon_volume(c):
    speaker(c)
    c.arc((0.34, 0.33, 0.66, 0.67), -55, 55, 0.065)
    c.arc((0.26, 0.20, 0.78, 0.80), -55, 55, 0.065)


def icon_mute(c):
    speaker(c)
    c.line([(0.62, 0.38), (0.86, 0.62)], 0.07)
    c.line([(0.86, 0.38), (0.62, 0.62)], 0.07)


def icon_heart(c):
    pts = heart_points(0.5, 0.52, 0.0245)
    c.line(pts + [pts[0]], 0.07)


def icon_heart_filled(c): c.poly(heart_points(0.5, 0.52, 0.0285))


def icon_search(c):
    c.d.ellipse([c.p(0.14), c.p(0.14), c.p(0.64), c.p(0.64)], outline=255, width=int(c.p(0.085)))
    c.line([(0.57, 0.57), (0.86, 0.86)], 0.10)


def icon_home(c): c.rounded_poly([(0.5, 0.13), (0.89, 0.47), (0.78, 0.47), (0.78, 0.86), (0.60, 0.86), (0.60, 0.61), (0.40, 0.61), (0.40, 0.86), (0.22, 0.86), (0.22, 0.47), (0.11, 0.47)], 0.02)


def icon_library(c):
    c.rrect((0.15, 0.18, 0.28, 0.82), 0.03)
    c.rrect((0.34, 0.18, 0.47, 0.82), 0.03)
    c.rounded_poly([(0.55, 0.24), (0.67, 0.20), (0.86, 0.76), (0.74, 0.80)], 0.02)


def icon_plus(c): c.rrect((0.44, 0.16, 0.56, 0.84), 0.05); c.rrect((0.16, 0.44, 0.84, 0.56), 0.05)
def icon_check(c): c.line([(0.20, 0.54), (0.42, 0.76), (0.82, 0.28)], 0.10)
def icon_close(c): c.line([(0.24, 0.24), (0.76, 0.76)], 0.09); c.line([(0.76, 0.24), (0.24, 0.76)], 0.09)
def icon_back(c): c.line([(0.64, 0.18), (0.32, 0.50), (0.64, 0.82)], 0.10)


def icon_backspace(c):
    c.d.polygon(c.pts([(0.10, 0.50), (0.31, 0.22), (0.90, 0.22), (0.90, 0.78), (0.31, 0.78)]), outline=255, width=int(c.p(0.06)))
    c.line([(0.48, 0.38), (0.72, 0.62)], 0.06)
    c.line([(0.72, 0.38), (0.48, 0.62)], 0.06)


def icon_note(c):
    c.ellipse((0.16, 0.58, 0.46, 0.84))
    c.rrect((0.40, 0.14, 0.50, 0.72), 0.03)
    c.poly([(0.46, 0.14), (0.84, 0.30), (0.84, 0.46), (0.46, 0.32)])


def icon_device(c):
    c.d.rounded_rectangle([c.p(0.28), c.p(0.10), c.p(0.72), c.p(0.90)], radius=c.p(0.08), outline=255, width=int(c.p(0.06)))
    c.ellipse((0.40, 0.46, 0.60, 0.66), fill=0, outline=255, width=0.05)
    c.ellipse((0.45, 0.20, 0.55, 0.30))


def icon_equalizer(c):
    c.rrect((0.16, 0.45, 0.32, 0.85), 0.04)
    c.rrect((0.42, 0.15, 0.58, 0.85), 0.04)
    c.rrect((0.68, 0.32, 0.84, 0.85), 0.04)


def icon_queue(c):
    for yy in (0.25, 0.45, 0.65):
        c.rrect((0.14, yy - 0.04, 0.62, yy + 0.04), 0.03)
    c.poly([(0.70, 0.40), (0.70, 0.80), (0.92, 0.60)])


ICONS = [("PLAY", icon_play), ("PAUSE", icon_pause), ("PREV", icon_prev), ("NEXT", icon_next), ("SHUFFLE", icon_shuffle),
         ("REPEAT", icon_repeat), ("REPEAT_ONE", icon_repeat_one), ("VOLUME", icon_volume), ("MUTE", icon_mute), ("HEART", icon_heart),
         ("HEART_FILLED", icon_heart_filled), ("SEARCH", icon_search), ("HOME", icon_home), ("LIBRARY", icon_library), ("PLUS", icon_plus),
         ("CHECK", icon_check), ("CLOSE", icon_close), ("BACK", icon_back), ("BACKSPACE", icon_backspace), ("NOTE", icon_note),
         ("DEVICE", icon_device), ("EQUALIZER", icon_equalizer), ("QUEUE", icon_queue)]
for index, (name, painter) in enumerate(ICONS):
    canvas = Canvas()
    painter(canvas)
    cell = canvas.done()
    ox, oy = reserve(ICON, ICON)
    atlas.paste(cell, (ox, oy))
    records[(2 << 24) | index] = (ox, oy, ICON, ICON, float(ICON), 0.0)

atlas_bytes = np.asarray(atlas, dtype=np.uint8)
# A solid white 4x4 corner (unused by glyphs) for plain rectangles.
atlas_bytes = atlas_bytes.copy()
atlas_bytes[ATLAS_H - 4:, ATLAS_W - 4:] = 255
used_rows = max(r[1] + r[3] for r in records.values())
assert used_rows < ATLAS_H - 6, f"atlas too full ({used_rows} of {ATLAS_H})"
font_alpha = zlib.compress(atlas_bytes.tobytes(), 9)
logo = zlib.compress(bytes(4), 9)  # 1x1 transparent: the PokeMMO artwork is gone


def c_bytes(data):
    return "\n".join("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 20]) + "," for i in range(0, len(data), 20))


keys = sorted(records)
code_rows = ", ".join(f"0x{k:08x}u" for k in keys)
code_lines = "\n".join("    " + ", ".join(f"0x{k:08x}u" for k in keys[i:i + 6]) + "," for i in range(0, len(keys), 6))
glyph_lines = "\n".join(
    f"    {{{r[0]}, {r[1]}, {r[2]}, {r[3]}, {r[4]:.3f}f, {r[5]:.3f}f}}," for r in (records[k] for k in keys))

(ROOT / "loader/src/overlay_assets.c").write_text(f"""// SPDX-License-Identifier: GPL-3.0-or-later
// Generated by tools/make_overlay_assets.py from DejaVu Sans and the icons drawn there. Do not edit.
// DejaVu fonts: Bitstream Vera / DejaVu license (see CREDITS.md).
#include "overlay_assets.h"

const unsigned overlay_font_width = {ATLAS_W}, overlay_font_height = {ATLAS_H}, overlay_font_line = {LINE}, overlay_font_ascent = {ascent};
const unsigned overlay_icon_cell = {ICON};
const unsigned overlay_glyph_count = {len(keys)};
const unsigned overlay_glyph_codes[{len(keys)}] = {{
{code_lines}
}};
const OverlayGlyph overlay_glyphs[{len(keys)}] = {{
{glyph_lines}
}};
const unsigned overlay_font_alpha_size = {len(font_alpha)};
const unsigned char overlay_font_alpha[] = {{
{c_bytes(font_alpha)}
}};
const unsigned overlay_logo_width = 1, overlay_logo_height = 1;
const unsigned overlay_logo_rgba_size = {len(logo)};
const unsigned char overlay_logo_rgba[] = {{
{c_bytes(logo)}
}};
""")
(ROOT / "loader/include/overlay_icons.h").write_text(
    "// SPDX-License-Identifier: GPL-3.0-or-later\n// Generated by tools/make_overlay_assets.py. Do not edit.\n#pragma once\n\n"
    "// Icon numbers for overlayIcon(): drawn in the overlay atlas, tinted with the colour given.\nenum {\n"
    + "".join(f"    OVERLAY_ICON_{name} = {i},\n" for i, (name, _) in enumerate(ICONS))
    + f"    OVERLAY_ICON_COUNT = {len(ICONS)}\n}};\n")
atlas.save(ROOT / "build" / "overlay_atlas_preview.png") if (ROOT / "build").exists() else None
print(f"{len(keys)} glyphs, atlas rows used {used_rows}/{ATLAS_H}, font {len(font_alpha)} bytes compressed, line {LINE}")
