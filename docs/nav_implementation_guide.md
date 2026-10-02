# Nav Implementation Guide — OsmAnd turn-by-turn over BLE

Status: **working end to end, banner only.** Written as a record of what was tried,
in the order it happened — including the dead ends, because the failures here were
almost all invisible from the symptom and cost far more time than the happy path.

Code as of this document (all uncommitted at time of writing):
- `firmware_v2/main/nav_view.c` / `.h` — banner + maneuver icons
- `firmware_v2/main/ble_nav.c` — nav characteristic handler
- `firmware_v2/main/ble_service.h` — the contract
- `app/.../OsmAndLink.kt` — bind + poll + decode
- `app/.../NavBridge.kt` — encoder
- `app/.../net/osmand/aidlapi/` — vendored GPL-3.0 AIDL client

---

## 1. Design decisions, and why

| Decision | Reason |
| --- | --- |
| **No route line** | OsmAnd's AIDL exposes no route/polyline geometry. Dropped entirely rather than approximated. |
| **Banner only** | Route geometry was the only thing that would need the line. No geometry → nothing to draw beyond the instruction. |
| **Icons drawn on the board** | LVGL primitives (rounded-rect strokes + triangle heads). Nothing crosses the BLE link, crisp at any size, no bitmap assets to license. |
| **Phone owns routing and text** | OsmAnd does route calculation; we only need the next instruction. Keeps the firmware dumb. |
| **Own maneuver enum, not OsmAnd's `TurnType`** | OsmAnd's `TurnType` is an internal implementation detail. Coupling our wire format to another project's internal numbering is a future breakage. |

---

## 2. Data source: OsmAnd AIDL

Facts established by reading the OsmAnd source (sparse clone at `/tmp/opencode/OsmAnd`)
and confirmed on-device against OsmAnd 5.3.10:

- Service action: `net.osmand.aidl.OsmandAidlServiceV2`, exported, **no permission**.
- Descriptor: `net.osmand.aidlapi.IOsmAndAidlInterface`.
- `getAppInfo()` is the rich channel — it returns `turnInfo` (a Bundle) plus
  `leftDistance` / `leftTime`.
- `registerForNavigationUpdates()` is only a **wake-up signal**: it gives you
  `distanceTo` + `turnType`, no street name. Not enough on its own; use it as a
  hint to poll `getAppInfo()`.
- `turnInfo == null` means **not navigating**.
- Upstream bug worth remembering: `no_speak_next_*` is a duplicate of
  `after_next*`. Use the `after_next*` keys.

Keys actually read out of `turnInfo`:

```
next_turn_type, next_turn_distance, next_turn_name
after_nextturn_type, after_nextturn_distance, after_nextturn_name
leftDistance, leftTime      (on AppInfoParams, not in the bundle)
```

### Manifest / build wiring (all three are required)

```xml
<!-- AndroidManifest.xml -->
<queries>
  <package android:name="net.osmand.plus" />
  <package android:name="net.osmand" />
  <package android:name="net.osmand.dev" />
  <intent>
    <action android:name="net.osmand.aidl.OsmandAidlServiceV2" />
  </intent>
</queries>
```

```kotlin
// build.gradle.kts
buildFeatures { aidl = true }
sourceSets["main"].aidl.srcDir("src/main/java")   // both lines, or nothing compiles
```

Without `<queries>` on Android 11+ the service is invisible and binding silently
fails.

### Licensing consequence

Vendoring OsmAnd's AIDL client makes **the app GPL-3.0**. That was the decision
after weighing it; the AIDL files are small and stable, and the alternative
(hand-writing the parcelable) means re-deriving a wire format that is not
documented anywhere.

---

## 3. The consent gate — the one true external blocker

OsmAnd requires the user to enable **Nav Companion** in *Plugins* before it will
accept a registration. Without it:

```
register rejected (consent?)  → returns -1
```

This is keyed by **package name + signing identity**, so:
- it resets when the debug APK is reinstalled with a different key,
- it does *not* reset on ordinary `adb install -r` with the same key.

Nothing in code can bypass it. Plan for it: detect `register() == -1`, and surface
"Enable Nav Companion in OsmAnd → Plugins" to the user (we set `NAV_F_NO_ACCESS`
and a notification rather than failing silently).

**Testing tip that saved hours:** the probe phase was dead for a while waiting on
this. When consent is granted, `register` returns token `0`; rejected returns `-1`.
That integer is the whole detection mechanism.

---

## 4. Wire protocol

One characteristic, two message types — same pattern as the media channel.

```
UUID 7c3d1f92-5a6b-4d18-9e2f-8b41c05d7a63   (write / write-no-rsp)

0x01 STATE — 12 bytes total, little-endian
  [0]      type = 0x01
  [1]      flags        bit0 NAV_F_ACTIVE, bit1 NAV_F_NO_ACCESS
  [2]      maneuver     NAV_* below
  [3..4]   dist_m       u16, to the maneuver
  [5]      after_maneuver
  [6..7]   after_dist_m  u16
  [8..9]   remaining_m  u16
  [10..11] remaining_s  u16

0x02 TEXT — street name
  [0] type = 0x02
  [1] len
  [2..]  bytes, Latin-1, no NUL
```

