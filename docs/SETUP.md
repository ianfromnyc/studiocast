# Setup / Install (Ubuntu 22.04+)

This repo is a **source build** project today (POC/MVP). The scripts below exist to make it easy for
testers and contributors to install prerequisites without guessing.

## 1) Install dependencies + v4l2loopback

```bash
./scripts/setup_ubuntu22.sh --deps --v4l2loopback --load-loopback --persist-loopback
```

This installs build dependencies (Qt6/CMake/Ninja/etc) and runtime dependencies (v4l2loopback DKMS, v4l-utils),
then creates a virtual camera device (by default at `/dev/video10` with label **StudioCast Camera**).

Verify:

```bash
v4l2-ctl --list-devices
```

## 2) Build StudioCast

```bash
./scripts/setup_ubuntu22.sh --build --build-type Release
```

Or manually:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## 3) Install NVIDIA Maxine SDK + features (required for effects)

StudioCast does **not** ship or redistribute NVIDIA Maxine SDK assets. You must obtain them yourself
from NVIDIA and comply with NVIDIA's license terms.

Use the helper tool to print authoritative paths and install commands:

```bash
./build/studiocast-maxine init
./build/studiocast-maxine install-hints
```

### Optional: automate extraction + feature install

If you already downloaded the SDK tarballs, you can extract them into the expected layout and install
features (models/libs) via NGC:

```bash
export NGC_CLI_API_KEY="..."   # do not commit this
export NGC_API_KEY="..."       # do not commit this
./scripts/setup_maxine.sh \
  --vfx-tar ~/Downloads/NVIDIA_VFX_SDK_linux_*.tar.gz \
  --ar-tar  ~/Downloads/NVIDIA_AR_SDK_linux_*.tar.gz \
  --afx-tar ~/Downloads/Audio_Effects_SDK.tar.gz \
  --install-features --install-afx-features --build-dir ./build
```

By default, `--install-afx-features` downloads the MVP AFX feature set (AEC + Superres). To customize:

```bash
./scripts/setup_maxine.sh --install-afx-features --afx-effects "superres-16k_to_48k,superres-8k_to_16k,aec-16k,aec-48k"
```

This runs the SDK-provided `install_feature.sh` scripts under the hood.

For AFX features, the helper uses the SDK-provided `download_features.sh` script and requires `NGC_API_KEY`.

## 4) Run daemon + use in OBS

```bash
./build/studiocastd
./build/studiocastctl status
```

In OBS: select **StudioCast Camera** as a camera source.

## Support bundle

If something doesn't work:

```bash
./build/studiocastctl debug-report --out studiocast-debug-report.txt
```

Attach that file in GitHub issues.