# Phase 1: Installing Maxine SDKs locally (developer notes)

This project does not redistribute NVIDIA Maxine assets. You must obtain SDK core packages and features/models from NVIDIA NGC under NVIDIA’s terms.

## Driver requirement
Maxine Linux SDKs require NVIDIA Linux driver 570.26+.
- AR SDK docs: 570.26+ and supports Ubuntu 20.04/22.04/24.04.
- AFX SDK docs: 570.26+ (note: distro list in the doc is older, but driver requirement is explicit).

## VFX (Video Effects) SDK
Per NVIDIA docs, the VFX SDK is extracted to /usr/local/VideoFX and features are installed via features/install_feature.sh.
- Install core: extract the VFX SDK tarball to /usr/local
- Install features: use install_feature.sh with an NGC API key

## AR (Augmented Reality) SDK
Per NVIDIA docs, the AR SDK is extracted to /usr/local/ARSDK and features are installed via features/install_feature.sh.

## AFX (Audio Effects) SDK
Per NVIDIA docs, extract the AFX SDK core tarball wherever you like, then install features under the SDK’s features/ directory.
Set:
```export AFX_SDK_ROOT=/path/to/your/extracted/Audio_Effects_SDK```

## Probe tool
Run:
```./build/studiocast-probe```
or:
```./build/studiocast-probe --json```
