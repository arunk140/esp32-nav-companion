# Valhalla Navigation Plan — a second routing source, phone-side

Status: **Phases A–D implemented** (2026-10-05): phone-side routing/nav loop,
Google Maps share ingest + recent list, and the on-device **route line** drawn
from Valhalla geometry. Firmware builds and flashes; app builds with 23 unit
tests green. Validated on phone hardware (Nothing Phone 3, Android 17) against a
bonded ESP32: BLE connect, Valhalla `/route` (24 maneuvers / 55.6 km), route
geometry `168 pts / 6 chunks` sent, plus `NAV_STATE`/`NAV_TEXT`/`NAV_DEST`
frames. On-panel visual confirmation of the line is the last check.

Goal: add Valhalla turn-by-turn as an **option alongside OsmAnd**, so the user
picks a destination in the companion app instead of relying on OsmAnd's UI.
Destination picking is done by **sharing a place out of Google Maps** (or any
maps app) into ours — no geocoder, no autocomplete server, no API key.

Related: `docs/nav_implementation_guide.md` (the OsmAnd source this mirrors),
`firmware_v2/main/ble_service.h` (the wire contract), `app/.../NavBridge.kt`.

---

## 0. Decisions (locked)

| # | Decision | Choice |
| --- | --- | --- |
| 1 | Valhalla endpoint | **Configurable, default `https://valhalla1.openstreetmap.de`** (FOSSGIS public demo) for bring-up; point at a self-hosted instance later. |
| 2 | Geocoding / search | **None.** The user searches in Google Maps and **shares the place** to the companion app via an `ACTION_SEND` intent. Google resolves the place to coordinates and a name; we just parse the share. |
| 3 | Destination picker | **Recent/saved list first**, populated by shares. A companion map + long-press pin is a later phase. |
| 4 | Route line on the ESP32 | **Banner first, route line later.** Valhalla *does* return route geometry (OsmAnd did not) — that unlocks a route line as its own phase D. |
| 5 | OsmAnd | **Keep both**, add a source selector (OsmAnd / Valhalla). OsmAnd stays the fallback. |

---

## 1. Why the firmware does not change (for the core feature)

The device is already routing-source-agnostic. `firmware_v2/main/ble_service.h`:

```
NAV_STATE (12 B): flags, maneuver, dist_m, after_maneuver, after_dist_m, remaining_m, remaining_s
NAV_TEXT:         street name (on change)
NAV_DEST (10 B):  destination lat/lon marker (on change)
```

`nav_view.c` renders exactly that and knows nothing about OsmAnd. `NavBridge.kt`
is a stateless encoder from a `NavState` to those bytes. The own-maneuver-enum
decision (`docs/nav_implementation_guide.md` §1) is what makes this cheap: a
second producer just fills the same struct.

**So Valhalla is a phone-side change.** Firmware edits are limited to Phase D
(route line). That also means the entire feature can be developed and tested
without reflashing the board, against the existing banner + DEST marker.

---

## 2. Architecture

```
Google Maps ──share──▶ Companion app (ACTION_SEND)
                         │ parse coords + label
                         ▼
                    Recent/Saved store (Room)
                         │ user taps Start
                         ▼
        Valhalla /route  ──▶ route {maneuvers, polyline6, summary}
                         │
   GPS fix (1 Hz) ───────┤ project fix onto polyline
                         │  → dist to next maneuver, remaining m/s
                         │  → off-route? → re-request /route
                         ▼
                    NavBridge.send(NavState)   [reused as-is]
                         ▼
                    BLE NAV_STATE / NAV_TEXT / NAV_DEST
                         ▼
                    ESP32 banner + destination marker   [unchanged]
```

A `NavSource` abstraction sits in `NavService`: `OsmAndSource` (existing
`OsmAndLink`) and `ValhallaSource` (new). Exactly one is active, selected by a
persisted preference. Both end at the same `NavBridge.send()`.

---

## 3. Endpoint: configuration and the FOSSGIS limits

Default: `https://valhalla1.openstreetmap.de`. It is a **community demo, not
production**. Obligations we must honour even for personal use:

- **Max 1 request/user/second** (global 100/s). Our design only calls `/route`
  at route start and on reroute (debounced ≥ 10 s), so this is easy.
- **Dev/testing only.** Fine for this device. If it ever becomes a product,
  self-host (`valhalla/valhalla` Docker image, regional OSM extract — Mumbai
  extract runs on modest RAM; a full planet does not).
