# StudioCast Developer Guide

This guide collects the source-build, architecture, daemon, IPC, model, and
testing notes that are too detailed for the top-level README.

For user-facing setup and usage, start with [../README.md](../README.md).

## Build from source

StudioCast currently targets Ubuntu 22.04 and 24.04. The setup helper supports
Ubuntu-family distributions and installs the common build/runtime dependencies,
ONNX Runtime, and v4l2loopback support. Fedora 44 is an early-preview target:
`./scripts/setup.sh` runs `scripts/setup/fedora.sh` there, which installs the
dependencies and sets up v4l2loopback with the same flags, and
`packaging/rpm/build_rpm.sh` builds an RPM package. Fedora ships no dlib
package, so `scripts/install/dlib.sh` builds the pinned dlib when a source
build needs Eye Contact. See [SETUP.md](SETUP.md) for the Fedora notes.

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
  a spec with the `VERSION` value and the pinned dlib version, creates the
  source archive, downloads the dlib source, and builds the source RPM and the
  binary RPMs in a private rpmbuild tree under `build/rpm`. It never touches
  `~/rpmbuild`.
- The script uses the same `packaging/_lib/source_archive.sh` helper as the
  AppImage script, so both flows ship an identical
  `StudioCast-<version>-source.tar.gz`.
- Results land in `dist/rpm/`: `studiocast-<version>-1.fc44.src.rpm`,
  `studiocast-<version>-1.fc44.x86_64.rpm`, the matching
  `studiocast-debuginfo` and `studiocast-debugsource` packages, and a
  `studiocast-<version>-rpm.sha256` checksum file.
- `--container` runs rpmbuild inside a `registry.fedoraproject.org/fedora:44`
  podman or docker container. Use it on a host that is not Fedora 44. The
  container build takes about 2.5 minutes on a 32-core machine. The container
  runs `packaging/rpm/container_build.sh`, which the script copies into the
  build directory; the install test runs `packaging/rpm/install_test.sh` over
  stdin. Both are checked-in files, so `shellcheck -x` reads them like the
  other packaging scripts.
- Other options: `--srpm-only` builds the source RPM only, `--with NAME` and
  `--without NAME` change a spec build conditional, `--install-builddeps` runs
  `dnf builddep` on the rendered spec before a native build (needs root or
  sudo; container mode installs its own build dependencies), `--rpmlint`
  prints an rpmlint report, and `--keep-downloads` keeps the cached dlib
  tarball when `--clean` removes the build tree. The rpmlint status never
  fails the script.
- Spec build conditionals: `open_cuda` (on), `open_audio` (on), `libyuv` (on),
  `dlib` (on), and `tests` (on); `installer` (off). With `tests`, `%check`
  runs the full ctest suite. `installer` stays off because the installer
  backend is Ubuntu-only. The spec is `ExclusiveArch: x86_64`.
- Every conditional reaches the configure step, so the source package decides
  the binary. `libyuv` sets `STUDIOCAST_ENABLE_LIBYUV`, a three-way option:
  `AUTO` is the default for a plain source build and falls back to the built-in
  scalar, SSSE3 and AVX2 conversion; `ON`, which `--with libyuv` passes, fails
  the configure step when libyuv is missing; `OFF`, which `--without libyuv`
  passes, ignores a libyuv that the build machine has installed. A build with
  libyuv also declares `Provides: studiocast(libyuv)`.

Bundled dlib in the RPM:

- Fedora ships no dlib package, so the RPM build makes its own. `Source1` is
  the dlib release tarball pinned in `packaging/rpm/dlib.lock`, which holds
  `DLIB_VERSION`, `DLIB_URL`, `DLIB_SHA256`, and the license note.
- `build_rpm.sh` downloads that tarball on the host, checks the SHA256, caches
  it under `build/rpm/downloads`, and copies it into the rpmbuild `SOURCES`
  directory. The container never reaches the network for the sources. A
  checksum mismatch stops the build.
- `%build` compiles dlib into a private prefix inside the build tree and
  passes `-Ddlib_DIR=<prefix>/lib64/cmake/dlib` to the StudioCast configure
  step. The library is static, position independent, and built with the Fedora
  hardening, LTO, and annobin flags, so the package ships no extra shared
  object. Image codecs, the GUI, FFmpeg, and CUDA are all off, because
  StudioCast uses the shape predictor and the plain image processing headers
  only.
