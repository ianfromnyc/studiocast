# StudioCast

StudioCast is an open-source Linux application that provides a virtual camera and virtual audio devices (microphone and speakers) with real-time effects for video calls, streaming, and recording.

Status: early preview / proof-of-concept. It is usable on Ubuntu 22.04+ and is still under active development.

## Not affiliated with NVIDIA

StudioCast is independent and does not ship or redistribute NVIDIA Broadcast binaries.

## What StudioCast is for

- Virtual camera for apps like OBS, Zoom/Teams, Discord, and browser/WebRTC
- Optional real-time video effects (for example background effects and eye contact, depending on what is installed and enabled)
- Optional real-time audio effects (noise suppression / enhancement, depending on what is installed and enabled)
- A background daemon that keeps virtual devices available and only does heavy work when an app is consuming them

## Quick start (most users)

StudioCast currently targets Ubuntu 22.04 and 24.04.

### 1) Set up the system (one-time per machine)

This installs OS dependencies and sets up the v4l2loopback virtual camera device.

```bash
./scripts/setup.sh --deps --v4l2loopback --load-loopback --persist-loopback
````

If this step fails, fix it first. Without v4l2loopback you will not get a virtual camera device.

### 2) Build StudioCast

```bash
./scripts/setup.sh --build --build-type Release
```

If you prefer Debug builds during testing:

```bash
./scripts/setup.sh --build --build-type Debug
```

### 3) Start StudioCast

You can run manually during development:

```bash
build/studiocastd
```

In another terminal:

```bash
build/studiocastctl status
```

### 4) Use StudioCast in OBS / Zoom / Teams / Discord / WebRTC

* In your target app, select the StudioCast virtual camera (v4l2loopback device).
* If StudioCast virtual audio devices are enabled/available on your system, select the StudioCast virtual microphone/speakers in your app’s audio settings.
* Open the StudioCast GUI (if built) to enable/disable effects and adjust settings.

Notes:

* StudioCast aims to avoid heavy processing when nothing is consuming the virtual camera. If you see heavy load with no consumer, that is a bug.
* Some effects require specific runtimes or model packs to be installed. See the “Models and engines” section below.

## Common commands (users)

Check status:

```bash
build/studiocastctl status
```

Generate a support bundle (useful when reporting issues):

```bash
build/studiocastctl debug-report --out studiocast-debug-report.txt
```

## Models and engines (users)

StudioCast supports multiple backends. What is available depends on what is installed on your machine.

* Open Video (ONNX Runtime + model packs): see `docs/open_source_video_install.md`
* Open Audio (ONNX Runtime + model packs): see `docs/open_source_audio_install.md`
* Maxine SDK (optional dependency): see `docs/maxine_install.md`

## Troubleshooting (users)

### Virtual camera does not appear

* Confirm v4l2loopback is installed and loaded:

  * `/dev/video*` should include the configured loopback node (for example `/dev/video10`)
* Re-run setup if needed:

  ```bash
  ./scripts/setup.sh --v4l2loopback --load-loopback --persist-loopback
  ```

### The daemon runs but apps show no video

* Verify the daemon is running and the virtual camera device is present.
* Use:

  ```bash
  build/studiocastctl status
  ```
* If reporting a bug, include:

  ```bash
  build/studiocastctl debug-report --out studiocast-debug-report.txt
  ```

---

# Developer guide

This section is for building, hacking on StudioCast, and understanding the architecture.

## Build (Ubuntu 22.04 / 24.04)

One-shot:

```bash
./scripts/setup.sh --deps --v4l2loopback --load-loopback --persist-loopback
./scripts/setup.sh --build --build-type Debug
```

Manual build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target studiocast
build/studiocast
```

Notes:

* CLion default build directory is often `cmake-build-debug/`.
* If you hit a compiler/libstdc++ mismatch, pick a distro-matching compiler and re-configure.
* Setup docs live in `docs/SETUP.md`.

## Dev tooling

* Formatting: `./scripts/format.sh`
* Version: `build/studiocast --version`
* Maxine install hints: `docs/maxine_install.md` or `build/studiocast-maxine install-hints`
* Open Video model packs: `build/studiocast-open install-hints` and `build/studiocast-open list-models`
* Support bundle: `build/studiocastctl debug-report --out studiocast-debug-report.txt`

## Daemon mode (studiocastd)

StudioCast includes a background daemon (`studiocastd`) that keeps the virtual camera available and only starts heavy video processing when a consumer opens the v4l2loopback device (OBS/Zoom/etc.).

During development you can run it manually:

```bash
build/studiocastd
build/studiocastctl status
```

The GUI (`studiocast`) acts as a controller and talks to the daemon over a Unix socket at:

`$XDG_RUNTIME_DIR/studiocast/studiocastd.sock`

There is also a systemd user service template in:

`packaging/systemd/user/studiocastd.service`

Install + enable it for dev/MVP testing:

```bash
./scripts/install.sh user-service --build-dir ./cmake-build-debug --yes
systemctl --user status studiocastd.service
```

## Effects model and availability (canonical)

* Canonical effect schema type: `BroadcastCameraEffects` in `src/core/video/effects/broadcast_effects.h`
* Stable effect IDs / parameter IDs / ranges for IPC + JSON: `src/core/video/effects/broadcast_effect_contract.h`
* Persistence + control plane use JSON:

  * `build/studiocastctl effects get` returns the canonical effects JSON
  * `build/studiocastctl effects set --file ...` sends a JSON patch
* Effect availability is computed by the daemon and exposed in status.

  * The GUI should not try to guess availability client-side.

## Next steps

* Improve stability under load and consumer connect/disconnect behavior
* Improve packaging and install flow for non-developers
* Adaptive streaming (e.g. adaptive bitrates/frame sizes to reduce latency on the fly)