# Installing the Maxine SDK and its features

StudioCast does **not** ship or redistribute NVIDIA Maxine SDK assets (SDK archives, binaries, models, feature packs, or NGC keys).
You get them from NVIDIA with your own NGC account, and you must obey NVIDIA's license terms.

One NGC API key does the whole install. The helper script asks NGC what it holds, downloads it, and puts it where StudioCast looks for it.

## 1. Get an NGC API key

1. Sign in at <https://ngc.nvidia.com>.
2. Open **Setup -> API key** and generate a personal key. A modern key starts with `nvapi-`.
3. Export it in your shell. Do not commit it and do not put it in a file that you push:

```bash
export NGC_API_KEY="<your key>"
```

`NGC_CLI_API_KEY` works too. The helper accepts either name and gives both names to the NVIDIA scripts that it calls, so you set only one.

## 2. Install

```bash
export NGC_API_KEY="<your key>"
./scripts/setup.sh --maxine -- --download all --install-features --install-afx-features
```

You can also call the helper directly:

```bash
./scripts/setup/maxine.sh --download all --install-features --install-afx-features
```

What the command does:

1. Asks NGC for the newest version of each core SDK.
2. Downloads each archive into the cache directory and checks its SHA-256.
3. Extracts each SDK into the Maxine base directory.
4. Runs the SDK feature scripts, so the models and the feature libraries are installed.

Every step runs as your own user. No step needs `sudo`, because the whole layout is under your XDG directories.

Add `--dry-run` to see what would happen. A dry run still asks NGC what it holds, but it writes nothing.

## 3. What your NGC account must have

The three cores and all their feature packs are available to an **NVIDIA Developer Program** account, which is free:

| Component | NGC resource (default) | Entitlement |
| --- | --- | --- |
| Video Effects (VFX) | `vfx_sdk_core` | NVIDIA Developer Program |
| AR SDK (AR) | `ar_sdk_core` | NVIDIA Developer Program |
| Audio Effects (AFX) | `maxine_linux_audio_effects_sdk` | NVIDIA Developer Program |

Other names hold the same SDKs in another packaging:

* `maxine_linux_vfx_sdk_ga`, `maxine_linux_ar_sdk_ga` — the NVIDIA AI Enterprise packaging of the same SDKs. They need that subscription.
* `maxine_linux_vfx_sdk_ea`, `maxine_linux_ar_sdk_ea` — Maxine Early Access. Request access on the catalog page.
* `maxine_linux_vfx_sdk`, `maxine_linux_ar_sdk` — earlier releases.

Pick one with `--vfx-resource NAME` or `--ar-resource NAME`.

Catalog page for any of them:

```
https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/resources/<name>
```

If your account has no entitlement for the name you asked for, the helper stops with a clear message and downloads nothing:

```
[maxine] ERROR: this NGC account has no entitlement for 'maxine_linux_vfx_sdk_ga' (402 Payment Required).
[maxine] ERROR: See https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/resources/maxine_linux_vfx_sdk_ga and request access there.
[maxine] ERROR: Alternate names for this component:
[maxine] ERROR:   vfx_sdk_core
[maxine] ERROR:   maxine_linux_vfx_sdk_ea
[maxine] ERROR:   maxine_linux_vfx_sdk
```

The exit code is 2. An audio only install is one command:

```bash
export NGC_API_KEY="<your key>"
./scripts/setup/maxine.sh --download afx --install-afx-features
```

## 4. Where the files go

Downloads are cached in:

* `$XDG_CACHE_HOME/studiocast/maxine/<resource>/<version>/` (usually `~/.cache/studiocast/maxine/...`)

The SDKs are installed in:

* `$XDG_DATA_HOME/studiocast/maxine` (usually `~/.local/share/studiocast/maxine`), with the roots
  * VFX: `.../maxine/VideoFX`
  * AR: `.../maxine/ARSDK`
  * AFX: `.../maxine/Audio_Effects_SDK`

Change either one with `--cache-dir DIR` and `--base DIR`.

The cache makes a second run cheap. A file that is already there with the right SHA-256 is not fetched again, and a part file from a stopped download is resumed:

```
[maxine] NVIDIA_AFX_SDK_Linux_2.1.0.10.tar.gz: already downloaded, sha256 verified.
```

The core SDK archives are large (AFX about 1.4 GiB, VFX about 2.6 GiB, AR about 2.7 GiB). Keep the cache if you plan to reinstall; delete the version directory if you do not.

A VFX or AR version holds more than one file. All of them are cached, but only the core SDK archive is extracted:

```
~/.cache/studiocast/maxine/vfx_sdk_core/1.2.0.0_linux/
  VFXSDK_linux_1.2.0.0.tgz      <- the core SDK, extracted into VideoFX
  VFXSDK_triton_1.2.0.0.tgz     <- the Triton Inference Server build, kept in the cache
  README_VFX_SDK_Triton.md
```

