#!/usr/bin/env python3
"""Drive the device's turn-by-turn banner from a PC over BLE.

No OsmAnd and no phone are involved: this writes the NAV state/TEXT payloads
straight to the characteristic, so the banner and every maneuver icon can be
exercised in isolation. Wire format lives in firmware_v2/main/ble_service.h.

Precondition: the board must be on its map screen, i.e. already paired. If it
shows the QR/pair screen, `settings_paired()` is false and `nav_view` does not
exist yet. Also make sure the phone's NavService is not connected - NimBLE
accepts one central at a time and the phone will steal the connection.

Examples
  python3 tools/nav_mock.py icons                 # all 14 maneuvers, 3 s each
  python3 tools/nav_mock.py one LEFT --dist 250 --street "MG Road"
  python3 tools/nav_mock.py route --street "MG Road"
  python3 tools/nav_mock.py live LEFT --duration 120   # hold one instruction
  python3 tools/nav_mock.py --addr AA:BB:CC:DD:EE:FF icons

Requires bleak (`pip install bleak`). Scan by name prefix "NAV-" by default.
"""
import argparse
import asyncio
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

SVC = "e8dffd56-e8b9-4279-9bff-1e0d2220d776"
NAV_CHR = "7c3d1f92-5a6b-4d18-9e2f-8b41c05d7a63"  # NAV_CHR_STATE_UUID

STATE = 0x01
TEXT = 0x02
DEST = 0x03
F_ACTIVE = 0x01
# Our maneuver enum, index == code (ble_service.h / OsmAndLink.kt).
MANEUVERS = [
    "NONE", "CONTINUE", "SLIGHT_LEFT", "LEFT", "SHARP_LEFT", "SLIGHT_RIGHT",
    "RIGHT", "SHARP_RIGHT", "KEEP_LEFT", "KEEP_RIGHT", "UTURN", "ROUNDABOUT",
    "OFF_ROUTE", "ARRIVE",
]
CODE = {name: i for i, name in enumerate(MANEUVERS)}


def state_payload(maneuver, dist, after, after_dist, remaining, remaining_s,
                  flags=F_ACTIVE):
    # 12 B on the wire: u8 type, u8 flags, u8 maneuver, u16 dist_m,
    # u8 after_maneuver, u16 after_dist_m, u16 remaining_m, u16 remaining_s.
    return struct.pack("<BBBHBHHH", STATE, flags, maneuver & 0xFF,
                       max(0, min(0xFFFF, dist)), after & 0xFF,
                       max(0, min(0xFFFF, after_dist)),
                       max(0, min(0xFFFF, remaining)),
                       max(0, min(0xFFFF, remaining_s)))


def text_payload(name):
    b = name.encode("latin-1", "ignore")[:48]  # NAV_TEXT_MAX
    return bytes([TEXT, len(b)]) + b


def dest_payload(lat=None, lon=None):
    # 10 B: u8 type, u8 present, i32 lat_e7, i32 lon_e7.
    if lat is None or lon is None:
        return struct.pack("<BBii", DEST, 0, 0, 0)
    return struct.pack("<BBii", DEST, 1, int(lat * 1e7), int(lon * 1e7))


async def find(timeout, addr):
    if addr:
        dev = await BleakScanner.find_device_by_address(addr, timeout=timeout)
        if not dev:
            raise SystemExit(f"no device at {addr}")
        return dev
    dev = await BleakScanner.find_device_by_filter(
        lambda d, a: (d.name or "").startswith("NAV-"), timeout=timeout)
    if not dev:
        raise SystemExit("no NAV- device advertising (asleep? phone holding it?)")
    return dev


async def send_text(c, name):
    await c.write_gatt_char(NAV_CHR, text_payload(name), response=False)


async def send_state(c, **kw):
    await c.write_gatt_char(NAV_CHR, state_payload(**kw), response=False)


async def send_dest(c, args):
    if args.dest_lat is not None and args.dest_lon is not None:
        await c.write_gatt_char(NAV_CHR, dest_payload(args.dest_lat, args.dest_lon),
                                response=False)
        print(f"  dest -> {args.dest_lat},{args.dest_lon}")


async def mode_icons(c, args):
    # Every icon in the enum, so each one can be eyeballed on hardware. The
    # street label stays fixed on purpose: only the icon should change.
    await send_text(c, args.street or "Test Road")
    await send_dest(c, args)
    for i, name in enumerate(MANEUVERS):
        await send_state(c, maneuver=i, dist=args.dist, after=(i + 1) % 14,
                         after_dist=800, remaining=5200 - i * 100,
                         remaining_s=600 - i * 5)
        print(f"  {i:2} {name}")
        await asyncio.sleep(args.hold)


