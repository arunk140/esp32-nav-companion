# Fork notes — `0015/map_tiles` v1.3.0 + nav-esp32 patches

Upstream: https://github.com/0015/map_tiles (MIT, retained in LICENSE).
Vendored 2026-09-26 because the registry version cannot be patched in place.

## Patches (all marked `nav-esp32` in source)

1. **Per-slot change tracking** (`map_tiles.cpp`, `include/map_tiles.h`):
   - `loaded_tx / loaded_ty / loaded_z` arrays, `INT32_MIN` = never loaded.
   - New `map_tiles_load_tile_if_changed(handle, index, x, y)`: skips the
     SD read when the slot already holds `(x, y)` at the current zoom.
   - Reason: stock callers reload every slot on each recenter — 9 × ~65-130 ms
     on this board's 1-bit SD bus. A one-tile pan now costs 3 reads.
   - Freed in `map_tiles_cleanup()`.

## Not patched (deliberately)

- Grid-origin formula (`tile_x = (int)x - grid_cols/2`): exact centering is
  handled by the caller (`main/map/map_view.c`), which keeps the GPS/marker at
  grid centre by construction. See `docs/waveshare-2.8c/10-map-tiles-reference.md`.
