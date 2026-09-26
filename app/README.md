# App — Android companion (nav device remote)

Kotlin + BLE central + QR pairing + fused location stream.
See `docs/phase-1-plan.md`.

## Layout

```
app/
  settings.gradle.kts
  build.gradle.kts
  gradle.properties
  app/build.gradle.kts
  app/src/main/AndroidManifest.xml
  app/src/main/java/com/navesp32/companion/
    MainActivity.kt        status, Connect, Scan QR, Unpair, service toggle, brightness slider
    NavService.kt          foreground service: BLE auto-connect + location stream
    BleManager.kt          scan/connect/bond, POSITION write, CMD writes
    QrScannerActivity.kt   CameraX + MLKit/ZXing, parses {"n","m","s","t"}
    LocationStream.kt      FusedLocationProvider, 1 Hz (5 Hz debug), 18 B pack
```

## Contract (mirrors firmware/main/ble/ble_service.h)

- POSITION 18 B LE: lat/le32 1e-7, lon, speed u16 cm/s, cog u16 0.01deg, sats u8, fix u8, ts u32 ms.
- CMD: `0x01 recenter`, `0x02 zoom`, `0x03 ping`, `0x04 unpair`, `0x05 brightness 0-100`.
- Brightness slider throttled to 200 ms; sync on connect via STATUS.
- Unpair: `CMD 0x04` → `removeBond()` → clear prefs → disconnect.
- Service toggle: OFF stops BLE + location + notification; persists across reboot (default ON after first pairing).

## Build

Open `app/` in Android Studio (Hedgehog+). Requires SDK 34, minSdk 26.
Permissions: BLUETOOTH_SCAN/CONNECT, ACCESS_FINE_LOCATION, CAMERA,
FOREGROUND_SERVICE + FOREGROUND_SERVICE_CONNECTED_DEVICE.
