# 09 — Hardware Notes for a Navigation Device

Constraints and opportunities this board imposes on the firmware design. This is
the bridge between the reference documentation and the firmware plan.

**No firmware has been written yet.** These notes are what the hardware forces,
to be weighed when we plan.

## The gap: no GPS

**There is no GNSS receiver on this board.** A navigation device needs an external
module. The board makes this straightforward:

| Option | Interface | Verdict |
| --- | --- | --- |
| **UART** | **GPIO43 (TXD) / GPIO44 (RXD)** on both the 12-pin and 4-pin UART headers | **Best choice.** Free, broken out, unused onboard. Typical GNSS modules (NMEA at 9600 baud) need ~2 kB/s — trivial. |
| I2C | GPIO7 / GPIO15 | Possible but I2C GNSS modules are uncommon and the bus is shared with 4 other devices. |
| USB | GPIO19 / GPIO20 | Would need a USB GNSS and USB host support. Overkill. |

GPIO43/44 is the obvious answer, and it is the only pair of general-purpose pins
conveniently broken out to a header. **A GNSS module's VCC current (~15–40 mA for
a typical module) is well within the 800 mA LDO budget** alongside Wi-Fi and the
backlight — but see the power section below.

> **Plan the UART baud rate and protocol now.** NMEA 0183 sentences are
> human-readable ASCII, roughly 60–100 bytes per fix, 1 Hz. Parsing is cheap. The
> expensive part is usually *storage* (see below), not parsing.

### RTC versus GPS time

The PCF85063 with a 1970 year offset stores a Unix epoch directly, which pairs
cleanly with NMEA UTC time. Two strategies:

- **With RTC backup battery** — correct clock at power-on, GPS as slow sync.
  Requires fitting the rechargeable cell.
- **Without** — a cold GNSS start takes 30–60 s outdoors before you have valid
  time. Worse: an RTC with a flat battery can return garbage, and 1970-01-01 is
  what you will see.

**Recommendation:** fit the RTC battery. It is a 2-pin rechargeable header and
costs almost nothing; a nav device that boots to the wrong time looks broken.

## The display is the design constraint

A 2.8" round 480 × 480 RGB565 panel is a demanding target. Four limits to respect.

### Colour depth is 16-bit

RGB565, not RGB888. The panel could do 262K; the ESP32's RGB peripheral cannot.
Gradients band. For map rendering — where a sky gradient, water shading and
elevation tinting all rely on smooth colour steps — this is the most visible
compromise.

**Mitigations:** dither map assets at build time; design a deliberately limited
palette; consider whether the round format and small size make subtle gradients
worth the effort anyway.

### The glass is a circle; the framebuffer is a square

70.13 mm active circle inside a 95.86 mm module, rendering into 480 × 480. About
**21.5% of the canvas is outside the visible area.**

**Implication:** you need either a circular clip, a mask, or a layout that
deliberately exploits the shape. The `traviscea` round-dash projects linked in
[08-resources-links.md](08-resources-links.md) are the closest published prior
art for solving this.

A **compass/heading rose** is the natural centrepiece for a circular nav display
— it uses the shape rather than fighting it, and puts heading where the driver's
attention already is.

### Refresh rate and bandwidth

| Config | Refresh | PSRAM DMA traffic |
| --- | --- | --- |
| ESP-IDF default (18 MHz pclk) | ~64.7 Hz | ~36 MB/s |
| Arduino (30 MHz pclk) | ~107.8 Hz | ~60 MB/s |

64.7 Hz is adequate for text and menus. **For a panning map, 107 Hz is
noticeably better** — consider raising the pixel clock in an ESP-IDF build, and
budget the extra ~24 MB/s of PSRAM bandwidth.

### Tearing

The ESP-IDF example supports tear avoidance with semaphores that hold the GUI
until scan-out completes. The Arduino example uses a bounce buffer instead
("*to avoid screen drift*"). Double buffering costs 450 KiB of PSRAM.

**For a continuously panning map, tearing is unacceptable** — pick one strategy
deliberately. Blocking the GUI on vsync is simplest; a bounce buffer is cheaper in
memory.

## Memory budget

8 MB PSRAM, 16 MB flash, 512 KB internal SRAM.

| Consumer | Size | Notes |
| --- | --- | --- |
| LVGL draw buffer(s) | 450 KiB each | 1–2 of them |
| Frame buffer(s) | 450 KiB each | 1–2 of them, in PSRAM |
| LVGL heap | remainder | `CONFIG_LV_MEM_CUSTOM` is on — you choose |
| Map tiles / route cache | **your design** | The big unknown |
| App (flash) | **3 MB partition** | Check `idf.py size` early |

