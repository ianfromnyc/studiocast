# StudioCast

StudioCast is an open-source Linux desktop application with a Broadcast-style UI for managing
real-time audio and video effects (planned: NVIDIA Maxine on Linux + PipeWire + v4l2loopback).

**Status:** Phase 0 scaffolding — GUI skeleton only.

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

## Next steps (Phase 1)
- Add Maxine SDK probing tooling (no redistribution of proprietary SDK assets)
- Define IPC boundary between GUI and future audio/video services