STATE goes out on **every** poll (~1 Hz) because distances shrink continuously.
TEXT goes out **only when the name changes**, so a burst of street names cannot
build a queue backlog. Firmware keeps the street sticky (it may arrive after STATE).

Maneuver enum (ours, 0–13): `NONE, CONTINUE, SLIGHT_LEFT, LEFT, SHARP_LEFT,
SLIGHT_RIGHT, RIGHT, SHARP_RIGHT, KEEP_LEFT, KEEP_RIGHT, UTURN, ROUNDABOUT,
OFF_ROUTE, ARRIVE`.

---

## 5. Banner geometry (round screen)

The banner is bottom-centred, and the numbers are not arbitrary. On a 480 px round
panel centred at (240, 240), the visible half-width at depth `dy` from centre is
`sqrt(240² − dy²)`:

| y | visible width |
| --- | --- |
| 400 | ≈ 358 px |
| **410** | **≈ 339 px** |
| 420 | ≈ 317 px |
| 440 | ≈ 249 px |

So: **340 × 84, horizontally centred, bottom edge at y = 410** — the widest box that
still fits inside the bezel. Anything wider gets its corners chopped by the bezel,
which looks like a rendering bug rather than a design choice. Anything lower starts
losing width fast.

Internal layout (banner-local coords): icon centred at (44, 42) at 64 px; text
column starts at x=88, width `340−88−12 = 240`; street name 20 pt at y=10,
distance 28 pt at y=40, remaining distance 14 pt right-aligned at y=52.

**Recentre button is hidden while the banner is up.** It lives at (240, 410) — dead
centre of the banner — and would sit on top of it whenever the user pans away during
navigation. `map_view.c`'s `update_buttons()` now reads `nav_view_active()`, and
`map_view_tick()` watches for the transition so the button comes back when
navigation ends.

---

## 6. What went wrong (the useful part)

### 6.1 Off-by-one payload length → `BufferOverflowException`

Symptom: `java.nio.BufferOverflowException` once per second on the phone, while the
decoded state still looked correct.

Cause: the state payload is **12 bytes on the wire** (a type byte + an 11-byte body).
Both sides declared 11. The app allocated 11 and wrote 12 — overflowing its own
buffer — and the firmware's length check was one byte too lax.

Fix: `NAV_STATE_LEN 12` in `ble_service.h`, `STATE_LEN = 12` in `NavBridge.kt`, with
a comment on each naming the other file. Two mirrored constants of the same name in
two languages will drift; at least make the comment point at the twin.

### 6.2 Hand-reversed NimBLE UUID bytes

NimBLE stores 128-bit UUIDs little-endian, i.e. the string's bytes **reversed**.
Reversing `7c3d1f92-5a6b-4d18-9e2f-8b41c05d7a63` by hand produced `0x0c` where
`0xc0` was required. The characteristic simply did not exist in the GATT table, so
the phone's writes vanished with no error anywhere.

Fix: generate the byte array programmatically. Then add a checker that compares every
`_UUID` string against every `_UUID_LE` array — all six now verify byte-for-byte.

### 6.3 The checker itself corrupted the contract header

The first version of that checker used a greedy regex that ran past the end of the
`_LE` array and swallowed ~95 lines of `ble_service.h` (including the maneuver
enum). Recovered with `git checkout` and re-applied by hand.

**Rule: never run clever text rewriting over a contract header.** Write a *checker*
that only reads and reports. If something must be generated, generate it into a
temp file and paste deliberately.

### 6.4 Missing comma between macro continuation rows

Generated macro rows joined without a trailing comma produced an expansion of
`... 0x9e 0x18 ...` — one token instead of two. Error surfaced as a confusing
`expected '}' before numeric constant` pointing at `ble_service.h:44`, three files
away from the real problem.

When a macro expansion error appears in an unrelated file, print the expansion:
`gcc -E` on a two-line snippet reproduces it instantly.

### 6.5 BlueZ serves a cached GATT table — a false negative that cost a whole build/flash cycle

