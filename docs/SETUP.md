# Setup / Install (Ubuntu 22.04+ and Fedora 44)

This repo supports both a manual source-build flow and a StudioCast installer
wizard target. The GUI installer is the polished user path for releases; the
scripts below remain the CLI fallback for CI, SSH, recovery, and debugging.

`./scripts/setup.sh` reads `/etc/os-release` and runs the helper for your
distribution: `scripts/setup/ubuntu.sh` for the Ubuntu family, or
`scripts/setup/fedora.sh` for Fedora. The numbered steps below are the Ubuntu
flow. Fedora users read the [Fedora 44](#fedora-44) section first, then follow
the same numbered steps for the optional parts.

## GUI installer wizard

Release downloads may include:

- `StudioCast-Installer-<version>-<arch>.AppImage`: GUI installer bundle when
  AppImage packaging tools succeeded in CI.
- `StudioCast-Installer-<version>-<arch>.AppDir.tar.gz`: staged AppDir archive
  for inspection or fallback testing.
- `StudioCast-<version>-source.tar.gz`: the same source archive bundled in the
  installer and also uploaded separately for inspection or manual use.
- `StudioCast-Installer-<version>-<arch>.sha256`: checksums for the release
  artifacts.

Run the GUI as your normal user, not with `sudo`. If using the AppImage, make it
executable and launch it:

```bash
chmod +x StudioCast-Installer-<version>-<arch>.AppImage
./StudioCast-Installer-<version>-<arch>.AppImage
```

The packaged installer includes its backend at
`usr/share/studiocast/installer/studiocast-installer-backend`, which matches the
GUI lookup path. It also includes the matching release source archive at
`usr/share/studiocast/source/StudioCast-<version>-source.tar.gz`; the GUI
automatically selects "Build from selected release archive" with that archive
prefilled. You can still choose a different release archive manually or build
from a local checkout.

The release AppImage/AppDir is self-contained for the installer GUI, backend,
and matching source archive. Runtime dependencies such as Qt build packages,
v4l2loopback support, PulseAudio tools, ONNX Runtime, and optional model/SDK
assets are installed or checked through supported system packages and the
backend scripts.

Build and run the installer from a checkout:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target studiocast-installer
./build/studiocast-installer
```

The GUI calls the scriptable
backend at `installer/backend/studiocast-installer-backend`, which uses the
existing setup/install/uninstall helpers for privileged package/module steps,
builds, binary links, and the systemd user service.

CLI equivalents:

```bash
./scripts/installer.sh detect-os
./scripts/installer.sh status
./scripts/installer.sh plan install
./scripts/installer.sh install --yes
./scripts/installer.sh update --source-dir /path/to/studiocast-release --yes
./scripts/installer.sh repair --yes
./scripts/installer.sh uninstall --yes
./scripts/installer.sh clean-install --yes
```

Supported installer OS bases:

- Ubuntu 22.04 / Jammy
- Ubuntu 24.04 / Noble
- Linux Mint when `/etc/os-release` exposes a reliable Ubuntu base:
  `UBUNTU_CODENAME=jammy` maps to Ubuntu 22.04 and
  `UBUNTU_CODENAME=noble` maps to Ubuntu 24.04. Mint 21.x and 22.x are mapped
  to those bases if `UBUNTU_CODENAME` is missing.
- Other Ubuntu-family distributions may also pass installer checks when
  `/etc/os-release` declares `ID_LIKE=ubuntu` and exposes
  `UBUNTU_CODENAME=jammy` or `UBUNTU_CODENAME=noble`.

Unsupported distros fail clearly with the detected `/etc/os-release` fields and
should use the manual source-build flow below. The GUI installer wizard stays
Ubuntu-only. On Fedora, use the CLI helper or the RPM package described in the
[Fedora 44](#fedora-44) section.

The installer writes:

```text
~/.local/share/studiocast/install-manifest.json
```

The manifest records installed version, source/build/install paths, installed
binaries, user service path/status, dependency setup method, install timestamp,
and whether user config/model/log/cache data was preserved. Update, repair,
uninstall, and clean-install workflows use this manifest when available.

Clean install removes app files and the user service before reinstalling. It
preserves user config, downloaded model packs, logs, and cache by default; the
GUI and backend require an explicit `--remove-user-data` choice before deleting
those XDG directories.

## Fedora 44

On Fedora, `./scripts/setup.sh` runs `scripts/setup/fedora.sh`. That helper
installs the dependencies with `dnf` and configures the virtual camera:

```bash
./scripts/setup.sh --deps --v4l2loopback --load-loopback --persist-loopback
```

Add `-y` to answer yes to the `dnf` prompts. Run
`./scripts/setup/fedora.sh --help` for the full option list. The options have
the same names as the Ubuntu helper, so `--build`, `--build-dir`,
`--build-type`, `--video-nr`, `--label`, and `--exclusive-caps` work the same
way.

### v4l2loopback needs RPM Fusion Free

Fedora does not ship `v4l2loopback`. RPM Fusion Free ships it as
`akmod-v4l2loopback`. The setup helper does not add third-party repositories
for you. Enable RPM Fusion Free first, then run the setup again:

```bash
sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm
./scripts/setup.sh --v4l2loopback --load-loopback --persist-loopback
```

Without that repository, `--v4l2loopback` and `--load-loopback` stop with exit
code 2 and print the command above. The helper then installs
`akmod-v4l2loopback` and the matching `kernel-devel`, and runs `akmods` so the
module builds for the running kernel.

### ONNX Runtime is the distro package, CPU only

Fedora ships `onnxruntime-devel`, and CMake finds it through its CMake config
file. There is no tarball download and no `pkg-config` shim, so the
`--onnxruntime-version`, `--onnxruntime-flavor`, and `--onnxruntime-arch`
options are accepted but do nothing on Fedora. The helper prints a note when
you pass them.

The Fedora package has the CPU execution provider only. The Open CUDA backend
needs the CUDA execution provider, so GPU inference needs an upstream ONNX
Runtime GPU build installed by hand. See `docs/open_cuda_install.md`.

### dlib is not packaged

Fedora has no `dlib-devel` package. CMake reports the missing dependency with a
STATUS message and disables the Open Video Eye Contact effect. The rest of the
build is not affected.

### Build a Fedora RPM

You can build and install a package instead of running from a build directory:

```bash
packaging/rpm/build_rpm.sh
sudo dnf install ./dist/rpm/studiocast-<version>-1.fc44.x86_64.rpm
systemctl --user enable --now studiocastd.service
```

`./scripts/setup.sh --rpm -- <args>` is a shortcut that runs
`packaging/rpm/build_rpm.sh` with the arguments after `--`. Use
`packaging/rpm/build_rpm.sh --help` for the build options, such as
`--container` to build in a Fedora container on a non-Fedora host.

## 1) Install dependencies + v4l2loopback

```bash
./scripts/setup.sh --deps --v4l2loopback --load-loopback --persist-loopback
```

This installs build dependencies (Qt6/CMake/Ninja/etc), ONNX Runtime (required; GPU build by default), and runtime dependencies (v4l2loopback tools, v4l-utils),
then creates a virtual camera device (by default at `/dev/video10` with label **StudioCast Camera**).
The setup helper prefers a kernel-provided/prebuilt v4l2loopback module and only falls back to `v4l2loopback-dkms` when the running kernel does not already provide one.

MJPEG decode uses **libjpeg-turbo** (via CMake `FindJPEG`). If you are installing dependencies manually:

```bash
sudo apt install libjpeg-turbo8 libjpeg-turbo8-dev
```

Verify:

```bash
v4l2-ctl --list-devices
```

### v4l2loopback module install fallback

Ubuntu kernels may already ship a signed/prebuilt `v4l2loopback.ko`. StudioCast
checks that first and avoids DKMS when it is available. If the helper falls back
to `v4l2loopback-dkms` and that DKMS build fails against a newer kernel, install
v4l2loopback from upstream source, then rerun the StudioCast setup with
`--load-loopback --persist-loopback`:

```bash
git clone https://github.com/v4l2loopback/v4l2loopback.git /tmp/v4l2loopback
cd /tmp/v4l2loopback
VERSION=$(grep 'PACKAGE_VERSION' dkms.conf | cut -d'"' -f2)
sudo mkdir -p /usr/src/v4l2loopback-${VERSION}
sudo cp -r ./* /usr/src/v4l2loopback-${VERSION}/
sudo dkms add v4l2loopback/${VERSION}
sudo dkms autoinstall
```

If you have the Ubuntu DKMS package installed and want to keep apt from
replacing the upstream source copy, hold it:

```bash
sudo apt-mark hold v4l2loopback-dkms
```

## 2) Build StudioCast

```bash
./scripts/setup.sh --build --build-type Debug
```

Or manually:

```bash
cmake -S . -B cmake-build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug
```

## 3) Optional: Open CUDA backend (no Maxine required)

The Open CUDA backend (`open_cuda`) runs GPU-only virtual background effects via ONNX Runtime (CUDA EP), using
user-supplied model packs.

Install/usage docs:

- `docs/open_cuda_install.md`

On Ubuntu 22.04+, the helper script that installs an ONNX Runtime GPU build is:

```bash
./scripts/setup.sh --deps
```

`--deps` ensures ONNX Runtime is available via `pkg-config onnxruntime` by installing the upstream tarball under
`/opt/studiocast/onnxruntime/<version>/...` and configuring runtime linking via `ldconfig`.

## 3b) Optional: Open Audio backend (no Maxine required)

The Open Audio backend (`open_audio`) runs **microphone** effects (noise removal + “studio voice”-style enhancement)
via ONNX Runtime, using user-supplied model packs.

Install/usage docs:

- `docs/open_source_audio_models_install.md`

Install the curated model pack:

```bash
./scripts/install.sh open-audio-models
```

Verify:

```bash
./cmake-build-debug/studiocast-open audio-list-models
./cmake-build-debug/studiocast-open audio-self-test --model-id fastenhancer_m_vd_v1
```

## 4) Optional: Install NVIDIA Maxine SDK + features

StudioCast does **not** ship or redistribute NVIDIA Maxine SDK assets. You must obtain them yourself
from NVIDIA and comply with NVIDIA's license terms.

Use the helper tool to print authoritative paths and install commands:

```bash
./cmake-build-debug/studiocast-maxine init
./cmake-build-debug/studiocast-maxine install-hints
```

Current Linux Maxine SDK builds typically expose `libVideoFX.so` for VFX and
`libnvARPose.so` for AR. StudioCast now auto-detects those names directly, but
you still need to run the SDK-provided `install_feature.sh` steps so the effect
models and feature libraries are installed.

### Optional: automate extraction + feature install

If you already downloaded the SDK tarballs, you can extract them into the expected layout and install
features (models/libs) via NGC:

```bash
export NGC_CLI_API_KEY="..."   # do not commit this
export NGC_API_KEY="..."       # do not commit this
./scripts/setup/maxine.sh \
  --vfx-tar ~/Downloads/NVIDIA_VFX_SDK_linux_*.tar.gz \
  --ar-tar  ~/Downloads/NVIDIA_AR_SDK_linux_*.tar.gz \
  --afx-tar ~/Downloads/Audio_Effects_SDK.tar.gz \
  --install-features --install-afx-features --build-dir ./cmake-build-debug
```

By default, `--install-afx-features` downloads the MVP AFX feature set (AEC + Superres). To customize:

```bash
./scripts/setup/maxine.sh --install-afx-features --afx-effects "superres-16k_to_48k,superres-8k_to_16k,aec-16k,aec-48k"
```

This runs the SDK-provided `install_feature.sh` scripts under the hood.

For AFX features, the helper uses the SDK-provided `download_features.sh` script and requires `NGC_API_KEY`.

## 5) Run daemon + use in OBS

```bash
./cmake-build-debug/studiocastd
./cmake-build-debug/studiocastctl status
```

In OBS: select **StudioCast Camera** as a camera source.

## Support bundle

If something doesn't work:

```bash
./cmake-build-debug/studiocastctl debug-report --out studiocast-debug-report.txt
```

Attach that file in GitHub issues.
