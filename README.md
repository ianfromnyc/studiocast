# StudioCast

StudioCast is intended to be an open-source Linux desktop application with a Broadcast-style UI for managing
real-time audio and video effects (planned: NVIDIA Maxine on Linux + PipeWire + v4l2loopback).

**Status:** Currently under development for initial POC/MVP

## Not affiliated with NVIDIA
StudioCast is independent and does **not** ship or redistribute NVIDIA Broadcast binaries.

## Build (Ubuntu 22.04)

For a one-shot prerequisites install on Ubuntu 22.04+:

```bash
./scripts/setup_ubuntu22.sh --deps --v4l2loopback --load-loopback --persist-loopback
./scripts/setup_ubuntu22.sh --build --build-type Debug
```

Manual build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/studiocast
```

See `docs/SETUP.md` for the full quickstart, including v4l2loopback and Maxine setup.


## Dev tooling
- Formatting: `./scripts/format.sh`
- Version: `./build/studiocast --version`
- Maxine install (SDK + features): see `docs/maxine_install.md` (or run `studiocast-maxine install-hints` for authoritative commands)
- Support bundle: `./build/studiocastctl debug-report --out studiocast-debug-report.txt`

## Effects model & availability (canonical)

- The single canonical effect schema is `BroadcastCameraEffects` (see `src/core/video/effects/broadcast_effect_contract.h`).
- Persistence + control plane use JSON:
  - `studiocastctl effects get` / `GET_CONFIG` returns the canonical effects JSON.
  - `studiocastctl effects set --file ...` sends a JSON patch (`SET_VIDEO_EFFECTS_JSON`) to update effects without shell-quoting issues.
- Effect availability is computed **only** by the daemon via `MaxineManager` and exposed in `GET_STATUS`.
  - The GUI must not try to “guess” availability client-side.
- There is **no CPU fallback**: effects are GPU-only (Maxine + small CUDA post-process where needed). If Maxine/GPU/driver/features are missing,
  effects must be treated as unavailable.

## Daemon mode (studiocastd)

StudioCast includes a background daemon (`studiocastd`) that keeps the virtual camera available and only
starts heavy video processing when a consumer opens the v4l2loopback device (OBS/Zoom/etc.).

During development you can run it manually:

```bash
./build/studiocastd
./build/studiocastctl status
```

The GUI (`studiocast`) acts as a thin controller by talking to the daemon over a Unix socket in
`$XDG_RUNTIME_DIR/studiocast/studiocastd.sock`.

There is also a systemd user service template in `packaging/systemd/user/studiocastd.service` (installation/packaging step).

## Next steps (Phase 1)
- Add Maxine SDK probing tooling (no redistribution of proprietary SDK assets)
- Expand daemon control plane to audio (PipeWire-Pulse) and add more CPU/GPU effects