The Triton archive holds a `VideoFX-triton-server` model repository for NVIDIA Triton Inference Server. StudioCast does not use it, so the helper never unpacks it into the SDK root.

## 5. Choosing a version

List what NGC holds:

```bash
./scripts/setup/maxine.sh --list-versions afx
```

```
[maxine] Versions of maxine_linux_audio_effects_sdk (NGC org nvidia, team maxine):
  VERSION              PLATFORM  STATUS             SIZE
  2.1.0                -         UPLOAD_COMPLETE    1.4 GiB
  2.0.0                -         UPLOAD_COMPLETE    1.4 GiB
  1.7.0                -         UPLOAD_COMPLETE    1.5 GiB
```

The VFX and AR cores hold one version per platform:

```
[maxine] Versions of vfx_sdk_core (NGC org nvidia, team maxine):
  VERSION              PLATFORM  STATUS             SIZE
  1.2.0.0_windows      windows   UPLOAD_COMPLETE    1014.6 MiB
  1.2.0.0_linux        linux     UPLOAD_COMPLETE    2.6 GiB
  1.1.0.0_windows      windows   UPLOAD_COMPLETE    1014.5 MiB
  1.1.0.0_linux        linux     UPLOAD_COMPLETE    2.6 GiB
```

Without a pin the helper takes the newest finished version **of your platform**, which is Linux. It does not use the "latest version" that NGC reports, because for these resources that is the Windows build. Use `--platform windows` to look at the other side.

Pin a version with:

```bash
./scripts/setup/maxine.sh --download vfx --sdk-version vfx=1.2.0.0_linux
```

`--afx-version`, `--vfx-version` and `--ar-version` are short forms of the same thing.

## 6. Feature packs

The core SDK alone runs nothing. Each effect needs a feature pack: a feature library plus the models built for your GPU architecture.

```bash
./scripts/setup/maxine.sh --install-features        # VFX and AR, through install_feature.sh
./scripts/setup/maxine.sh --install-afx-features    # AFX, through download_features.sh
./scripts/setup/maxine.sh --download-features all   # both, with a fallback (see below)
```

All three commands prefer the scripts that NVIDIA ships inside each SDK. Those scripts need only `curl` or `wget` and the API key, so no `ngc` command line tool is needed. When the core SDK is not extracted, `--download-features` fetches the same NGC packs itself over the REST API and writes the same layout.

A feature install puts:

* the feature library in `<SDK root>/features/<feature>/{include,lib}`
* the engine files in `<SDK root>/lib/models` for VFX and AR, and in `<SDK root>/features/<effect>/models/sm_<CC>` for AFX

> A feature pack alone cannot run an effect. Its library links against core SDK libraries such as `libVideoFXLocal.so`, `libNVCVImage.so` and `libnvARPoseLocal.so`, which ship only in the core SDK archives. Fetch the feature packs without the core SDK for inspection or for a cache, not to run effects.

### The VFX and AR feature script needs one workaround

`features/install_feature.sh` of SDK 1.x starts with a fixed path:

```bash
VFXSDK_PATH="/usr/local/VideoFX"     # ARSDK_PATH="/usr/local/ARSDK" in the AR SDK
```

It stops at once when that directory is missing, and it has no option and no variable to change it. StudioCast installs under your XDG data directory instead, so the helper writes a copy of the script beside the original with that one line changed, runs the copy, and deletes it. The copy has to stay in the `features` directory, because the script looks for its `compute_capability` helper beside itself. You see this in the log:

```
[maxine] VFX: install_feature.sh installs only into /usr/local/VideoFX, which is not this install.
[maxine] VFX: running a copy of it that points at /home/<user>/.local/share/studiocast/maxine/VideoFX.
```

Nothing is written outside your own directories, and no step needs root.

### Audio effects

The default effect list is the minimal set for the StudioCast MVP: acoustic echo cancellation and super resolution.

```bash
./scripts/setup/maxine.sh --install-afx-features
```

Choose another set:

```bash
./scripts/setup/maxine.sh --install-afx-features \
  --afx-effects "denoiser-48k,dereverb-48k,dereverb_denoiser-48k,studio_voice-48k"
```

The AFX script reads the compute capability of GPU 0 by itself. Name the GPU with `--afx-gpu` (for example `a40`, `t4`, `l4`) when you install on a machine whose GPU is not the target, and set `CUDA_VISIBLE_DEVICES` when the machine has more than one GPU.

### GPU architecture

Both the SDK scripts and the REST fallback read the compute capability of your card, from the `compute_capability` helper that the SDK ships or from `nvidia-smi`. `--gpu` names a GPU for `install_feature.sh` (it knows datacenter names such as `a40`, `t4`, `l4`, `h100`); a name it does not know is dropped, and it then detects the GPU itself.

The REST fallback takes the architecture number with `--sm`, for example `--sm 86` for an Ampere GeForce card:

```bash
./scripts/setup/maxine.sh --download-features ar --sm 86
```

