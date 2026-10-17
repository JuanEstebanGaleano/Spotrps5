#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Spotrps5 contributors
"""Draws Spotrps5's own artwork (an original design: no third-party logo or mascot) and writes

  assets/branding-spotify/icon0.png       512x512 home screen icon
  assets/branding-spotify/background.dds  3840x2160 BC7 home screen background (the build uses it as pic0.dds and pic1.dds)

  python3 tools/make_branding_spotify.py [--preview DIR]

Needs Pillow and NumPy; the results are committed, so the Docker build needs neither.
"""
import math
import random
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets/branding-spotify"

INK = (7, 6, 18)           # near black with a violet tint
VIOLET = (46, 16, 96)
MAGENTA = (255, 54, 160)
GREEN = (36, 255, 140)
CYAN = (40, 210, 255)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def gradient_strip(length, stops):
    """A (length, 3) uint8 colour ramp through `stops` [(position 0..1, colour)]."""
    out = np.zeros((length, 3), np.float32)
    ts = np.linspace(0, 1, length)
    for c in range(3):
        out[:, c] = np.interp(ts, [s[0] for s in stops], [s[1][c] for s in stops])
    return out


def radial(width, height, centre, radius, colour, strength=1.0, power=2.0):
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    d = np.hypot(x - centre[0], y - centre[1]) / radius
    a = np.clip(1.0 - d, 0.0, 1.0) ** power * strength
    layer = np.zeros((height, width, 3), np.float32)
    layer[:] = colour
    return layer, a


def add_glow(base, centre, radius, colour, strength=1.0, power=2.0):
    layer, a = radial(base.shape[1], base.shape[0], centre, radius, colour, strength, power)
    base += layer * a[..., None]


def vinyl(size, label_text_triangle=True):
    """An RGBA record: grooves, a light sweep, a colourful label with a play triangle. size in px (square)."""
    S = size
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    y, x = np.mgrid[0:S, 0:S].astype(np.float32)
    cx = cy = (S - 1) / 2
    r = np.hypot(x - cx, y - cy) / (S / 2)
    ang = np.arctan2(y - cy, x - cx)
    body = np.zeros((S, S, 3), np.float32)
    base = 14 + 6 * np.sin(r * 140)                      # fine grooves
    sweep = np.clip(np.cos(ang * 2 - 0.9), 0, 1) ** 6 * 46  # two light sweeps
    shade = base + sweep * np.clip((r - 0.32) / 0.2, 0, 1)
    body[...] = shade[..., None] * np.array([0.9, 0.85, 1.25], np.float32)
    # glowing rim: green to cyan
    rim = np.clip((r - 0.93) / 0.07, 0, 1)
    ramp = (np.cos(ang - 0.6) * 0.5 + 0.5)[..., None]
    rimcol = np.array(GREEN, np.float32) * (1 - ramp) + np.array(CYAN, np.float32) * ramp
    body += rimcol * (rim ** 1.5)[..., None] * 0.9
    # label: magenta -> violet
    lab = r < 0.34
    t = np.clip(r / 0.34, 0, 1)
    labcol = np.array(MAGENTA, np.float32) * (1 - t[..., None]) + np.array((140, 40, 255), np.float32) * t[..., None]
    labcol = labcol * (0.75 + 0.25 * np.cos(ang + 0.8))[..., None]
    body[lab] = labcol[lab]
    ring = (np.abs(r - 0.355) < 0.012)
    body[ring] = (255, 255, 255)
    alpha = (r <= 1.0).astype(np.float32)
    arr = np.dstack([np.clip(body, 0, 255), alpha * 255]).astype(np.uint8)
    img = Image.fromarray(arr, "RGBA")
    if label_text_triangle:
        d = ImageDraw.Draw(img)
        R = S * 0.34 * 0.5
        pts = [(cx - R * 0.55, cy - R * 0.85), (cx - R * 0.55, cy + R * 0.85), (cx + R * 0.95, cy)]
        d.polygon(pts, fill=(255, 255, 255, 255))
        hole = S * 0.012
        d.ellipse((cx + R * 1.6 - hole, cy - hole, cx + R * 1.6 + hole, cy + hole), fill=(0, 0, 0, 0))
    # a soft highlight blob
    return img


