# Installing Maxine SDK + features

StudioCast does **not** ship or redistribute NVIDIA Maxine SDK assets (SDK tarballs, binaries, models, feature packs, or NGC keys).
You must obtain them yourself from NVIDIA and comply with NVIDIA's licensing terms.

`studiocast-maxine` prints the **authoritative** install commands for your machine.

Build and run:

```bash
cmake --build <build-dir> --target studiocast-maxine
./<build-dir>/studiocast-maxine install-hints
```

The tool prints the exact paths it expects (based on XDG dirs), plus copy/paste commands.

## Expected on-disk layout (defaults)

By default the base directory is:

* `$XDG_DATA_HOME/studiocast/maxine` (typically `~/.local/share/studiocast/maxine`)

And the canonical SDK roots are:

* VFX: `.../maxine/VideoFX`
* AR: `.../maxine/ARSDK`
* AFX: `.../maxine/Audio_Effects_SDK`

`studiocast-maxine install-hints` prints these as absolute paths for your user.

Recent Linux Maxine SDK releases typically ship the core shared libraries as
`libVideoFX.so` (VFX) and `libnvARPose.so` (AR). StudioCast auto-detects those
current names, while still accepting older aliases such as `libnvvfx.so`,
`libNvVFX.so`, `libnvar.so`, and `libNvAR.so`.

The core SDK tarballs are not sufficient by themselves to run effects. You also
need to run the SDK-provided `install_feature.sh` scripts so that the required
feature libraries and models are present under each SDK root.

## Commands (as printed by `install-hints`)

### VFX core

Extract so that `.../maxine/VideoFX` exists:

```bash
mkdir -p "<Maxine base>"
tar -xvf NVIDIA_VFX_SDK_linux_<version>.tar.gz -C "<Maxine base>"
```

### AR core

Extract so that `.../maxine/ARSDK` exists:

```bash
mkdir -p "<Maxine base>"
tar -xvf NVIDIA_AR_SDK_linux_<version>.tar.gz -C "<Maxine base>"
```

### AFX core

Create `.../maxine/Audio_Effects_SDK`:

```bash
mkdir -p "<Maxine base>"
cd "<Maxine base>"
tar xvf --one-top-level Audio_Effects_SDK.tar.gz
```

### VFX/AR feature install

On a supported Tensor Core GPU machine (Turing+), `install-hints` prints the `--gpu` values and the exact
commands to run once per unique `--gpu` value:

```bash
export NGC_CLI_API_KEY="<your_api_key>"
cd "<VFX root>/features" && ./install_feature.sh --gpu <gpu> --feature all --ngc-org nvidia --ngc-team maxine
cd "<AR root>/features" && ./install_feature.sh --gpu <gpu> --feature all --ngc-org nvidia --ngc-team maxine
```

If no supported GPU mapping is detected, the tool prints a message explaining that feature installation
must be run on a Tensor Core GPU machine.

### AFX features

```bash
export NGC_API_KEY="<your_api_key>"
cd "<AFX root>/features" && ./download_features.sh --effects superres-16k_to_48k,superres-8k_to_16k,aec-16k,aec-48k
```

This downloads only the **minimal** AFX effect set needed for the MVP (AEC + Superres).

Optional (noise removal / room echo / studio voice):

```bash
cd "<AFX root>/features" && ./download_features.sh --effects denoiser-48k,dereverb-48k,dereverb_denoiser-48k,studio_voice-48k
```

After installing, `studiocastctl status` / `studiocastd status` (and the GUI) should report that AFX/audio effects are ready.
