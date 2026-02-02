# StudioCast

StudioCast is intended to be an open-source Linux desktop application with a Broadcast-style UI for managing
real-time audio and video effects (planned: NVIDIA Maxine on Linux + PipeWire + v4l2loopback).

**Status:** Currently under development for initial POC/MVP

## Not affiliated with NVIDIA
StudioCast is independent and does **not** ship or redistribute NVIDIA Broadcast binaries.

## Build (Ubuntu 22.04)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config qt6-base-dev qt6-base-dev-tools qtbase5-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/studiocast
```

## Dev tooling
- Formatting: `./scripts/format.sh`
- Version: `./build/studiocast --version`

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