def arcs(size, centre, radii, width, span, start, colour_a, colour_b):
    """Sound-wave arcs as an RGBA layer."""
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for i, rad in enumerate(radii):
        t = i / max(1, len(radii) - 1)
        col = lerp(colour_a, colour_b, t) + (int(255 * (1 - 0.45 * t)),)
        box = (centre[0] - rad, centre[1] - rad, centre[0] + rad, centre[1] + rad)
        d.arc(box, start - span / 2, start + span / 2, fill=col, width=int(width * (1 - 0.25 * t)))
    return layer


def to_rgb(arr):
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")


def dither(arr, amount=1.4, seed=1):
    rng = np.random.default_rng(seed)
    return arr + rng.normal(0, amount, arr.shape).astype(np.float32)


def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, size - 1, size - 1), radius, fill=255)
    return m


# ---- icon -------------------------------------------------------------------------------------------------------------------------
def make_icon():
    S = 2048  # drawn large, shrunk to 512
    base = np.zeros((S, S, 3), np.float32)
    base[:] = INK
    add_glow(base, (S * 0.30, S * 0.25), S * 0.95, VIOLET, 1.0, 1.6)
    add_glow(base, (S * 0.82, S * 0.85), S * 0.80, (150, 20, 110), 0.85, 1.8)
    add_glow(base, (S * 0.35, S * 0.55), S * 0.55, (10, 120, 90), 0.55, 2.0)
    bg = to_rgb(dither(base)).convert("RGBA")

    rec = vinyl(int(S * 0.72))
    # glow behind the record
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    c = (int(S * 0.40), int(S * 0.49))
    R = int(S * 0.36)
    gd.ellipse((c[0] - R, c[1] - R, c[0] + R, c[1] + R), fill=GREEN + (200,))
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.06))
    bg.alpha_composite(glow)
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    sd.ellipse((c[0] - R + S * 0.02, c[1] - R + S * 0.035, c[0] + R + S * 0.02, c[1] + R + S * 0.035), fill=(0, 0, 0, 170))
    shadow = shadow.filter(ImageFilter.GaussianBlur(S * 0.025))
    bg.alpha_composite(shadow)
    bg.alpha_composite(rec, (c[0] - rec.width // 2, c[1] - rec.height // 2))

    # sound waves leaving the record on the right (the console 'sending out' music)
    w = arcs(S, (c[0] + int(R * 0.12), c[1]), [int(R * 1.15), int(R * 1.33)], int(S * 0.034), 56, 0, CYAN, MAGENTA)
    wglow = w.filter(ImageFilter.GaussianBlur(S * 0.012))
    bg.alpha_composite(wglow)
    bg.alpha_composite(w)

    # equaliser bars: bottom right, over the record's edge
    bars = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    bd = ImageDraw.Draw(bars)
    heights = [0.07, 0.13, 0.095, 0.17, 0.11]
    x0 = int(S * 0.56)
    bw = int(S * 0.044)
    gap = int(S * 0.024)
    for i, h in enumerate(heights):
        hh = int(S * h)
        x = x0 + i * (bw + gap)
        y1 = int(S * 0.915)
        col = lerp(GREEN, CYAN, i / 4)
        bd.rounded_rectangle((x, y1 - hh, x + bw, y1), bw // 2, fill=col + (255,))
    bg.alpha_composite(bars.filter(ImageFilter.GaussianBlur(S * 0.012)))
    bg.alpha_composite(bars)

    # glass edge highlight
    edge = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ed = ImageDraw.Draw(edge)
    ed.rounded_rectangle((S * 0.012, S * 0.012, S * 0.988, S * 0.988), int(S * 0.2), outline=(255, 255, 255, 60), width=int(S * 0.008))
    bg.alpha_composite(edge)
    icon = bg.convert("RGB").resize((512, 512), Image.LANCZOS)
    return icon


# ---- background -------------------------------------------------------------------------------------------------------------------
def make_background(width=3840, height=2160):
    base = np.zeros((height, width, 3), np.float32)
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    # diagonal base gradient
    t = np.clip((x / width * 0.55 + y / height * 0.45), 0, 1)
    ramp = gradient_strip(512, [(0.0, (14, 8, 38)), (0.5, (8, 8, 28)), (1.0, (5, 4, 14))])
    base[:] = ramp[(t * 511).astype(int)]
    add_glow(base, (width * 0.82, height * 0.30), width * 0.55, (30, 190, 120), 0.55, 2.2)
    add_glow(base, (width * 0.12, height * 0.88), width * 0.50, (170, 30, 130), 0.60, 2.0)
    add_glow(base, (width * 0.55, height * 0.05), width * 0.45, (70, 40, 190), 0.55, 2.0)
    add_glow(base, (width * 0.35, height * 0.55), width * 0.35, (20, 90, 160), 0.30, 2.2)

    img = to_rgb(dither(base, 1.2)).convert("RGBA")

    # equaliser skyline along the bottom, with a soft reflection
    random.seed(7)
    bars = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    bd = ImageDraw.Draw(bars)
    bw, gap = 34, 20
    n = width // (bw + gap)
    base_y = int(height * 0.935)
    for i in range(n):
        x0 = i * (bw + gap) + gap // 2
        phase = i / n
        env = 0.5 + 0.5 * math.sin(phase * math.pi * 3.2 + 0.7) * math.sin(phase * math.pi * 1.1 + 0.3)
        hgt = int(height * (0.035 + 0.17 * env * (0.55 + 0.45 * random.random())))
        # fade the skyline out towards the right, where the record is
        fade = 1.0 - 0.80 * smoothstep(0.55, 0.95, phase)
        hgt = int(hgt * fade) + 12
        col = ramp3(phase)
        bd.rounded_rectangle((x0, base_y - hgt, x0 + bw, base_y), bw // 2, fill=col + (230,))
    img.alpha_composite(bars.filter(ImageFilter.GaussianBlur(16)))
    img.alpha_composite(bars)


    # concentric rings around the big record
    rc = (int(width * 0.74), int(height * 0.50))
    rings = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    rd = ImageDraw.Draw(rings)
    for i in range(1, 9):
        rad = int(height * (0.34 + 0.115 * i))
        a = int(70 * (1 - i / 10))
        col = lerp(GREEN, MAGENTA, i / 9)
        rd.ellipse((rc[0] - rad, rc[1] - rad, rc[0] + rad, rc[1] + rad), outline=col + (a,), width=4 if i % 2 else 2)
    img.alpha_composite(rings.filter(ImageFilter.GaussianBlur(1.2)))

    # the record
    rec = vinyl(int(height * 0.74))
    glow = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    R = rec.width // 2
    gd.ellipse((rc[0] - R, rc[1] - R, rc[0] + R, rc[1] + R), fill=GREEN + (170,))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(height * 0.05)))
    sh = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse((rc[0] - R + 30, rc[1] - R + 60, rc[0] + R + 30, rc[1] + R + 60), fill=(0, 0, 0, 190))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(40)))
    img.alpha_composite(rec, (rc[0] - R, rc[1] - R))
    w = arcs(max(width, height), (rc[0] + int(R * 0.1), rc[1]), [int(R * 1.10), int(R * 1.22), int(R * 1.34)], 20, 58, 0, CYAN, MAGENTA)
    w = w.crop((0, 0, width, height)) if w.size != (width, height) else w
    img.alpha_composite(w.filter(ImageFilter.GaussianBlur(8)))
    img.alpha_composite(w)

    # dust: a few soft sparkles
    sp = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sp)
    for _ in range(260):
        px, py = random.randint(0, width), random.randint(0, int(height * 0.85))
        r = random.choice([2, 2, 3, 4, 6])
        col = random.choice([GREEN, CYAN, MAGENTA, (255, 255, 255)])
        sd.ellipse((px - r, py - r, px + r, py + r), fill=col + (random.randint(60, 190),))
    img.alpha_composite(sp.filter(ImageFilter.GaussianBlur(1.6)))

    # vignette
    arr = np.asarray(img.convert("RGB")).astype(np.float32)
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    v = np.hypot((xx - width / 2) / (width / 2), (yy - height / 2) / (height / 2))
    arr *= (1.0 - 0.35 * np.clip(v - 0.55, 0, 1) ** 1.6)[..., None]
    return to_rgb(dither(arr, 1.1, 2))


