# StudioCast

StudioCast is intended to be an open-source Linux desktop application with a Broadcast-style UI for managing
real-time audio and video effects (planned: NVIDIA Maxine on Linux + PipeWire + v4l2loopback).

**Status:** Currently under development for initial POC/MVP

## Not affiliated with NVIDIA
StudioCast is independent and does **not** ship or redistribute NVIDIA Broadcast binaries.

## Use cases

- **Video calls / streaming with a virtual camera:** run the daemon to keep a v4l2loopback camera available, then use OBS/Zoom/etc. as the consumer. When supported (and when optional Maxine components are present), enable effects like noise removal or virtual background.
- **Headless / scripted control:** run `studiocastd` as a background service and use `studiocastctl` to query status, apply effect JSON, and gather a debug report.

## Build (Ubuntu 22.04 / 24.04)

StudioCast is known to build on Ubuntu **22.04** and **24.04**.

For a one-shot prerequisites install (tuned for Ubuntu 22.04+, including 24.04):

```bash
./scripts/setup_ubuntu22.sh --deps --v4l2loopback --load-loopback --persist-loopback
./scripts/setup_ubuntu22.sh --build --build-type Debug
```

Manual build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target studiocast
build/studiocast
```

Notes:

- CLion’s default build directory in this repo is `cmake-build-debug/`.
- If you hit a compiler/libstdc++ mismatch, explicitly pick a distro-matching compiler and re-configure (e.g. `g++-12` on 22.04, `g++-13` on 24.04).
- See `docs/SETUP.md` for the full quickstart, including v4l2loopback and optional Maxine setup.


## Dev tooling
- Formatting: `./scripts/format.sh`
- Version: `build/studiocast --version`
- Maxine install (SDK + features): see `docs/maxine_install.md` (or run `build/studiocast-maxine install-hints` for authoritative commands)
- Support bundle: `build/studiocastctl debug-report --out studiocast-debug-report.txt`

## Effects model & availability (canonical)

- The canonical effect schema type is `BroadcastCameraEffects` (see `src/core/video/effects/broadcast_effects.h`).
  Stable effect IDs / parameter IDs / ranges for IPC + JSON live in `src/core/video/effects/broadcast_effect_contract.h`.
- Persistence + control plane use JSON:
  - `build/studiocastctl effects get` / `GET_CONFIG` returns the canonical effects JSON.
  - `build/studiocastctl effects set --file ...` sends a JSON patch (`SET_VIDEO_EFFECTS_JSON`) to update effects without shell-quoting issues.
- Effect availability is computed **only** by the daemon via `MaxineManager` and exposed in `GET_STATUS`.
  - The GUI must not try to “guess” availability client-side.
- Maxine/AI effects are GPU-only (no CPU fallback). If Maxine/GPU/driver/features are missing, those effects must be treated as unavailable.
- Simple transforms (e.g., mirror) may still run on CPU.

## Daemon mode (studiocastd)

StudioCast includes a background daemon (`studiocastd`) that keeps the virtual camera available and only
starts heavy video processing when a consumer opens the v4l2loopback device (OBS/Zoom/etc.).

During development you can run it manually:

```bash
build/studiocastd
build/studiocastctl status
```

The GUI (`studiocast`) acts as a thin controller by talking to the daemon over a Unix socket in
`$XDG_RUNTIME_DIR/studiocast/studiocastd.sock`.

There is also a systemd user service template in `packaging/systemd/user/studiocastd.service` (installation/packaging step).

## Next steps
- Add Maxine SDK probing tooling (no redistribution of proprietary SDK assets)
- Expand daemon control plane to audio (PipeWire-Pulse) and add more CPU/GPU effects