- dlib links CBLAS and LAPACK through `flexiblas-devel`, selected with
  `-DBLA_VENDOR=FlexiBLAS`. FlexiBLAS is the Fedora BLAS front end: it keeps
  one link-time interface and lets the machine owner pick the back end
  (OpenBLAS by default) at run time. The package therefore requires
  `libflexiblas.so.3`.
- The package declares `Provides: bundled(dlib) = <version>`, installs the
  upstream license as `/usr/share/licenses/studiocast/dlib-LICENSE.txt`, and
  carries the license tag `MPL-2.0 AND BSL-1.0`.
- `--without dlib` skips the download and the extra build. The package is then
  plain `MPL-2.0`, has no `bundled(dlib)` provide, and Open Video Eye Contact
  is unavailable.
- To bump the pin: change `DLIB_VERSION`, `DLIB_URL`, and `DLIB_SHA256` in
  `packaging/rpm/dlib.lock` in one change, then run
  `packaging/rpm/build_rpm.sh --clean --container --rpmlint` and
  `packaging/rpm/verify_rpm.sh --install-test --container`. The lock file holds
  the full procedure.
- `scripts/install/dlib.sh` builds the same pinned dlib for a Fedora source
  build, into `/opt/studiocast/dlib/<version>` by default, and prints the
  `-Ddlib_DIR` value to pass to CMake. It supports `--prefix`, `--jobs`,
  `--dry-run`, and `-y`. The RPM build does not need it.
- The package ships the `Runtime` CMake component only: `studiocast`,
  `studiocastd`, `studiocastctl`, `studiocast-probe`, `studiocast-maxine`,
  `studiocast-open`, `studiocast-audio`, and `studiocast-video`, plus a desktop
  entry, an icon, the license files, and the systemd user unit.
- Run-time `Requires` are `pulseaudio-utils`, `v4l-utils`, and
  `hicolor-icon-theme`, plus the soname dependencies that rpmbuild finds for
  Qt6, libpulse, ONNX Runtime, libjpeg, libpng, and libyuv, plus FlexiBLAS in
  a dlib build. Nothing links sqlite, so the package has no sqlite dependency.
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
- The script reads `rpm -qp --provides` for `bundled(dlib)` and for
  `studiocast(libyuv)`, and adapts. For a dlib package it also expects the
  license tag, the dlib license file, and the FlexiBLAS dependency, and the
  install test looks for dlib type names inside
  `/usr/bin/studiocastd`. That binary check is the cheapest observable: dlib
  shows up in the product only through Open Video Eye Contact, which needs a
  running daemon, a camera, and an installed model pack.
