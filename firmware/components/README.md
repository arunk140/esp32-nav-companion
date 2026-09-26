# components/ — vendored forks (not committed yet)

- `map_tiles/` — fork of `0015/map_tiles` v1.3.0 with:
  1. grid centering fix (GPS at grid center, recenter only outside inner tile),
  2. 3x3 default, optional 1-tile cache.
  See `docs/waveshare-2.8c/10-map-tiles-reference.md`.

Vendor with: copy the fork here, or point `main/idf_component.yml` at the
fork's git URL. Do not use the upstream registry version unwatched.