- **Identify the client** via `X-Client-Id` header, and notify FOSSGIS via
  GitHub Discussions if this is ever published.
- Don't hammer it from the share-parse path; only the navigator calls it.

Implementation: a settings field in `MainActivity` (persisted in `Prefs`) so the
base URL can be repointed at a LAN self-host without a rebuild. Default is the
demo URL above. Add `X-Client-Id: nav-esp32` to every request.

---

## 4. Destination ingest — Google Maps share

Valhalla cannot geocode (its docs say so explicitly). Instead of adding
Nominatim/MapTiler, we accept a place the user already found.

### 4.1 Intent filters

Register in `AndroidManifest.xml` for the `.companion` package:

- `ACTION_SEND` with `text/plain` — the "Share → Nav Companion" path. Read
  `EXTRA_TEXT` and `EXTRA_SUBJECT`.
- `ACTION_VIEW` for `geo:` URIs and `https://` hosts `maps.app.goo.gl`,
  `goo.gl`, `www.google.com/maps`, `maps.google.com` — so tapping a Maps link
  can open us too.

If the app is already running as the foreground service, a share arrives at
`MainActivity` (a `singleTask` activity) via `onNewIntent`.

### 4.2 Parse rules

A Google Maps share is typically `"<Place name>\nhttps://maps.app.goo.gl/xxxx"`.
Algorithm:

1. Take `EXTRA_TEXT`; the first `https?://…` token is the URL, the text before
   it is the candidate **label**.
2. If the host is a shortener (`maps.app.goo.gl`, `goo.gl/maps`), resolve
   redirects (`OkHttp` with `followRedirects(true)`) and parse the final URL.
3. Extract coordinates, in priority order:
   - `!3d(-?<lat>)!4d(-?<lng>)` — the **POI** coordinates. Prefer this.
   - `[?&](q|query|ll|destination)=(-?<lat>),(-?<lng>)` (also `%2C`).
   - `@(-?<lat>),(-?<lng>)` — **viewport centre**, last resort: for a place
     share this can sit a street away from the pin.
4. Label falls back to "Dropped pin" when the share carried none.

