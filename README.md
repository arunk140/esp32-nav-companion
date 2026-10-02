# nav-esp32

Monorepo: ESP32-S3 nav device firmware + Android companion app + docs.

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
