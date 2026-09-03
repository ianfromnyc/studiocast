# StudioCast Developer Guide

This guide collects the source-build, architecture, daemon, IPC, model, and
testing notes that are too detailed for the top-level README.

For user-facing setup and usage, start with [../README.md](../README.md).

## Build from source

StudioCast currently targets Ubuntu 22.04 and 24.04. The setup helper supports
Ubuntu-family distributions and installs the common build/runtime dependencies,
ONNX Runtime, and v4l2loopback support. Fedora 44 is an early-preview target:
the setup helper handles its dependencies and v4l2loopback with the same flags,
and `packaging/rpm/build_rpm.sh` builds an RPM package. See [SETUP.md](SETUP.md)
for the Fedora notes.

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
./build/studiocast-installer --version
./build/studiocastd
./build/studiocastctl status --pretty
ctest --test-dir build --output-on-failure
./scripts/dev/format.sh
```

CLion often uses `cmake-build-debug/` or `cmake-build-release/` instead of
`build/`. Substitute the build directory in commands as needed.

See [SETUP.md](SETUP.md) for the longer install notes and v4l2loopback fallback
details.

## Versioning

The canonical project version lives in the top-level
[../VERSION](../VERSION) file as `MAJOR.MINOR.PATCH`. CMake reads that file
before `project()`, so `PROJECT_VERSION`, the generated
`studiocast/version.h`, CLI `--version` output, daemon status, and GUI About
surfaces all use the same value.

The current automatic versioning line starts at `0.2.0`. On every normal push
to `master`, `.github/workflows/version-bump.yml` increments the patch component
and commits the updated `VERSION` file back to `master` with a `[skip ci]`
message. The workflow skips bot commits and skips pushes that already changed
`VERSION`, which prevents commit loops and lets maintainers intentionally set a
new baseline such as `0.3.0`.

If `master` is protected, repository settings must allow the GitHub Actions bot
to push the version-bump commit.

For releases, wait for the automatic version-bump commit to land on `master`,
then tag that commit with the matching version:

```bash
git fetch origin master --tags
git checkout origin/master
git tag -a v0.2.1 -m "StudioCast 0.2.1"
git push origin v0.2.1
```

If maintainers need a new minor or major line, update `VERSION` in a normal
change, merge it to `master`, and tag the resulting commit if it is a release.

## Repo layout

- [../CMakeLists.txt](../CMakeLists.txt): build graph, options, executable
  targets, and test targets.
- [../src/core](../src/core): shared non-Qt core code for config, IPC, audio,
  video, effects, CUDA, Maxine, ONNX Runtime, and utility code.
- [../src/daemon](../src/daemon): `studiocastd`, the background service that
  owns runtime state and device orchestration.
- [../src/gui](../src/gui): Qt GUI controller.
- [../src/tools](../src/tools): command-line helpers.
- [../installer/gui](../installer/gui): standalone Qt installer wizard.
- [../installer/backend](../installer/backend): scriptable installer backend
  used by the GUI and CLI fallback.
- [../tests](../tests): unit and integration-style tests that do not require
  full desktop hardware workflows.
- [../scripts](../scripts): setup, install, uninstall, model, and developer
  helper scripts. See [../scripts/README.md](../scripts/README.md).
- [../resources/model_packs](../resources/model_packs): metadata templates for
  curated model packs. Model binaries are downloaded separately.
- [../packaging/systemd/user](../packaging/systemd/user): systemd user service
  template for `studiocastd`.
- [../packaging/appimage](../packaging/appimage): release packaging scaffold
  for the standalone GUI installer bundle.
- [../packaging/rpm](../packaging/rpm): Fedora spec template, desktop entry,
  icon, and the RPM build and verify scripts.
- [../packaging/_lib](../packaging/_lib): shared shell helpers for the
  packaging scripts.
- [../docs](../docs): architecture, setup, model installation, manual testing,
  trademark, roadmap, and design notes.

## Main binaries and tools

| Binary | Purpose |
| --- | --- |
| `studiocast` | Qt GUI controller for users. |
| `studiocast-installer` | Qt installer wizard that calls the scriptable backend. |
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

## Installer architecture

The installer is intentionally split into a GUI front end and a scriptable
backend:

- `studiocast-installer` is a separate Qt Widgets target. It does not require
  StudioCast to already be installed and refuses to run as root.
- `installer/backend/studiocast-installer-backend` owns OS detection, planning,
  install/update/repair/uninstall/clean-install execution, and manifest writes.
- `scripts/installer.sh` is the stable CLI entrypoint for CI, SSH, recovery,
  and debugging.
- Existing `scripts/setup.sh`, `scripts/install.sh`, and `scripts/uninstall.sh`
  remain compatibility entrypoints. The backend delegates dependency setup,
  v4l2loopback setup, binary linking, model installs, and systemd user service
  setup to those scripts instead of duplicating their logic.

Backend examples:

```bash
./scripts/installer.sh detect-os --json
./scripts/installer.sh status --json
./scripts/installer.sh plan install --json
./scripts/installer.sh install --yes
./scripts/installer.sh update --source-dir /path/to/release-source --yes
./scripts/installer.sh update --release-archive ~/Downloads/studiocast.tar.gz --yes
./scripts/installer.sh repair --yes
./scripts/installer.sh uninstall --yes
./scripts/installer.sh clean-install --yes
```

The installer backend supports Ubuntu 22.04/Jammy, Ubuntu 24.04/Noble, and Linux
Mint when `/etc/os-release` exposes a reliable Ubuntu base. Mint
`UBUNTU_CODENAME=jammy` maps to Ubuntu 22.04; `UBUNTU_CODENAME=noble` maps to
Ubuntu 24.04. If that field is absent, Mint 21.x maps to Jammy and Mint 22.x
maps to Noble.

The install manifest lives at:

```text
~/.local/share/studiocast/install-manifest.json
```

It records installed version, source/build/install paths, installed binaries,
systemd user service path/status, dependency install method, install timestamp,
and whether config/models/logs/cache were preserved. Update and repair prefer
manifest paths unless a source directory or release archive is explicitly
selected. Uninstall removes app symlinks, service files, desktop entries, and
runtime artifacts. Clean install preserves user config, downloaded models, logs,
and cache unless `--remove-user-data` is explicitly selected.

Release packaging:

- `packaging/appimage/build_appimage.sh` configures an isolated Release build,
  builds only `studiocast-installer`, installs the `Installer` CMake component
  into an AppDir, adds desktop/icon metadata, archives the AppDir, and writes
  SHA256 checksums.
- The AppDir layout places the backend at
  `usr/share/studiocast/installer/studiocast-installer-backend`, which is the
  installed path the GUI already probes relative to the installer binary.
- The same packaging script creates `StudioCast-<version>-source.tar.gz` with
  the shared helper `packaging/_lib/source_archive.sh`, which uses `git archive`
  on `HEAD` when git is available. The script stages the archive at
  `usr/share/studiocast/source/StudioCast-<version>-source.tar.gz` and leaves
  the standalone source archive in `dist/appimage/`.
- Release AppImages are self-contained for the installer GUI, backend, and
  matching source archive. Runtime dependencies still come from supported system
  packages, ONNX Runtime/model helpers, optional SDK assets, and the installer
  backend scripts.
- Local packaging does not download tools. If `linuxdeploy` and
  `linuxdeploy-plugin-qt` are available, the script also creates
  `StudioCast-Installer-<version>-<arch>.AppImage`; otherwise it leaves the
  staged AppDir tarball as the local artifact.
- Release CI is in `.github/workflows/release-packaging.yml`. It runs only from
  `workflow_dispatch` or a published GitHub Release event, downloads AppImage
  packaging tools from `packaging/appimage/tools.lock`, verifies each tool's
  SHA256 before making it executable, requires AppImage generation, and uploads
  the installer bundle, AppDir archive, source archive, and checksum file as
  workflow artifacts. It does not tag commits or publish release assets by
  itself. The same workflow carries the `rpm-fedora-44` job for the Fedora
  package, which is described below.
- The packaged installer is still a source-build installer. The GUI defaults to
  the bundled source archive, and users can still point it at another release
  archive or a local checkout.

Fedora RPM packaging:

- `packaging/rpm/build_rpm.sh` renders `packaging/rpm/studiocast.spec.in` into
  a spec with the `VERSION` value, creates the source archive, and builds the
  source RPM and the binary RPMs in a private rpmbuild tree under `build/rpm`.
  It never touches `~/rpmbuild`.
- The script uses the same `packaging/_lib/source_archive.sh` helper as the
  AppImage script, so both flows ship an identical
  `StudioCast-<version>-source.tar.gz`.
- Results land in `dist/rpm/`: `studiocast-<version>-1.fc44.src.rpm`,
  `studiocast-<version>-1.fc44.x86_64.rpm`, the matching
  `studiocast-debuginfo` and `studiocast-debugsource` packages, and a
  `studiocast-<version>-rpm.sha256` checksum file.
- `--container` runs rpmbuild inside a `registry.fedoraproject.org/fedora:44`
  podman or docker container. Use it on a host that is not Fedora 44. The
  container build takes about 2.5 minutes on a 32-core machine.
- Other options: `--srpm-only` builds the source RPM only, `--with NAME` and
  `--without NAME` change a spec build conditional, `--install-builddeps` runs
  `dnf builddep` on the rendered spec before a native build (needs root or
  sudo; container mode installs its own build dependencies), and `--rpmlint`
  prints an rpmlint report. The rpmlint status never fails the script.
- Spec build conditionals: `open_cuda` (on), `open_audio` (on), `libyuv` (on),
  and `tests` (on); `dlib` (off) and `installer` (off). With `tests`, `%check`
  runs the full ctest suite. `dlib` stays off because Fedora has no dlib
  package, so Open Video eye contact is unavailable in the RPM. `installer`
  stays off because the installer backend is Ubuntu-only. The spec is
  `ExclusiveArch: x86_64`.
- The package ships the `Runtime` CMake component only: `studiocast`,
  `studiocastd`, `studiocastctl`, `studiocast-probe`, `studiocast-maxine`,
  `studiocast-open`, `studiocast-audio`, and `studiocast-video`, plus a desktop
  entry, an icon, the license files, and the systemd user unit.
- Run-time `Requires` are `pulseaudio-utils`, `v4l-utils`, and
  `hicolor-icon-theme`, plus the soname dependencies that rpmbuild finds for
  Qt6, libpulse, ONNX Runtime, libjpeg, libpng, sqlite, and libyuv.
  `Recommends: v4l2loopback` is a weak dependency, so `dnf` installs the package
  when it is absent. The module comes from RPM Fusion Free as
  `akmod-v4l2loopback`, and the virtual camera needs it.
- The Fedora `onnxruntime` package is CPU-only and has no CUDA execution
  provider. GPU Open CUDA inference on Fedora therefore needs an upstream GPU
  ONNX Runtime that the user installs. Open Audio runs on the CPU, so it works
  with the Fedora package.
- `studiocast --version` reports the git SHA as `unknown` in an RPM build,
  because `git archive` puts no git metadata in the source archive.
- The packaged unit comes from
  `packaging/systemd/user/studiocastd-system.service.in`. The build replaces
  `@BINDIR@` and installs the result as
  `/usr/lib/systemd/user/studiocastd.service` with
  `ExecStart=/usr/bin/studiocastd`. The source-install template
  `packaging/systemd/user/studiocastd.service` is a different file and keeps
  `ExecStart=%h/.local/bin/studiocastd`.
- `packaging/rpm/verify_rpm.sh` checks the artifacts: package names, metadata,
  the file list, the declared dependencies, and the checksum file.
  `--install-test` installs the binary RPM with `dnf`, runs the programs, and
  removes the package again. That test needs root, so use `--container` to run
  it in a Fedora 44 container. `--no-container-check` lets it run directly on a
  disposable root system, such as a CI container job.
- CI: `.github/workflows/release-packaging.yml` has the `rpm-fedora-44` job. It
  runs in a `registry.fedoraproject.org/fedora:44` container, always on release
  events, and on `workflow_dispatch` when the `build_rpm` input is true, which
  is the default. It uploads the RPMs and the SHA256 file as the
  `studiocast-rpm-fedora-44` artifact. `.github/workflows/ci.yml` has the
  `rpm-package-smoke` job, which runs the same build on every push and is the
  only Fedora/GCC 16 compile check in push CI. Both jobs run the scripts
  natively with `--install-builddeps` and `--no-container-check`, because a
  GitHub container job cannot start podman.

First release checklist:

1. Merge the release change to `master`.
2. Wait for `.github/workflows/version-bump.yml` to commit the next `VERSION`
   value back to `master`.
3. Fetch the updated branch and tags:

   ```bash
   git fetch origin master --tags
   git checkout origin/master
   ```

4. Confirm the release version:

   ```bash
   cat VERSION
   ```

5. Create and push an annotated tag that matches `VERSION`:

   ```bash
   version="$(cat VERSION)"
   git tag -a "v${version}" -m "StudioCast ${version}"
   git push origin "v${version}"
   ```

6. In GitHub, create and publish a Release for that tag. Publishing the Release
   triggers `.github/workflows/release-packaging.yml`.
7. After release packaging finishes, download the
   `studiocast-gui-installer-ubuntu-22.04` workflow artifact and attach the
   AppImage, AppDir archive, source archive, and SHA256 file to the GitHub
   Release. Download the `studiocast-rpm-fedora-44` artifact as well, and attach
   the source RPM, the binary RPM, and its SHA256 file.

Pinned AppImage tool updates:

1. Choose fixed release asset URLs for `linuxdeploy` and
   `linuxdeploy-plugin-qt`; do not pin to upstream `continuous` URLs.
2. Download each AppImage and record `sha256sum <file>`.
3. Update `packaging/appimage/tools.lock` with the matching version, URL, and
   SHA256 values in one change.
4. Run release packaging or a workflow-dispatch dry run so CI verifies the
   checksums before either packaging tool is executed.

Maintainer command:

```bash
packaging/appimage/build_appimage.sh --clean
```

For release-equivalent local validation with preinstalled packaging tools:

```bash
packaging/appimage/build_appimage.sh --clean --appimage-required
```

Maintainer commands for the Fedora RPM, on a Fedora 44 host (add
`--install-builddeps` to the build command when the build dependencies are not
installed yet):

```bash
packaging/rpm/build_rpm.sh --clean --rpmlint
packaging/rpm/verify_rpm.sh --install-test --container
```

The same commands on a host that is not Fedora 44:

```bash
packaging/rpm/build_rpm.sh --clean --rpmlint --container
packaging/rpm/verify_rpm.sh --install-test --container
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