> **The 3 MB app partition is the constraint most likely to bite.** LVGL alone is
> several hundred KB. Add font subsets for street names, an NMEA parser, routing
> logic, and a map renderer, and 3 MB is not generous. Plan to enlarge
> `partitions.csv` up front rather than discovering the ceiling late.

**Storage strategy is a firmware-design decision, not a hardware one.** The TF
slot is 1-bit SD — fine for map tiles, plenty for this purpose. But 1-bit also
means sequential-only, so a large route/POI database on the card will be slow to
query in a way that matters for a moving vehicle.

The `flash_test` FAT partition on internal flash is not a substitute for real
storage, but it is a working FAT implementation you can borrow.

## Power budget — the real ceiling

The **ME6217C33M5G LDO is rated 800 mA maximum.** That is the whole system.

| Load | Typical | Notes |
| --- | --- | --- |
| ESP32-S3 @ 240 MHz, Wi-Fi active | 100–250 mA peak | Peaks hard during TX |
| Backlight at full brightness | 100–200 mA | Dominant, and highly variable |
| GT911 + QMI8658 + PCF85063 + expander | < 20 mA | |
| GNSS module | 15–40 mA | Cold start is the worst case |
| TF card | 20–100 mA | Spiky during writes |

Summed, worst case lands uncomfortably close to 800 mA.

**The single biggest lever is backlight brightness.** PWM-dimming to ambient light
level cuts the largest load by most of the total. Keep the PWM frequency at or
above ~20 kHz (the Arduino example uses 20 kHz) so the dimming is not audible and
does not interfere with the buzzer.

**Battery life is a UI/UX problem as much as an electrical one.** A nav device
sitting in a dark car at night with the backlight at 100% is the worst case. An
ambient-light-responsive dimming policy is close to mandatory, not a nice-to-have.

**Low-power is explicitly supported** — the wiki notes "flexible clock control and
various power modes for low-power scenarios." Combined with the touch interrupt on
**GPIO16**, a wake-on-touch design is feasible: screen off, GPS on a duty cycle,
touch wakes the device.