def ramp3(t):
    t = min(1.0, max(0.0, t))
    return lerp(MAGENTA, (150, 70, 255), t / 0.4) if t < 0.4 else lerp((150, 70, 255), CYAN, (t - 0.4) / 0.3) if t < 0.7 else lerp(CYAN, GREEN, (t - 0.7) / 0.3)


def smoothstep(a, b, x):
    t = min(1.0, max(0.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


# ---- BC7 (mode 6, one mip) ------------------------------------------------------------------------------------------------------
def encode_bc7(image):
    rgba = np.asarray(image.convert("RGBA"), dtype=np.int32)
    H, W = rgba.shape[:2]
    blocks = rgba.reshape(H // 4, 4, W // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    low, high = blocks.min(axis=1), blocks.max(axis=1)
    e0, e1 = (low >> 1), (high >> 1)
    q0, q1 = (e0 << 1).astype(np.float32), (e1 << 1).astype(np.float32)
    axis = q1 - q0
    length = (axis * axis).sum(axis=1)
    tt = ((blocks - q0[:, None, :]) * axis[:, None, :]).sum(axis=2) / np.where(length > 0, length, 1)[:, None]
    indices = np.clip(np.rint(tt * 15), 0, 15).astype(np.int64)
    swap = indices[:, 0] >= 8
    e0[swap], e1[swap] = e1[swap].copy(), e0[swap].copy()
    indices[swap] = 15 - indices[swap]
    bits = [(np.full(len(blocks), 1 << 6, np.int64), 7)]
    for channel in range(4):
        bits.append((e0[:, channel].astype(np.int64), 7))
        bits.append((e1[:, channel].astype(np.int64), 7))
    bits.append((np.zeros(len(blocks), np.int64), 1))
    bits.append((np.zeros(len(blocks), np.int64), 1))
    for i in range(16):
        bits.append((indices[:, i], 3 if i == 0 else 4))
    lo = np.zeros(len(blocks), np.uint64)
    hi = np.zeros(len(blocks), np.uint64)
    offset = 0
    for value, width in bits:
        for b in range(width):
            bit = ((value >> b) & 1).astype(np.uint64)
            position = offset + b
            if position < 64:
                lo |= bit << np.uint64(position)
            else:
                hi |= bit << np.uint64(position - 64)
        offset += width
    assert offset == 128
    payload = np.stack([lo, hi], axis=1).astype("<u8").tobytes()
    header = b"DDS " + struct.pack("<7I", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, H, W, len(payload), 0, 1)
    header += b"\0" * 44 + struct.pack("<2I", 32, 0x4) + b"DX10" + b"\0" * 20 + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    header += struct.pack("<5I", 98, 3, 0, 1, 0)
    return header + payload


def decode_preview(dds_bytes, W, H):
    """Not a decoder: the preview is the source image itself."""
    return None


if __name__ == "__main__":
    preview = None
    if "--preview" in sys.argv:
        preview = Path(sys.argv[sys.argv.index("--preview") + 1])
        preview.mkdir(parents=True, exist_ok=True)
    icon = make_icon()
    background = make_background()
    if preview:
        icon.save(preview / "icon0.png")
        background.resize((1920, 1080), Image.LANCZOS).save(preview / "background.png")
    else:
        OUT.mkdir(parents=True, exist_ok=True)
        icon.save(OUT / "icon0.png", optimize=True)
        (OUT / "background.dds").write_bytes(encode_bc7(background))
        print("wrote", OUT / "icon0.png", "and", OUT / "background.dds")
