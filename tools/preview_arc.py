#!/usr/bin/env python3
"""Preview of the curved destination path (mirrors draw_dest_line in
map_view.c). Confirms the bow direction, dot spacing and phase march offline."""
import math
from PIL import Image, ImageDraw

LINK_DASH = 5
LINK_GAP = 6
LINK_W = 3
LINK_BOW = 0.22
LINK_SEG = 96
LINK_COL = (0x8A, 0x90, 0x96)

W, H = 420, 420
img = Image.new("RGB", (W * 2, H), (0x1B, 0x20, 0x27))
d = ImageDraw.Draw(img)


def draw_path(x0, y0, x1, y1, phase, ox):
    dx, dy = x1 - x0, y1 - y0
    ln = math.hypot(dx, dy)
    if ln < 4:
        return
    ux, uy = dx / ln, dy / ln
    px_, py_ = -uy, ux
    mx, my = (x0 + x1) / 2, (y0 + y1) / 2
    bow = ln * LINK_BOW
    cxp, cyp = mx + px_ * bow, my + py_ * bow
    period = LINK_DASH + LINK_GAP
    p = int(phase) % period
    s_off = 0.0
    prev = (x0, y0)
    for i in range(LINK_SEG + 1):
        t = i / LINK_SEG
        omt = 1 - t
        bx = omt * omt * x0 + 2 * omt * t * cxp + t * t * x1
        by = omt * omt * y0 + 2 * omt * t * cyp + t * t * y1
        if i > 0:
            seg = math.hypot(bx - prev[0], by - prev[1])
            s = s_off
            s_off += seg
            off = math.fmod(s + p, period)
            if 0 <= off < LINK_DASH:
                r = LINK_W / 2
                d.ellipse([ox + bx - r, by - r, ox + bx + r, by + r], fill=LINK_COL)
        prev = (bx, by)
    # markers for reference
    d.ellipse([ox + x0 - 7, y0 - 7, ox + x0 + 7, y0 + 7], fill=(0x1A, 0x73, 0xE8))
    d.ellipse([ox + x1 - 7, y1 - 7, ox + x1 + 7, y1 + 7], fill=(0xE8, 0x71, 0x0A))


# two phases side by side, same endpoints, to see the march
draw_path(60, 360, 340, 90, 0, 0)
draw_path(60, 360, 340, 90, 5, W)
img.save("tmp/nav_arc_preview.png")
print("wrote tmp/nav_arc_preview.png")