There are two systemd user service templates:

- [../packaging/systemd/user/studiocastd.service](../packaging/systemd/user/studiocastd.service)
  is for source installs. It keeps `ExecStart=%h/.local/bin/studiocastd`,
  because the install helper copies it into `~/.config/systemd/user/`.
- [../packaging/systemd/user/studiocastd-system.service.in](../packaging/systemd/user/studiocastd-system.service.in)
  is for distribution packages. The RPM build replaces `@BINDIR@` and installs
  the result as `/usr/lib/systemd/user/studiocastd.service` with
  `ExecStart=/usr/bin/studiocastd`.

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

The RPM does not enable its unit. A packaged install starts the daemon after:

```bash
systemctl --user enable --now studiocastd.service
```

A unit in `~/.config/systemd/user/` always wins over the packaged unit. A user
who did a source install first therefore keeps starting
`~/.local/bin/studiocastd` after an RPM install. Remove the old copy before you
switch to the package:

```bash
./scripts/uninstall.sh
```

The uninstall script stops the unit, deletes
`~/.config/systemd/user/studiocastd.service`, and reloads the user manager.
Deleting that file by hand has the same result if you then run:

```bash
systemctl --user daemon-reload
```

The Fedora RPM is the first distribution package. The source-build flow remains
suitable for development and MVP testing. Treat packages for other
distributions and polished non-developer install flows as future work.

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