> Note the interaction: a display sleep/wake cycle re-initialises SPI2, which
> **breaks the TF card** until `SD_Init()` runs again (see
> [07-faq-troubleshooting.md](07-faq-troubleshooting.md#2-gpio1-and-gpio2-are-shared-between-the-lcd-and-the-tf-card)).
> Budget for this in any power-management design. It is the single most likely
> source of a "works until I add sleep" bug.

## Sensors: what they are actually good for

### QMI8658 IMU

6-axis, on the shared I2C bus. Useful for:

- **Orientation detection** — a 95.86 mm round device in a windscreen mount can be
  mounted in 4 rotations. The IMU tells you which, and you rotate the touch
  transform and UI to match. This is the single most valuable use.
- **Tilt-based compass calibration prompts** — tell the user to hold the device
  level before trusting the heading.
- **Shake-to-wake.**

Caveats: the vendor driver **busy-waits** and does not use the available
`IMU_INT1`/`IMU_INT2` interrupts. Consider enabling interrupts to free CPU, after
confirming the lines are routed on your board revision.

### Buzzer

Driven from `EXIO8`. For turn-by-turn navigation, an audio prompt is close to
essential — and the board has no speaker, only a buzzer. A buzzer cannot play
speech. This is the hardest constraint on the board, because there are only two
free GPIOs.

**The pin budget rules out I2S.** A normal I2S amp (MAX98357A, PCM5102, WM8731)
needs three pins — BCLK, LRCLK, DOUT. You have two, and they are the same two
that also serve as the UART header. Full reasoning in
[02-pinout.md — Free / available GPIO](02-pinout.md#free--available-gpio).

That leaves four viable options:

| Option | Extra hardware | Extra GPIO | Voice quality |
| --- | --- | --- | --- |
| **A. Phone speaks** | None | 0 | **Best.** Android TTS is far better than anything you would add. |
| **B. I2C DAC + amp** | MCP4725-class DAC, small amp, speaker (~$4) | **0** — SDA/SCL already on a header | Middling; intelligible at 8–16 kHz sample rate |
| **C. Buzzer tones only** | None | 0 | No speech. Tone patterns for turn type only. |
| **D. I2S by reclaiming a pin** | I2S amp + speaker | 3, by sacrificing GPIO43/44 *and* one more (GPIO0 or USB) | Good, but leaves no UART and no BOOT button |

**Option A is the coherent design**, and it is worth being honest about why: if
the phone is already in a windscreen mount supplying fused GPS, then the phone
is the natural place for the voice too. The ESP32 becomes a **glanceable
display** — a big next-turn arrow, distance countdown, and map — which is exactly
what a 2.8" round screen is good at, and exactly what the phone is bad at (small
screen, buried in a pocket or cradle, fiddly to glance at while driving).

**Option B is the fallback if you want the device to be standalone.** It is the
only option that adds audio without consuming a GPIO, because I2C already has
GPIO7/GPIO15 broken out on a header and the bus carries just 4 devices. A
12-bit I2C DAC over a 400 kHz bus sustains roughly 10–20 kS/s, which is enough
for 8–16 kHz mono speech. Pre-render clips on the phone at trip-plan time, cache
them on the TF card, and playback is a simple indexed lookup — so decode quality
matters less than you might fear.

**Option C is a legitimate floor**, not a joke. Garmin shipped tone-based
navigation for years. Encode turn type in tone pattern and distance in beep
cadence; it works with zero hardware and zero BOM.

**Decide this before the UI design**, because it changes the state machine: with
Option A the device must know which maneuver the phone has already spoken
(easy — the phone drives, it announces), whereas with B or D the device owns the
audio state and must sequence clip playback itself.

### TF card

1-bit SD. For map tiles and route data this is the right tool. Keep the init
ordering discipline in mind.

### Wi-Fi

For map downloads, live traffic, or NTRIP corrections. The **external antenna
requires re-soldering an onboard resistor** — worth doing if GPS and Wi-Fi will
be adjacent, since they interfere. Find the resistor in
[`datasheets/ESP32-S3-Touch-LCD-2.8C_schematic_diagram.pdf`](datasheets/).

## Hardware decisions to make before writing firmware

1. **Where does position come from — the phone, or a GNSS module on GPIO43/44?**
   The phone's fused GPS is genuinely better and costs nothing. If the phone
   supplies position, **no GNSS module is needed and GPIO43/44 stay free.** If you
   want a standalone device, a module on GPIO43/44 is the only viable UART — but
   then those pins are gone, and with them any chance of I2S audio.
2. **Fit the RTC backup cell?** Recommended — a nav device that boots to the wrong
   time looks broken.
3. **Audio or no audio?** The buzzer is there. Speech needs added hardware and
   a shared I2C bus. Affects the UI design.
4. **Enclosure: dev board, or custom carrier?** The 95.86 mm round module is large
   and has protruding USB-C connectors. A custom board would change which GPIOs
   are available — and with only GPIO43/44 conveniently broken out, losing those
   would hurt.
5. **Framework: Arduino or ESP-IDF?** ESP-IDF is the stronger choice for this
   project (better FreeRTOS control, tear-avoidance support, better debugging),
   but Arduino gets to a working LVGL display faster. They are **not**
   interchangeable — see [04](04-development-arduino.md) vs
   [05](05-development-esp-idf.md).
6. **Device or dashboard?** Mounted in a vehicle (needs vibration tolerance,
   wide temperature, always-on) or handheld (needs battery life and portability)?
   This changes almost every requirement above.
7. **Map data source and format?** Raster tiles, vector, or offline routing? The
   storage format and the rendering approach are the largest single design
   decisions, and they are not yet made.

## Known-unknowns to resolve early

| Question | Why it matters | Where to look |
| --- | --- | --- |
| Which GNSS module, and on which UART? | **Superseded** — if the phone supplies position, GPIO43/44 are free and no module is needed | — |
| Does I2C survive adding a 5th device (audio DAC)? | The only zero-GPIO path to real audio | Bus is polled at 10 Hz; 400 kHz has headroom |
| Are `IMU_INT1`/`IMU_INT2` routed? | Removes a busy-wait, enables motion wake | Schematic + continuity test |
| Is touch active on your unit? | Board is sold with and without touch | Product page variant selector |
| Does the panel tolerate 30 MHz pclk in an IDF build? | Better map panning | Test; watch for tearing or artefacts |
| Actual battery draw under load | Determines viable battery capacity | Measure with a USB current meter |
| Practical BLE throughput on ESP32-S3? | Sizes the tile-prefetch budget | Measure before committing to a tile budget |