### Which features are installed

By default the helper names the features that NVIDIA builds for Linux in SDK 1.x:

* VFX: `nvvfxaigsrelighting`, `nvvfxbackgroundblur`, `nvvfxdenoising`, `nvvfxgreenscreen`, `nvvfxrelighting`, `nvvfxtransfer`, `nvvfxupscale`, `nvvfxvideosuperres`
* AR: `nvaractivespeakerdetection`, `nvarbodydetection`, `nvarbodyposeestimation`, `nvarfaceboxdetection`, `nvarfaceexpressions`, `nvargazeredirection`, `nvarlandmarkdetection`

Select a subset with `--vfx-features` or `--ar-features`. A pack that another pack needs is added by itself:

```bash
./scripts/setup/maxine.sh --download-features ar --ar-features nvarlandmarkdetection
```

Pass `all` to let the SDK script ask NGC for the whole list. That list also names features that NVIDIA builds for Windows only, such as `nvarlipsync`. The SDK script ends with an error for such a feature even when every other one installed, so the helper reads the per-feature result lines instead of the exit code alone:

```
[maxine] AR: NGC has no Linux pack for: nvarlipsync
[maxine] AR: those features are built for Windows only, or they are not in this SDK.
[maxine] AR: 7 feature(s) installed.
```

The install fails only when no feature at all was installed, or when a feature that you named yourself could not be installed.

## 7. Offline install

If you already have the archives, or the machine has no network, extract them instead:

```bash
./scripts/setup/maxine.sh \
  --vfx-tar ~/Downloads/VFXSDK_linux_1.2.0.0.tgz \
  --ar-tar  ~/Downloads/ARSDK_linux_1.1.1.0.tgz \
  --afx-tar ~/Downloads/NVIDIA_AFX_SDK_Linux_<version>.tar.gz
```

The helper reads the archive first and puts the SDK root in the right place, whether the archive holds a top level `VideoFX`, `ARSDK` or `Audio_Effects_SDK` directory, a versioned directory, or a `usr/local` prefix. If it cannot find the root, it prints the top of the archive and tells you what to do.

You can also seed the cache by hand and let the download step verify it. Put the file at
`$XDG_CACHE_HOME/studiocast/maxine/<resource>/<version>/<file name>` and run the same `--download` command.

## 8. Verify

```bash
cmake --build <build-dir> --target studiocast-maxine
./<build-dir>/studiocast-maxine doctor
```

After an AFX install, `doctor` reports the AFX root, the `libnv_audiofx.so` library, and the features directory as present. After a VFX and AR install, it reports `libVideoFX.so` and `libnvARPose.so` with their features directories. `install-hints` prints the paths it expects for your user, and `./<build-dir>/studiocastctl status` shows whether the daemon can use the effects.

Recent Linux Maxine releases name the core libraries `libVideoFX.so` (VFX) and `libnvARPose.so` (AR). StudioCast finds those names, and still accepts older ones such as `libnvvfx.so`, `libNvVFX.so`, `libnvar.so` and `libNvAR.so`.

> `doctor` looks for a `models` directory at the top of each SDK root. SDK 1.x installs the VFX and AR engine files in `<SDK root>/lib/models` instead, so `doctor` can report that directory as missing while the effects are in fact installed.

## 9. Result of an install

The VFX core, as it comes out of the archive:

```
VideoFX/Changelog.txt
VideoFX/README.md
VideoFX/external/{cuda,tensorrt}     <- the CUDA runtime and TensorRT that the SDK needs
VideoFX/features/{install_feature.sh,compute_capability,README.md}
VideoFX/include/
VideoFX/lib/libVideoFX.so ... libNVCVImage.so, libnvngxruntime.so, models/
VideoFX/share/
```

The AR core has the same shape, with `libnvARPose.so` in `lib`.

A VFX or AR feature install adds:

```
VideoFX/features/nvvfxgreenscreen/{include,lib}
VideoFX/lib/models/<engine files>
```

An audio feature install makes one directory per effect:

```
Audio_Effects_SDK/features/aec/include/aec.h
Audio_Effects_SDK/features/aec/lib/libnv_audiofx_aec.so.2.1.0
Audio_Effects_SDK/features/aec/models/sm_86/aec_16k_4096.trtpkg
Audio_Effects_SDK/features/aec/models/sm_86/aec_16k.trtpkg -> aec_16k_4096.trtpkg
```

## Troubleshooting

* `401` — NGC rejected the key. Generate a new one and export it again.
* `402` — the account has no entitlement for that resource. Use the Developer Program names in the table above (`vfx_sdk_core`, `ar_sdk_core`).
* `403` — there is no such name in the org and team, or the key cannot read it. Check the spelling of `--vfx-resource`, `--ar-resource` or `--afx-resource`.
* `404` — no such version. Run `--list-versions` first.
* A download that stops can be started again with the same command. It continues where it stopped, and it checks the SHA-256 before it extracts anything.
