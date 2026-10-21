# nav-esp32

Monorepo: ESP32-S3 nav device firmware + Android companion app + docs.

## Demo

[![Eyes Up, Not on the Phone — night ride demo](https://img.youtube.com/vi/zr3O4h-VliI/hqdefault.jpg)](https://www.youtube.com/watch?v=zr3O4h-VliI)

Night ride on the real device: destination set once in the app, turn-by-turn on the bars, music control from the display. [Watch on YouTube](https://www.youtube.com/watch?v=zr3O4h-VliI).

```
nav-esp32/
  firmware_v2/ ESP-IDF firmware (ESP32-S3-Touch-LCD-2.8C, LVGL 9, SD maps, BLE, media controls)
  app/        Android companion app (QR pairing, BLE central, location stream)
  docs/       Reference docs, pinout, Phase 1 plan
  3mf/        Enclosure models (existing)
```

See `docs/phase-1-plan.md` for the build order. Phase 1 is USB-powered, no battery.

## Quick start

- Firmware: `cd firmware && idf.py build && idf.py flash`
- App: open `app/` in Android Studio, run on a phone.
- Tiles: see `docs/waveshare-2.8c/10-map-tiles-reference.md` for the SD pipeline.