- For a libyuv package the script expects the `libyuv.so.0` dependency, and for
  a package without the provide it expects no libyuv dependency at all. The two
  checks together prove that the build conditional, not the build machine,
  decided the backend.
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
./build/studiocastctl audio monitor on --sink alsa_output.usb_headset
./build/studiocastctl audio monitor status
```

## Audio pipeline overview

Audio code lives mainly under [../src/core/audio](../src/core/audio).

Important pieces:

- `virtual_mic.*`: StudioCast virtual microphone management.
- `virtual_speaker.*`: StudioCast virtual speaker management.
- `mic_monitor.*`: microphone monitor loopback (`studiocast_mic` to a chosen
  output sink) with its own safety and stale-module rules.
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

### Microphone monitor

The monitor plays the processed microphone feed on an output sink so the user
can hear the effects while adjusting them. It is a Pulse `module-loopback` from
`studiocast_mic` into the chosen sink. Both loopback streams carry
`media.name=StudioCast_Microphone_Monitor`. The name has no spaces because the
Pulse module argument parser keeps only the text before the first space of a
property value. StudioCast finds and unloads its own monitor loopback by that
tag. The service does this once at every start, whatever the monitor setting
says, so a loopback that a killed daemon left behind is removed and a daemon
restart never leaves a second monitor behind.

Config lives in the audio config JSON under an additive `monitor` object
(`enabled`, `sink`, `latency_ms`, `volume`) and in the daemon config under
`audio.monitor.*`. The audio config JSON carries no schema version of its own,
so additive fields need no version bump.

Safety reuses the speaker target rules and adds one more: the monitor refuses a
sink whose monitor source is the selected microphone input, because that is a
feedback loop.

A sink of `"auto"` is resolved once, at the start, and the resolved name is
pinned. Only an explicit restart resolves it again: a change to the sink or the
delay, or the monitor going from off to on. A restart the user did not ask for,
such as the one after a failed route check, uses the pinned name. This is
because Pulse moves its default output on an unplug, usually to the built-in
loudspeakers, so a monitor that resolved `"auto"` again would build a feedback
loop on its own. When the output disappears the monitor stops and writes what
happened to `monitor.note`, not to `monitor.last_error`, because the GUI prints
the note as written and turns any error into a request to open Support.

The pin does not outlive the output it names. A start that used the pinned name
and failed asks the sound server whether that sink is still in its list. While
the sink is there the pin holds and the start is retried, so a sound server
hiccup cannot move the monitor. When the sink is gone the service reports the
lost output, drops the pin and stops, so the next explicit restart resolves
`"auto"` afresh instead of asking for a sink that no longer exists. A sound
server that cannot be reached gives no answer at all, and no answer keeps the
pin: a monitor that only waits for the sound server comes back on its own,
while a lost output needs the user.

The daemon never writes the monitor setting back. A safety check that finds no
usable output gives a warning and leaves `monitor.enabled` as the user wrote
it, because the same check fails when the sound server is only unreadable for a
moment and the config it checks is the one that is saved to disk.

The monitor talks to Pulse directly: `StartMicMonitor` and `StopMicMonitor`
call `pactl` and know `module-loopback` and the name `studiocast_mic`.
`VirtualAudioServiceHooks` gives a seam for the tests, but there is no
transport seam, so the monitor needs `pipewire-pulse` (or a real PulseAudio
server) even when the rest of the audio path uses another backend. A native
transport would need a second implementation of the route, which the monitor
does not have yet.

The monitor is a consumer of `studiocast_mic`, so it starts the microphone
pipeline on its own. Status reports `mic_app_consumer_count` and
`mic_monitor_consumer_count` next to `mic_consumer_count` so readiness text can
still speak about apps, and the microphone readiness detail says when only the
monitor is listening.

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

### Maxine SDK layouts

StudioCast supports two SDK layouts. `src/core/maxine/paths.cpp` resolves them
and records which one it found in `models_dir_source`.

| Item | Legacy SDK (0.7/0.8) | SDK Core 1.x |
| --- | --- | --- |
| Core library | `<root>/lib/libVideoFX.so` | same |
| Models | `<root>/models/` | `<root>/lib/models/` |
| Features | `<root>/features/<name>/` | same |
| Bundled runtime | none | `<root>/external/cuda/lib`, `<root>/external/tensorrt/lib` |

The SDK Core 1.x libraries carry no usable RPATH, and their CUDA 12 and
TensorRT 10 dependencies exist only under `<root>/external`. Before the VFX,
AR, AFX or NvCVImage loader calls dlopen, `src/core/maxine/sdk_runtime.cpp`
reads the DT_NEEDED list of the target library and pre-loads the SDK copy of
each soname it ships, in dependency order, with `RTLD_NOW | RTLD_LOCAL`. Local
is enough: the loader still satisfies a later DT_NEEDED from the link map by
soname, and the SDK's own TensorRT and CUDA stay out of the process-wide
namespace, where ONNX Runtime, built against other versions of the same
sonames, would otherwise bind to them. Core system libraries and the driver
library stay with the system loader. So the core SDK libraries need no
`LD_LIBRARY_PATH`.

The AFX *feature* libraries are the exception. The AFX core opens them by
their bare names, so their directories must be on the loader path of the
process, and glibc reads `LD_LIBRARY_PATH` only at start.
`src/core/maxine/afx/afx_loader_path.cpp` therefore sets the variable and
starts the program again, one time, before the daemon opens any socket. See
`EnsureAfxFeatureLibsOnLoaderPath`.

Effect and parameter selector strings (`GreenScreen`, `BackgroundBlur`,
`ModelDir`, `SrcImage0`, ...) live in `src/core/maxine/vfx_api.h` and must match
`include/nvVideoEffects.h` and the per-feature headers of the SDK.

Run `studiocast-maxine doctor` to see the resolved root, library, pre-loaded
runtime, models directory and features directory of each component.

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