async def mode_one(c, args):
    code = CODE[args.maneuver.upper()]
    if args.street:
        await send_text(c, args.street)
    await send_dest(c, args)
    await send_state(c, maneuver=code, dist=args.dist, after=0, after_dist=0,
                     remaining=args.remaining, remaining_s=args.remaining_s)
    print(f"maneuver={args.maneuver.upper()}({code}) dist={args.dist} "
          f"remaining={args.remaining}m/{args.remaining_s}s street={args.street!r}")
    await asyncio.sleep(args.hold)


async def mode_route(c, args):
    # Simulated approach: CONTINUE closing on a left turn, then next leg.
    await send_text(c, args.street or "MG Road")
    await send_dest(c, args)
    steps = [
        ("CONTINUE", 320, "RIGHT", 450, 2500, 240),
        ("CONTINUE", 180, "RIGHT", 450, 2400, 220),
        ("CONTINUE", 60, "RIGHT", 450, 2300, 200),
        ("LEFT", 40, "SLIGHT_RIGHT", 300, 2200, 190),
        ("LEFT", 0, "SLIGHT_RIGHT", 300, 2100, 180),
        ("SLIGHT_RIGHT", 150, "ROUNDABOUT", 900, 1900, 160),
        ("ROUNDABOUT", 30, "CONTINUE", 600, 1700, 140),
        ("KEEP_LEFT", 120, "UTURN", 0, 1400, 110),
        ("UTURN", 20, "ARRIVE", 0, 1100, 90),
        ("ARRIVE", 0, "NONE", 0, 0, 0),
    ]
    remaining = 2500
    for name, dist, after, after_d, rem, rem_s in steps:
        await send_state(c, maneuver=CODE[name], dist=dist, after=CODE[after],
                         after_dist=after_d, remaining=rem, remaining_s=rem_s)
        print(f"  {name:12} in {dist:>4} m  then {after:12} {after_d:>4} m  "
              f"left {rem} m")
        await asyncio.sleep(args.hold)
    # End of navigation: flags without ACTIVE hides the banner immediately.
    await send_state(c, maneuver=0, dist=0, after=0, after_dist=0,
                     remaining=0, remaining_s=0, flags=0)
    print("  stopped (banner hidden)")


async def mode_live(c, args):
    # Keep one instruction alive (STATE must repeat or the banner goes stale
    # after STALE_MS) so it can be tapped on hardware.
    code = CODE[args.maneuver.upper()]
    await send_text(c, args.street or "MG Road")
    await send_dest(c, args)
    t0 = time.time()
    while time.time() - t0 < args.duration:
        await send_state(c, maneuver=code, dist=args.dist, after=0, after_dist=0,
                         remaining=args.remaining, remaining_s=args.remaining_s)
        await asyncio.sleep(2.0)
    print("live done")


async def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["icons", "one", "route", "live"])
    ap.add_argument("maneuver", nargs="?", default="CONTINUE",
                    help="for mode 'one' or 'live'")
    ap.add_argument("--street", default="")
    ap.add_argument("--dest-lat", type=float, default=None,
                    help="destination latitude (with --dest-lon draws a pin)")
    ap.add_argument("--dest-lon", type=float, default=None,
                    help="destination longitude")
    ap.add_argument("--dist", type=int, default=250)
    ap.add_argument("--remaining", type=int, default=2500)
    ap.add_argument("--remaining-s", type=int, default=180)
    ap.add_argument("--hold", type=float, default=3.0, help="seconds per step")
    ap.add_argument("--duration", type=float, default=60.0,
                    help="seconds for mode 'live'")
    ap.add_argument("--addr", default=None, help="BLE address instead of scan")
    ap.add_argument("--scan-timeout", type=float, default=10.0)
    args = ap.parse_args()

    dev = await find(args.scan_timeout, args.addr)
    print(f"connecting to {dev.address} ({dev.name})")
    async with BleakClient(dev, timeout=20.0) as c:
        if not c.is_connected:
            raise SystemExit("connect failed")
        try:
            await c.write_gatt_char(NAV_CHR, text_payload(args.street or "start"),
                                    response=True)
        except Exception as e:
            print(f"nav characteristic not writable: {e}", file=sys.stderr)
            print("(is the firmware flashed and the board on the map screen?)",
                  file=sys.stderr)
            raise SystemExit(2)
        if args.mode == "icons":
            await mode_icons(c, args)
        elif args.mode == "one":
            await mode_one(c, args)
        elif args.mode == "live":
            await mode_live(c, args)
        else:
            await mode_route(c, args)
        # Leave a final live state only for single-shot modes; icons/route end
        # on their own. Keep the banner from going stale while still attached.
        await asyncio.sleep(0.2)
    print("disconnected")


if __name__ == "__main__":
    asyncio.run(main())