Known failure mode to handle explicitly: some short-link redirects land on a
URL with only a place-id (`…data=!4m2!…1s0x…`) and **no coordinates**. Do not
guess from a place-id; either make one follow-up request to the resolved URL or
reject the share with a toast ("Couldn't read this link — open it in Maps once,
then share again"). Never route to a wrong point silently.

The parser is pure URL/string work — unit-testable with a table of real share
strings, independent of Android.

### 4.3 Recent / saved store

`SharedPreferences` (not Room), as a small JSON array: this is a bounded list
(saved + ~10 recent), the app already stores every other preference this way,
and it avoids adding KSP/Room to a build that has no annotation processing
today. Most-recent 10 shown; a long-press toggles `saved`. Dedup is by
`"%.7f,%.7f"` coordinate id, so sharing the same place twice just bumps its
timestamp. (The plan originally proposed Room; the dependency-free store won on
KISS grounds — storage of a dozen pins is not where complexity belongs.)

---

## 5. Routing

`POST {base}/route`, JSON body:

```json
{
  "locations": [
    {"lat": <origin_lat>, "lon": <origin_lon>, "heading": <bearing_deg>},
    {"lat": <dest_lat>,   "lon": <dest_lon>,  "name": "<label>"}
  ],
  "costing": "auto",
  "units": "kilometers",
  "directions_type": "instructions",
  "language": "en-US",
  "id": "nav-companion"
}
```

- Origin = current GPS fix; destination = the shared place. `heading` from the
  fix's bearing so the route leaves in the direction of travel.
- `units: kilometers` because Valhalla has no metres option — **maneuver
  `length` is in the requested units**; convert ×1000 for the wire (`u16`
  metres).
- `directions_type: instructions` returns `instruction` + `street_names`.
- Request `costing: "auto"`; expose walking/cycling later if wanted.

### 5.1 Maneuver mapping (Valhalla `type` → our `NAV_*`)

Our enum is the wire contract; Valhalla's numbering stops here.

| Valhalla | # | NAV_* |
| --- | --- | --- |
| kNone | 0 | NAV_NONE |
| kStart / kStartRight / kStartLeft | 1–3 | NAV_CONTINUE |
| kDestination / …Right / …Left | 4–6 | NAV_ARRIVE |
| kBecomes, kContinue, kRampStraight, kStayStraight, kMerge | 7,8,17,22,25 | NAV_CONTINUE |
| kSlightRight, kRampRight | 9,18 | NAV_SLIGHT_RIGHT |
| kRight, kExitRight | 10,20 | NAV_RIGHT |
| kSharpRight | 11 | NAV_SHARP_RIGHT |
| kUturnRight, kUturnLeft | 12,13 | NAV_UTURN |
| kSharpLeft | 14 | NAV_SHARP_LEFT |
| kLeft, kExitLeft | 15,21 | NAV_LEFT |
| kSlightLeft, kRampLeft | 16,19 | NAV_SLIGHT_LEFT |
| kStayRight | 23 | NAV_KEEP_RIGHT |
| kStayLeft | 24 | NAV_KEEP_LEFT |
| kRoundaboutEnter, kRoundaboutExit | 26,27 | NAV_ROUNDABOUT |
| kFerryEnter, kFerryExit | 28,29 | NAV_CONTINUE |
| kTransit* | 30–36 | NAV_NONE |

Roundabout exit number (`roundabout_exit_count`) has no icon slot; fold it into
the TEXT as "Roundabout, exit N".

### 5.2 Shape

Each leg returns `shape`, an **encoded polyline with 6-decimal precision**
(polyline6), and each maneuver has `begin_shape_index` / `end_shape_index`.
Decode once per route with our own ~40-line decoder (don't pull a dependency),
keep the decoded points for the nav loop (and, later, the device route line).

### 5.3 Route geometry to the device (implemented)

Valhalla's shape is also sent to the board so it can draw the actual route —
the capability OsmAnd never exposed. On the existing nav characteristic, a new
message `0x04 ROUTE` arrives in order:

```
[0] 0x04, [1] flags (bit0 START, bit1 END), [2] seq, [3] count,
[4..] count * (i32 lat_e7, i32 lon_e7)      count <= 31 (chunk fits MTU 253)
```

* The app simplifies the polyline (Douglas-Peucker, tolerance widened until it
  fits 512 points, stride fallback) and sends chunks with **tag=null** so the
  GATT queue never coalesces them. Empty `START|END, count=0` clears the line.
* The NimBLE write callback only queues a `route_chunk_t`; the GUI task
  assembles `reset -> add* -> commit`, checking `seq` for gaps and dropping the
  whole route rather than drawing a broken one.
* `map_view.c` stores up to 512 points and draws a two-pass (casing + fill)
  polyline under the markers, reusing the marker's pan/zoom/heading-up
  projection and culling off-screen segments. The old bowed "suggested route"
  dotted line is now only the fallback when no route geometry exists.
* Cleared on link drop and on any inactive `NAV_STATE`.

---

## 6. The nav loop (the real work Valhalla doesn't do)

Valhalla `/route` is a static response — no "you are here". On each 1 Hz fix:

1. **Snap** the fix to the polyline: project into a local equirectangular frame
   (metres, scaled by `cos(lat)`), find the nearest segment and the interpolation
   parameter `t`. Distances are short, so this approximation is well under the
   wire's 1 m resolution.
2. **Progress:** keep a monotonic shape index (ignore a snapped point *behind*
   the current one — GPS drift and u-turns must not rewind the banner). Compute
   `dist_m` = geodesic distance from snap point to the next maneuver's
   `begin_shape_index`. `after_*` = the maneuver after that.
3. **Remaining:** precompute cumulative distance and time along the shape
   (distribute each maneuver's `time` across its shape span). `remaining_m` /
   `remaining_s` = totals minus the values at the snapped index.
4. **Off-route:** perpendicular distance from the fix to the polyline. If it
   exceeds ~50 m for ~3 consecutive fixes (not a single outlier), mark
   `NAV_OFF_ROUTE` and re-request `/route` from the current position. Debounce
   reroutes to ≥ 10 s (also respects FOSSGIS 1 req/s).
5. **Arrival:** snapped near the final `end_shape_index` and within ~25 m, or
   the active maneuver is `kDestination` within ~25 m → `NAV_ARRIVE`, then stop.
6. **Street text:** current maneuver's `street_names[0]` (verbatim `instruction`
   as a fallback), sent only on change via the existing TEXT path.

State machine: `IDLE → ROUTING → NAVIGATING → (OFF_ROUTE → ROUTING) →
ARRIVED/STOPPED`. All of it runs off the main thread (`OkHttp` enqueue /
coroutine), inside the foreground service so it survives screen-off.

### 6.1 Where it hooks in

`LocationStream` currently emits only the packed 18 B POSITION payload. Refactor
it to emit a typed `Fix(lat, lon, speed, bearing, payload)`; `NavService` keeps
packing POSITION exactly as today *and* feeds the active `NavSource` when
Valhalla is running. No change to the POSITION wire format.

BLE writes still go through `BleManager.writeNav` (queued, coalesced by tag as
today). Nothing here runs in a NimBLE callback, so the "callbacks stash only"
rule (AGENTS.md #3) is unaffected — but route HTTP must never run on the GUI/
main thread.

---

## 7. App lifecycle and UI

- `Prefs`: add `navSource` (`osmAnd` | `valhalla`) and `valhallaBaseUrl`.
- `MainActivity`: a source radio/switch; a "Destination" section (recent list,
  Start, Stop); keep the OsmAnd probe button for the fallback source.
- `NavService`: on connect, start the selected source only. `ACTION_NAV_START`
  (with dest lat_e7/lon_e7/name) and `ACTION_NAV_STOP` intents so the route
  survives the activity being backgrounded/killed. On link loss, stop the
  navigator and `NavBridge.reset()`, same as the OsmAnd path does today.
- The **DEST marker** on the device already works (`NAV_DEST`, `on_nav_dest`);
  reuse it to show the selected destination even before moving.

### 7.1 Dependencies

**No new runtime dependencies.** `HttpURLConnection` and `org.json` ship with the
Android SDK, and places use `SharedPreferences`. This keeps the build free of
KSP/kapt and avoids OkHttp/serialization/Room for a feature that does not need
them. The only addition is `testImplementation("junit:junit:4.13.2")` for the
pure-logic unit tests (polyline decode, projection, maneuver map, nav loop, share
parser — 15 tests, all green).

The app is already GPL-3.0 because of the vendored OsmAnd AIDL client
(`docs/nav_implementation_guide.md` §2), so this adds no new licensing
constraint.

---

## 8. Phasing

**Phase A — bring-up (no UI polish).** Hardcode a destination, call `/route`,
decode, drive `NavBridge`, confirm the banner + DEST marker on hardware.
Validates the maneuver table, unit conversion, and the wire path. Also proves
the `!3d/!4d` parser against a table of real share strings.
→ **Implemented** (client, decoder, maneuver map, parser + unit tests; hardware
confirmation pending a phone run).

**Phase B — nav loop.** Precomputed route + fixes → progress, remaining,
off-route, arrival, reroute, stop. This is the core.
→ **Implemented** (`ValhallaNavigator`, `ValhallaSource`; unit-tested).

**Phase C — destination UX.** `ACTION_SEND`/`ACTION_VIEW` ingest → recent/
saved list → Start/Stop in `MainActivity`; source selector.
→ **Share ingest + list + source selector implemented.** Companion map with
long-press pin (needs an osmdroid/MapLibre dependency) still open.

**Phase D — firmware extras.** Route line from the polyline6 shape: decode +
project lat/lon → tile pixels + draw on the round panel (the thing
`docs/nav_implementation_guide.md` §1 says was impossible with OsmAnd).
→ **Route line implemented** (§5.3). ETA and next-next maneuver still open.

---

## 9. Acceptance

- Share a place from Google Maps → it appears with a name in the recent list.
- Start → a route is drawn on the device banner; `NAV_DEST` marker shows.
- Walk/drive the route: distance shrinks, street name updates, maneuvers advance.
- Leave the route: banner flips to Off route, then recovers after a reroute.
- Arrive: banner shows Arrived, then hides.
- Kill/background the app: guidance continues from the foreground service.
- OsmAnd source still works end to end when selected.

---

## 10. Risks / open questions

- **Short-link redirects may omit coordinates** (§4.2). Must fail loudly, never
  route to a guessed point.
- **FOSSGIS demo is not production** and is 1 req/s. Reroute debounce protects
  it, but a self-host should be the eventual default.
- **Route line rendering** (Phase D) is the only firmware risk: polyline6
  decode, projection to the tile grid, and drawing on the 480×480 RGB panel
  within the 28 ms/frame budget.
- **Room/KSP** is a build-system change, not just a dependency.
- **Attribution:** showing Valhalla/OSM directions should carry OSM attribution;
  confirm the device map's existing attribution covers route display.
- **Two nav sources** must never run at once — guard the source selector so
  switching stops the previous source first.
