#!/usr/bin/env python3
"""Generate synthetic 256x256 map tiles for firmware bring-up.

Draws a grid + tile coordinates + a fake coastline so the render path,
panning, zoom and marker code can be validated WITHOUT real map data
(real tiles need a MapTiler key — see tools/download_tiles.py).

Output PNGs mirror the tile layout, then feed them to the upstream
converter:
  python3 make_test_tiles.py --bbox 18.89 18.95 72.80 72.86 --zooms 15 16 17 \\
      --out /tmp/opencode/fake_tiles
  python3 converter.py --input /tmp/opencode/fake_tiles --output fake_bins
"""
import argparse
import math
import os
import sys

from PIL import Image, ImageDraw


def deg2num(lat, lon, z):
    n = 2 ** z
    xtile = int((lon + 180.0) / 360.0 * n)
    lat_rad = math.radians(lat)
    ytile = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    return xtile, ytile


def draw_tile(z, x, y):
    im = Image.new("RGB", (256, 256), (235, 235, 220))
    d = ImageDraw.Draw(im)
    # fake "roads": diagonal + cross, colour varies with coords for orientation
    road = (200 + (x * 37 + y * 11) % 40, 190, 170)
    d.line([(0, y % 256), (256, (y + 90) % 256)], fill=road, width=10)
    d.line([(x % 256, 0), ((x + 60) % 256, 256)], fill=(180, 180, 200), width=6)
    # fake water blob in some tiles
    if (x + y) % 3 == 0:
        d.ellipse([180, 180, 256, 256], fill=(140, 180, 220))
    # border + label
    d.rectangle([0, 0, 255, 255], outline=(90, 90, 90), width=3)
    d.text((12, 12), f"z{z}", fill=(20, 20, 20))
    d.text((12, 30), f"{x}", fill=(20, 20, 20))
    d.text((12, 48), f"{y}", fill=(20, 20, 20))
    # center crosshair
    d.line([(118, 128), (138, 128)], fill=(200, 40, 40), width=2)
    d.line([(128, 118), (128, 138)], fill=(200, 40, 40), width=2)
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bbox", nargs=4, type=float, required=True,
                    metavar=("LAT_MIN", "LAT_MAX", "LON_MIN", "LON_MAX"))
    ap.add_argument("--zooms", nargs="+", type=int, required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    lat_min, lat_max, lon_min, lon_max = a.bbox
    n = 0
    for z in a.zooms:
        x0, y1 = deg2num(lat_min, lon_min, z)
        x1, y0 = deg2num(lat_max, lon_max, z)
        lim = 2 ** z
        for x in range(max(0, min(x0, x1)), min(lim, max(x0, x1)) + 1):
            for y in range(max(0, min(y0, y1)), min(lim, max(y0, y1)) + 1):
                p = os.path.join(a.out, str(z), str(x), f"{y}.png")
                os.makedirs(os.path.dirname(p), exist_ok=True)
                draw_tile(z, x, y).save(p)
                n += 1
    print(f"wrote {n} fake tiles -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
