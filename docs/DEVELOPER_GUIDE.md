# StudioCast Developer Guide

This guide collects the source-build, architecture, daemon, IPC, model, and
testing notes that are too detailed for the top-level README.

For user-facing setup and usage, start with [../README.md](../README.md).

## Build from source

StudioCast currently targets Ubuntu 22.04 and 24.04. The setup helper supports
Ubuntu-family distributions and installs the common build/runtime dependencies,
ONNX Runtime, and v4l2loopback support.

One-shot development setup:

```bash
./scripts/setup.sh --deps --v4l2loopback --load-loopback --persist-loopback
./scripts/setup.sh --build --build-dir ./build --build-type Debug
```

Manual CMake build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target studiocast studiocastd studiocastctl
```

Useful development commands:

```bash
./build/studiocast --version
./build/studiocastd
./build/studiocastctl status --pretty
ctest --test-dir build --output-on-failure
./scripts/dev/format.sh
```

CLion often uses `cmake-build-debug/` or `cmake-build-release/` instead of
`build/`. Substitute the build directory in commands as needed.

See [SETUP.md](SETUP.md) for the longer install notes and v4l2loopback fallback
details.

## Repo layout

- [../CMakeLists.txt](../CMakeLists.txt): build graph, options, executable
  targets, and test targets.
- [../src/core](../src/core): shared non-Qt core code for config, IPC, audio,
  video, effects, CUDA, Maxine, ONNX Runtime, and utility code.
- [../src/daemon](../src/daemon): `studiocastd`, the background service that
  owns runtime state and device orchestration.
- [../src/gui](../src/gui): Qt GUI controller.
- [../src/tools](../src/tools): command-line helpers.
- [../tests](../tests): unit and integration-style tests that do not require
  full desktop hardware workflows.
- [../scripts](../scripts): setup, install, uninstall, model, and developer
  helper scripts. See [../scripts/README.md](../scripts/README.md).
- [../resources/model_packs](../resources/model_packs): metadata templates for
  curated model packs. Model binaries are downloaded separately.
- [../packaging/systemd/user](../packaging/systemd/user): systemd user service
  template for `studiocastd`.
- [../docs](../docs): architecture, setup, model installation, manual testing,
  trademark, roadmap, and design notes.

## Main binaries and tools

| Binary | Purpose |
| --- | --- |
| `studiocast` | Qt GUI controller for users. |
| `studiocastd` | Background daemon that owns camera/audio services, config, status, and effect availability. |
| `studiocastctl` | CLI client for daemon status, config, effects, audio/video controls, and debug reports. |
| `studiocast-open` | Open Video/Open Audio model path, listing, validation, and benchmark helper. |
| `studiocast-maxine` | Maxine path, GPU, doctor, install-hints, and smoke-test helper. |
| `studiocast-probe` | System probe/diagnostic helper. |
| `studiocast-audio` | Audio diagnostics/helper tool. |
| `studiocast-video` | Video diagnostics/helper tool. |

The install helper can symlink built binaries into `~/.local/bin` for a user
service workflow:

```bash
./scripts/install.sh user-service --build-dir ./build --yes
```

## Daemon architecture

`studiocastd` is the runtime authority. The GUI and CLI are controllers that
communicate with the daemon rather than directly owning the production device
state.

The daemon is responsible for:

- Loading persisted config from the XDG config location.
- Publishing status for the GUI and CLI.
- Keeping the virtual camera device available through v4l2loopback.
- Starting and stopping video processing based on consumer detection.
- Managing virtual microphone and speaker state.
- Resolving safe audio sources and targets.
- Computing effect and engine availability.
- Persisting canonical audio and video effect JSON.

Heavy video processing should be consumer-gated: the virtual camera can remain
available, but the camera pipeline should not keep doing expensive work when no
external app or GUI preview is consuming it. Audio routing distinguishes speaker
pass-through loopback from processed speaker effects in daemon status.

Existing architecture notes live in [ARCHITECTURE.md](ARCHITECTURE.md).

## IPC and socket behavior

The control plane is a small line-based Unix domain socket protocol.

Default socket path:

```text
$XDG_RUNTIME_DIR/studiocast/studiocastd.sock
```

If `XDG_RUNTIME_DIR` is unavailable, the socket helper falls back to a
per-user temporary runtime directory. The authoritative path is reported in
`studiocastctl status`.

Relevant source files:

- [../src/core/ipc/daemon_socket.cpp](../src/core/ipc/daemon_socket.cpp)
- [../src/core/ipc/daemon_client.cpp](../src/core/ipc/daemon_client.cpp)
- [../src/core/ipc/daemon_server.cpp](../src/core/ipc/daemon_server.cpp)
- [../src/daemon/studiocastd_main.cpp](../src/daemon/studiocastd_main.cpp)

Common daemon commands:

- `GET_STATUS`: full runtime status and diagnostics.
- `GET_CONFIG`: canonical video effects JSON.
- `GET_AUDIO_CONFIG`: canonical audio config/effects JSON.
- `SET_ENABLED`: enable/disable video pipeline intent.
- `SET_VIDEO_CONFIG`: patch video device and pipeline options.
- `SET_AUDIO_CONFIG`: patch audio device, routing, and effect options.
- `SET_VIDEO_EFFECTS_JSON`: patch canonical video effects JSON.

Use `studiocastctl` when possible instead of manually writing socket messages:

```bash
./build/studiocastctl status --pretty
./build/studiocastctl effects get
./build/studiocastctl effects set --file effects.json
./build/studiocastctl audio get
./build/studiocastctl audio set --file audio.json
```

## Audio pipeline overview

Audio code lives mainly under [../src/core/audio](../src/core/audio).

Important pieces:

- `virtual_mic.*`: StudioCast virtual microphone management.
- `virtual_speaker.*`: StudioCast virtual speaker management.
- `virtual_audio_service.*`: high-level virtual audio service coordination.
- `audio_pipeline.*`: real-time processing path when libpulse-simple is
  available.
- `audio_device_safety.*`: source/target validation to avoid feedback loops.
- `audio_backend_resolver.*`: backend selection for audio effects.
- `effects/broadcast_audio_effects*`: canonical audio effect model and JSON.
- `open_audio/*`: ONNX Runtime model discovery, diagnostics, and processing.
- `maxine/afx/*`: optional Maxine Audio Effects integration.

Safety rule: production audio config should go through daemon IPC. The daemon
and audio pipeline reject StudioCast virtual microphone sources, Pulse monitor
sources, and StudioCast virtual speaker targets where they could create feedback
or self-capture. `source: "auto"` resolves to a safe physical microphone when
possible; otherwise status should report an actionable error.

Canonical audio effects are persisted under `audio.effects.json` in the daemon
config.

## Video, effects, and model backends

Video code lives mainly under [../src/core/video](../src/core/video), with
backend-specific code under [../src/core/open_video](../src/core/open_video),
[../src/core/maxine](../src/core/maxine), and [../src/core/cuda](../src/core/cuda).

Key concepts:

- v4l2loopback provides the writable virtual camera device.
- `v4l2_capture.*` reads physical camera frames.
- `v4l2_writer.*` writes output frames to the virtual camera.
- `virtual_camera_service.*` coordinates the virtual camera lifecycle.
- `camera_pipeline.*` handles capture, effects, scaling, and output.
- `effects/broadcast_effects.*` defines the canonical video effect state.
- `effects/broadcast_effect_contract.h` defines stable effect IDs, parameter
  IDs, and ranges for IPC and JSON.

Engine preference values:

- `auto` / `auto_select`: prefer Maxine when available, otherwise use Open CUDA
  for supported effects.
- `maxine`: force the Maxine backend where supported.
- `open_cuda`: force the Open Video/Open CUDA backend where supported.

Availability is daemon-owned. The GUI should use `GET_STATUS` instead of trying
to infer local engine or model state. Daemon status reports nested engine
diagnostics such as `engines.maxine`, `engines.open_cuda`, and
`engines.open_audio`, plus compatibility aliases where present.

Open CUDA video effects are GPU-only. If CUDA, ONNX Runtime, or model packs are
missing, the effects should be marked unavailable rather than silently falling
back to CPU.

Canonical video effects are persisted under `video.effects.json` in the daemon
config.

## Model installation and validation

StudioCast keeps model binaries out of git. Metadata templates live under
[../resources/model_packs](../resources/model_packs), and install scripts fetch
curated assets into XDG data directories.

Open Video / Open CUDA:

```bash
./scripts/install.sh open-video-models --list
./scripts/install.sh open-video-models
./build/studiocast-open video-list-models
./build/studiocast-open video-self-test --model-id <id>
```

Details:

- [open_source_video_models_install.md](open_source_video_models_install.md)
- [open_source_video_model_conversion_to_onnx.md](open_source_video_model_conversion_to_onnx.md)
- [open_video_shared_inference.md](open_video_shared_inference.md)
- [how_to_add_your_own_model.md](how_to_add_your_own_model.md)

Open Audio:

```bash
./scripts/install.sh open-audio-models --list
./scripts/install.sh open-audio-models
./build/studiocast-open audio-list-models
./build/studiocast-open audio-self-test --model-id fastenhancer_s_vd_v1
./build/studiocast-open audio-bench --effect noise_removal --model-id fastenhancer_s_vd_v1 --seconds 10
```

Details:

- [open_source_audio_models_install.md](open_source_audio_models_install.md)

NVIDIA Maxine:

```bash
./build/studiocast-maxine init
./build/studiocast-maxine install-hints
./build/studiocast-maxine doctor
```

Details:

- [maxine_install.md](maxine_install.md)

StudioCast does not redistribute NVIDIA SDK assets, feature packs, model files,
or keys. Users must obtain those from NVIDIA and comply with NVIDIA's license
terms.

## Manual testing and debugging

Hardware, GUI, v4l2loopback, desktop app, GPU, and audio-routing workflows need
manual validation. Use [MANUAL_TESTING.md](MANUAL_TESTING.md) for regression
passes.

High-value commands:

```bash
./build/studiocastctl status --pretty
./build/studiocastctl debug-report --out studiocast-debug-report.txt
v4l2-ctl --list-devices
v4l2-ctl --all -d /dev/video10
pactl info
pactl list short sources
pactl list short sinks
pactl list short modules
journalctl --user -u studiocastd.service -f
```

Useful checks:

- Start `studiocastctl status` before the daemon and confirm it fails quickly
  with a clear socket error.
- Start `studiocastd`, then confirm status reports the configured virtual
  camera.
- Open OBS or a browser/WebRTC test page and confirm consumer detection starts
  the video pipeline.
- Close all consumers and confirm heavy processing returns to idle.
- Generate a debug report after failures.

## Packaging and systemd notes

The systemd user service template is:

- [../packaging/systemd/user/studiocastd.service](../packaging/systemd/user/studiocastd.service)

The install helper:

```bash
./scripts/install.sh user-service --build-dir ./build --yes
```

What it does:

- Creates or refreshes `~/.local/bin` symlinks to built StudioCast binaries.
- Copies the service file to `~/.config/systemd/user/studiocastd.service`.
- Runs `systemctl --user daemon-reload`.
- Enables and starts `studiocastd.service`.

Service commands:

```bash
systemctl --user status studiocastd.service
systemctl --user restart studiocastd.service
journalctl --user -u studiocastd.service -f
```

The current packaging flow is suitable for development and MVP testing. Treat
distribution packaging and polished non-developer install flows as future work.

## Deeper docs

- [SETUP.md](SETUP.md): source setup, dependencies, v4l2loopback, and optional
  backend setup.
- [ARCHITECTURE.md](ARCHITECTURE.md): canonical effect model and daemon-owned
  availability notes.
- [MANUAL_TESTING.md](MANUAL_TESTING.md): hardware and GUI manual regression
  plan.
- [ROADMAP.md](ROADMAP.md): project direction and planned work.
- [TRADEMARKS.md](TRADEMARKS.md): affiliation and trademark note.
- [../CONTRIBUTING.md](../CONTRIBUTING.md): contribution conventions.
- [../SECURITY.md](../SECURITY.md): security reporting.
