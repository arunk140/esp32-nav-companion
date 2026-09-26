#!/usr/bin/env python3
"""Download slippy-map PNG tiles for an offline bbox. Personal use only.

Respects tile-server etiquette: max 2 workers, per-request delay, proper
User-Agent, exponential backoff on 429/5xx, resume by skipping existing files.

Output: {out}/{z}/{x}/{y}.png  (feed to lvgl_map_tile_converter.py)

Usage:
  python3 download_tiles.py --bbox 18.89 18.95 72.80 72.86 --zooms 15 16 17 \\
      --out tiles_mumbai_test
  python3 download_tiles.py --bbox 18.85 19.35 72.70 73.10 --zooms 15 16 17 \\
      --out tiles_mumbai --delay 0.5
"""
import argparse
import math
import os
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

UA = "nav-esp32-personal/1.0 (offline-cache personal testing; Mumbai bbox)"

# SHA-256 prefixes of known block/placeholder tiles. Downloads matching these
# are rejected, not saved.
#   b02c44252dac5a5e = OSM 403 "Access blocked" tile
#   69be4aecf9f814ba = CARTO "API KEY REQUIRED" placeholder
BLOCK_HASHES = {"b02c44252dac5a5e", "69be4aecf9f814ba"}

SUBDOMAINS = ["a", "b", "c", "d"]


def deg2num(lat, lon, z):
    n = 2 ** z
    xtile = int((lon + 180.0) / 360.0 * n)
    lat_rad = math.radians(lat)
    ytile = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    return xtile, ytile


def plan(bbox, zooms):
    lat_min, lat_max, lon_min, lon_max = bbox
    jobs = []
    for z in zooms:
        x0, y1 = deg2num(lat_min, lon_min, z)
        x1, y0 = deg2num(lat_max, lon_max, z)
        n = 2 ** z
        for x in range(max(0, min(x0, x1)), min(n, max(x0, x1)) + 1):
            for y in range(max(0, min(y0, y1)), min(n, max(y0, y1)) + 1):
                jobs.append((z, x, y))
    return jobs


def fetch(url_tmpl, out, job, delay, retries, subdomains=True):
    z, x, y = job
    path = os.path.join(out, str(z), str(x), f"{y}.png")
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return "skip"
    import hashlib
    subs = SUBDOMAINS if ("{s}" in url_tmpl and subdomains) else ["-"]
    url = url_tmpl.format(s=subs[(x + y) % len(subs)], z=z, x=x, y=y)
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    for attempt in range(retries + 1):
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                ctype = r.headers.get_content_type()
                data = r.read()
            if ctype != "image/png":
                raise ValueError(f"bad content-type {ctype}")
            if len(data) < 100:
                raise ValueError("tiny response")
            if hashlib.sha256(data).hexdigest()[:16] in BLOCK_HASHES:
                return "fail:blocked-tile"
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + ".part"
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, path)
            # Validate decodable PNG, else delete.
            try:
                from PIL import Image
                with Image.open(path) as im:
                    im.verify()
            except Exception:
                os.remove(path)
                raise ValueError("undecodable png")
            time.sleep(delay)
            return "ok"
        except Exception as e:
            code = getattr(e, "code", None)
            if code in (429, 403, 500, 502, 503, 504):
                wait = min(120, 5 * (2 ** attempt))
                print(f"  server said {code}, backing off {wait}s", flush=True)
                time.sleep(wait)
            elif attempt < retries:
                time.sleep(2 * (attempt + 1))
            else:
                return f"fail:{code or type(e).__name__}"
    return "fail:retries"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bbox", nargs=4, type=float, required=True,
                    metavar=("LAT_MIN", "LAT_MAX", "LON_MIN", "LON_MAX"))
    ap.add_argument("--zooms", nargs="+", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--url",
                    default="https://{s}.basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}.png",
                    help="tile URL template ({z}/{x}/{y}, optional {s} subdomain). "
                         "Default: CARTO Voyager (free, attribution required: OSM + CARTO). "
                         "MapTiler (100k/mo free, needs key): "
                         "https://api.maptiler.com/maps/streets-v2/{z}/{x}/{y}.png?key=KEY")
    ap.add_argument("--workers", type=int, default=2)
    ap.add_argument("--delay", type=float, default=0.5,
                    help="seconds between requests per worker")
    ap.add_argument("--retries", type=int, default=4)
    a = ap.parse_args()

    if a.workers > 2:
        print("capping workers at 2 (tile etiquette)", flush=True)
        a.workers = 2
    jobs = plan(a.bbox, a.zooms)
    print(f"{len(jobs)} tiles, zooms {a.zooms} -> {a.out}", flush=True)

    done = {"ok": 0, "skip": 0, "fail": 0}
    fails = []
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=a.workers) as ex:
        futs = {ex.submit(fetch, a.url, a.out, j, a.delay, a.retries): j for j in jobs}
        for i, f in enumerate(futs):
            r = f.result()
            k = r if r in done else "fail"
            done[k] += 1
            if k == "fail":
                fails.append((futs[f], r))
            if (i + 1) % 500 == 0 or (i + 1) == len(jobs):
                el = time.time() - t0
                print(f"  {i+1}/{len(jobs)} ok={done['ok']} skip={done['skip']} "
                      f"fail={done['fail']} {el:.0f}s", flush=True)
    print(f"DONE ok={done['ok']} skip={done['skip']} fail={done['fail']}")
    for (z, x, y), r in fails[:20]:
        print(f"  FAILED {z}/{x}/{y}: {r}")
    return 1 if done["fail"] else 0


if __name__ == "__main__":
    sys.exit(main())
