#!/usr/bin/env python3
"""Offline preview of nav_view.c's banner + draw_icon geometry.

Icons are rendered at 100% into a transparent patch, scaled by ICON_PCT around
their centre (equivalent to the C `ip()` transform, which scales strokes too),
then pasted. Approximate text boxes are drawn to check the rounded corner and
the icon/text boundary.

Requires Pillow (`pip install pillow`). Writes tmp/nav_icons_preview.png.
"""
import math
import os
from PIL import Image, ImageDraw

BANNER_W, BANNER_H = 340, 84
RADIUS = 16
ICON_CX, ICON_CY = 44, 42
ICON_PCT = 0.9
W = 13
BG = (0x1B, 0x20, 0x27)
FG = (0xF1, 0xF3, 0xF4)
WARN = (0xF2, 0x8B, 0x82)
TEXT_TOP = (0xC8, 0xC8, 0xC8)

MANEUVERS = [
    "NONE", "CONTINUE", "SLIGHT_LEFT", "LEFT", "SHARP_LEFT", "SLIGHT_RIGHT",
    "RIGHT", "SHARP_RIGHT", "KEEP_LEFT", "KEEP_RIGHT", "UTURN", "ROUNDABOUT",
    "OFF_ROUTE", "ARRIVE",
]


def seg(d, p, x1, y1, x2, y2):
    d.line([(x1, y1), (x2, y2)], fill=p["c"], width=p["w"])
    r = p["w"] / 2
    for (x, y) in ((x1, y1), (x2, y2)):
        d.ellipse([x - r, y - r, x + r, y + r], fill=p["c"])


def arrow_head(d, p, x, y, deg):
    a = math.radians(deg)
    s, c = math.sin(a), math.cos(a)
    tip, back, wid = 22, 12, 13
    d.polygon([(x + s * tip, y - c * tip),
               (x - s * back + c * wid, y + c * back + s * wid),
               (x - s * back - c * wid, y + c * back - s * wid)], fill=p["c"])


def straight(d, p, cx, cy):
    seg(d, p, cx, cy + 34, cx, cy - 12)
    arrow_head(d, p, cx, cy - 14, 0.0)


def draw_icon(d, cx, cy, t):
    name = MANEUVERS[t]
    p = {"c": WARN if name == "OFF_ROUTE" else FG, "w": W}
    if name == "CONTINUE":
        straight(d, p, cx, cy)
    elif name == "SLIGHT_LEFT":
        seg(d, p, cx, cy + 36, cx, cy - 6); seg(d, p, cx, cy - 6, cx - 16, cy - 22)
        arrow_head(d, p, cx - 18, cy - 24, -40.0)
    elif name == "LEFT":
        seg(d, p, cx, cy + 36, cx, cy - 8); seg(d, p, cx, cy - 8, cx - 18, cy - 8)
        arrow_head(d, p, cx - 20, cy - 8, -90.0)
    elif name == "SHARP_LEFT":
        seg(d, p, cx, cy + 36, cx, cy - 6); seg(d, p, cx, cy - 6, cx - 20, cy + 10)
        arrow_head(d, p, cx - 22, cy + 12, -130.0)
    elif name == "SLIGHT_RIGHT":
        seg(d, p, cx, cy + 36, cx, cy - 6); seg(d, p, cx, cy - 6, cx + 16, cy - 22)
        arrow_head(d, p, cx + 18, cy - 24, 40.0)
    elif name == "RIGHT":
        seg(d, p, cx, cy + 36, cx, cy - 8); seg(d, p, cx, cy - 8, cx + 18, cy - 8)
        arrow_head(d, p, cx + 20, cy - 8, 90.0)
    elif name == "SHARP_RIGHT":
        seg(d, p, cx, cy + 36, cx, cy - 6); seg(d, p, cx, cy - 6, cx + 20, cy + 10)
        arrow_head(d, p, cx + 22, cy + 12, 130.0)
    elif name == "KEEP_LEFT":
        seg(d, p, cx + 10, cy + 36, cx + 10, cy - 6); seg(d, p, cx + 10, cy - 6, cx - 18, cy - 6)
        arrow_head(d, p, cx - 20, cy - 6, -90.0)
    elif name == "KEEP_RIGHT":
        seg(d, p, cx - 10, cy + 36, cx - 10, cy - 6); seg(d, p, cx - 10, cy - 6, cx + 18, cy - 6)
        arrow_head(d, p, cx + 20, cy - 6, 90.0)
    elif name == "UTURN":
        seg(d, p, cx + 14, cy + 36, cx + 14, cy - 14)
        seg(d, p, cx + 14, cy - 14, cx - 10, cy - 14)
        seg(d, p, cx - 10, cy - 14, cx - 10, cy + 8)
        arrow_head(d, p, cx - 10, cy + 10, 180.0)
    elif name == "ROUNDABOUT":
        d.arc([cx - 22, cy - 22, cx + 22, cy + 22], 0, 270, fill=p["c"], width=W)
        seg(d, p, cx + 12, cy - 12, cx + 26, cy - 22)
        arrow_head(d, p, cx + 27, cy - 23, 40.0)
    elif name == "OFF_ROUTE":
        seg(d, p, cx, cy - 28, cx, cy + 2)
        d.ellipse([cx - W, cy + 14, cx + W, cy + 14 + W * 2], fill=p["c"])
    else:
        d.ellipse([cx - 26, cy - 26, cx + 26, cy + 26], fill=p["c"])


SCALE = 2
pad = 4
img = Image.new("RGB", (BANNER_W * SCALE, (BANNER_H * SCALE + pad) * len(MANEUVERS)), (20, 20, 20))
for i, name in enumerate(MANEUVERS):
    cell = Image.new("RGB", (BANNER_W, BANNER_H), (10, 10, 10))
    d = ImageDraw.Draw(cell)
    d.rounded_rectangle([0, 0, BANNER_W - 1, BANNER_H - 1], radius=RADIUS, fill=BG)

    # icon: render large, scale around its centre, paste
    big = Image.new("RGBA", (200, 200), (0, 0, 0, 0))
    draw_icon(ImageDraw.Draw(big), 100, 100, i)
    n = int(200 * ICON_PCT)
    small = big.resize((n, n), Image.LANCZOS)
    cell.paste(small, (ICON_CX - n // 2, ICON_CY - n // 2), small)

    # approximate text boxes: street 20pt@ (88,10), dist 28pt@ (88,40),
    # remaining 14pt right-aligned ending at x=328, y=52
    d.rectangle([88, 10, 220, 34], outline=(70, 90, 70))
    d.rectangle([88, 40, 190, 68], outline=(90, 90, 70))
    d.rectangle([230, 52, 328, 68], outline=(90, 70, 70))
    d.line([(88, 0), (88, BANNER_H)], fill=(90, 60, 60))
    cell = cell.resize((BANNER_W * SCALE, BANNER_H * SCALE), Image.NEAREST)
    img.paste(cell, (0, i * (BANNER_H * SCALE + pad)))
out = os.path.join("tmp", "nav_icons_preview.png")
os.makedirs("tmp", exist_ok=True)
img.save(out)
print("wrote", out, img.size)
