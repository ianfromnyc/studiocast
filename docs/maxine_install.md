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

| Component | NGC resource (default) | Entitlement |
| --- | --- | --- |
| Audio Effects (AFX) | `maxine_linux_audio_effects_sdk` | NVIDIA Developer Program (free account) |
| Video Effects (VFX) | `maxine_linux_vfx_sdk_ga` | NVIDIA AI Enterprise subscription |
| AR SDK (AR) | `maxine_linux_ar_sdk_ga` | NVIDIA AI Enterprise subscription |

Other names hold the same components on another tier:

* `maxine_linux_vfx_sdk_ea`, `maxine_linux_ar_sdk_ea` — Maxine Early Access. Request access on the catalog page.
* `maxine_linux_vfx_sdk`, `maxine_linux_ar_sdk` — earlier releases.

Pick one with `--vfx-resource NAME` or `--ar-resource NAME`.

Catalog page for any of them:

```
https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/resources/<name>
```

If your account has no entitlement, the helper stops with a clear message and downloads nothing:

```
[maxine] ERROR: this NGC account has no entitlement for 'maxine_linux_vfx_sdk_ga' (402 Payment Required).
[maxine] ERROR: See https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/resources/maxine_linux_vfx_sdk_ga and request access there.
[maxine] ERROR: Alternate names for this component:
[maxine] ERROR:   maxine_linux_vfx_sdk_ea
[maxine] ERROR:   maxine_linux_vfx_sdk
```

The exit code is 2. Audio only installs are therefore possible with a free account:

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

The core SDK archives are large (AFX about 1.4 GB, VFX about 2.6 GB, AR about 2.8 GB). Keep the cache if you plan to reinstall; delete the version directory if you do not.

## 5. Choosing a version

List what NGC holds:

```bash
./scripts/setup/maxine.sh --list-versions afx
```

```
[maxine] Versions of maxine_linux_audio_effects_sdk (NGC org nvidia, team maxine):
  2.1.0            UPLOAD_COMPLETE    1.4 GiB
  2.0.0            UPLOAD_COMPLETE    1.4 GiB
  1.7.0            UPLOAD_COMPLETE    1.5 GiB
```

Without a pin the helper takes the newest finished version. Pin one with:

```bash
./scripts/setup/maxine.sh --download afx --sdk-version afx=2.1.0
```

`--afx-version`, `--vfx-version` and `--ar-version` are short forms of the same thing.

## 6. Feature packs

The core SDK alone runs nothing. Each effect needs a feature pack: a feature library plus the models built for your GPU architecture.

```bash
./scripts/setup/maxine.sh --install-features        # VFX and AR, through install_feature.sh
./scripts/setup/maxine.sh --install-afx-features    # AFX, through download_features.sh
./scripts/setup/maxine.sh --download-features all   # both, with a fallback (see below)
```

`--install-features` and `--install-afx-features` run the scripts that NVIDIA ships inside each SDK. Those scripts need only `curl` or `wget` and the API key, so no `ngc` command line tool is needed.

`--download-features` is the same step with one addition: when the core SDK is not extracted, it fetches the same NGC feature packs itself over the REST API and writes the same layout. That is useful on a machine without an NVIDIA AI Enterprise entitlement, because the VFX and AR **feature packs** need only a Developer Program account even though the **core SDK** does not.

> A feature pack alone cannot run an effect. Its library links against core SDK libraries such as `libVideoFXLocal.so`, `libNVCVImage.so` and `libnvARPoseLocal.so`, which ship only in the core SDK archives. Fetch the feature packs without the core SDK for inspection or for a cache, not to run effects.

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

The REST fallback needs the compute capability of your card. It reads it from the SDK helper binary or from `nvidia-smi`. Give it yourself with `--sm`, for example `--sm 86` for an Ampere GeForce card:

```bash
./scripts/setup/maxine.sh --download-features ar --sm 86
```

Select a subset of the VFX or AR feature packs with `--vfx-features` or `--ar-features`. A pack that another pack needs is added by itself:

```bash
./scripts/setup/maxine.sh --download-features ar --ar-features nvarlandmarkdetection
```

## 7. Offline install

If you already have the archives, or the machine has no network, extract them instead:

```bash
./scripts/setup/maxine.sh \
  --vfx-tar ~/Downloads/NVIDIA_VFX_SDK_linux_<version>.tar.gz \
  --ar-tar  ~/Downloads/NVIDIA_AR_SDK_linux_<version>.tar.gz \
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

After an AFX install, `doctor` reports the AFX root, the `libnv_audiofx.so` library, and the features directory as present. After a VFX and AR install, it reports `libVideoFX.so` and `libnvARPose.so` with their models and features directories. `install-hints` prints the paths it expects for your user, and `./<build-dir>/studiocastctl status` shows whether the daemon can use the effects.

Recent Linux Maxine releases name the core libraries `libVideoFX.so` (VFX) and `libnvARPose.so` (AR). StudioCast finds those names, and still accepts older ones such as `libnvvfx.so`, `libNvVFX.so`, `libnvar.so` and `libNvAR.so`.

## 9. Result of a feature install

For audio, one directory per effect under the AFX features directory:

```
Audio_Effects_SDK/features/aec/include/aec.h
Audio_Effects_SDK/features/aec/lib/libnv_audiofx_aec.so.2.1.0
Audio_Effects_SDK/features/aec/models/sm_86/aec_16k_4096.trtpkg
Audio_Effects_SDK/features/aec/models/sm_86/aec_16k.trtpkg -> aec_16k_4096.trtpkg
```

For video, one directory per feature pack under the VFX or AR features directory, with the same shape.

## Troubleshooting

* `401` — NGC rejected the key. Generate a new one and export it again.
* `402` — the account has no entitlement for that resource. See the table above.
* `403` — there is no such name in the org and team, or the key cannot read it. Check the spelling of `--vfx-resource`, `--ar-resource` or `--afx-resource`.
* `404` — no such version. Run `--list-versions` first.
* A download that stops can be started again with the same command. It continues where it stopped, and it checks the SHA-256 before it extracts anything.