After fixing the UUID, the board *still* appeared to be missing the characteristic.
It wasn't. BlueZ caches a bonded peer's GATT database and does not re-discover
unless it honours the Service Changed indication (it didn't).

Verification that settled it: `bluetoothctl remove <addr>` (unpair), then scan
again — the characteristic was there all along.

**Lesson: when a bonded device's GATT table looks stale, unpair before believing it.**
Android behaves similarly, which is §6.8.

### 6.6 A successful build proves nothing about your code being in the binary

`main.c`'s nav wiring was lost as collateral of the `git checkout` in §6.3. The
build still succeeded, because `--gc-sections` silently discarded the whole
`nav_view.o` when nothing referenced it. No warning, no error, no behaviour.

Checks that would have caught it in seconds, and should be standard practice here:

```bash
nm build/nav-esp32-fw-v2.elf | grep -c nav_view_update      # 0 = dead code
strings build/nav-esp32-fw-v2.bin | grep -c "your log string"
```

Also worth knowing: **ninja did not rebuild** after one edit and the stale artifact
was flashed. `touch main/nav_view.c && idf.py build` forced it. When a change
"doesn't take", verify the artifact before re-diagnosing the code.

### 6.7 NULL dereference → Guru Meditation

```
Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
  nav_view_update  nav_view.c:285
  process_events   main.c:318
```

`nav_view_update(NULL, street)` — the TEXT-only path — dereferenced `*st`
unconditionally. The header documented that either half may be absent; the
implementation didn't honour it. Fixed with an early `if (!st && !street) return;`
and per-field guards.

Backtrace decode (AGENTS.md rule):

```bash
xtensa-esp32s3-elf-addr2line -e build/nav-esp32-fw-v2.elf -f -C <addrs>
```

### 6.8 The "first state" log that never appeared

Spent real time on this. It was a **measurement artifact, not a bug**: a one-shot
`if (!logged)` log fires on the *first* state after connect, but the serial capture
window was opened ~10 s after tapping Connect. The event had already happened and
was gone.

Rule: a one-shot log is invisible to any capture that starts after the interesting
moment. Either make it periodic, or start capturing *before* connecting.

### 6.9 OsmAnd's bundle types are not consistent → 308 exceptions

```
java.lang.String cannot be cast to java.lang.Integer   (×308)
```

`turnInfo` hands back some values as `String` where other keys/builds return `Int`.
A typed `Bundle.getInt()` throws `ClassCastException` and takes the **entire frame**
down with it — which is why ~150 frames succeeded and 308 died in the same window.

Fix: read through `bundle.get(key)` and switch on the runtime type, with each field
in its own try/catch so one bad field cannot lose the rest:

```kotlin
private fun bInt(b: Bundle?, key: String, def: Int) = try {
    when (val v = b.get(key)) {
        is Int -> v
        is Long -> v.toInt()
        is String -> v.trim().substringBefore('.').toIntOrNull() ?: def
        else -> def
    }
} catch (e: Exception) { def }
```

Never trust a third party's `Bundle` to be typed.

### 6.10 Stale GATT cache on Android

The phone's cached service table lacked the new characteristic, so writes were
dropped. Two fixes, both worth keeping:

1. `BleManager.servicesDiscovered` now checks a `REQUIRED` list of **all**
   characteristics and refreshes the cache if any is missing. It previously checked
   only the media one — which is exactly why a missing nav channel hid.
2. A permanent warning when a write targets an unknown characteristic:

```
W write dropped: 7c3d1f92-… not in service (12 B)
```

   Silent drops are the single most expensive failure mode in this stack. There is
   now no path where a dropped write is invisible.

---

## 7. Verification toolkit that worked

- **Raw GATT table from a PC** — the ground truth for "what does the board actually
  advertise", independent of any phone cache. Requires unbonding first (§6.5).
  `blevenv/bin/python list_chars.py` via bleak.
- **Binary inspection** — `nm` and `strings` on `build/nav-esp32-fw-v2.{elf,bin}`
  to prove what is compiled in. Compare against source intent whenever behaviour
  and code disagree.
- **Continuous logcat to a file** — `adb logcat -v time > file &`. Ad-hoc
  `logcat -d` with tag filters lost the very lines being investigated (buffer
  rotation). Always capture to a file first, filter afterwards.
- **Serial capture** — `mon.py` in `/tmp/opencode`, plus `addr2line` for panics.
- **UUID byte checker** — `tools/check_uuids.py` (read-only) compares every
  `_UUID` string to its `_UUID_LE` array; it reports, never rewrites (§6.2).
- **Mock nav driver** — `tools/nav_mock.py` writes STATE/TEXT straight to the
  characteristic over BLE (no OsmAnd, no phone): `icons`, `one`, `route`, `live`.
  This is what finally exercised the §8 icons/paths on hardware.
- **Offline icon preview** — `tools/preview_icons.py` mirrors `draw_icon` geometry
  into `tmp/nav_icons_preview.png`, catching clipped/malformed shapes before a
  flash cycle.

---

## 8. Open / unproven

- **Only `CONTINUE` has ever rendered.** The phone was stationary for every test, so
  `slight`/`sharp`/`keep`/`uturn`/`roundabout`/`off-route`/`arrive` icons have never
  been drawn on hardware. They are geometry in `draw_icon()`, nothing more.
- **Street name has never been non-empty** — the test route was on an unnamed path.
  `next_turn_name` remains unverified end to end.
- `NAV_OFF_ROUTE` and `NAV_ARRIVE` paths untested.
- Staleness/heartbeat behaviour (`STALE_MS 6000`) untested against a real drop.
- Dark-map inversion cost on tile load still unmeasured.
- Routing is OsmAnd's; the device has no route line and no voice guidance.

All OsmAnd work is uncommitted. The app is GPL-3.0 because of the vendored AIDL.