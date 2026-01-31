# Maxine Setup (Local, Production-like Layout)

StudioCast expects Maxine SDK cores to be installed user-locally under:

~/.local/share/studiocast/maxine/

This follows the XDG Base Directory default for user data storage. (See XDG_DATA_HOME.)

## Expected layout

 - `~/.local/share/studiocast/maxine/VideoFX`
- `~/.local/share/studiocast/maxine/ARSDK`
- `~/.local/share/studiocast/maxine/Audio_Effects_SDK`

Overrides:
- STUDIOCAST_VFX_SDK_ROOT
- STUDIOCAST_AR_SDK_ROOT
- AFX_SDK_ROOT

StudioCast still accepts system installs at:
- `/usr/local/VideoFX`
- `/usr/local/ARSDK`

## VFX core + features (NGC)

1) Extract the VFX core SDK so that VideoFX/ exists.
2) Install features/models using `features/install_feature.sh`.

Note: VFX feature installation uses the env var `NGC_CLI_API_KEY`.

## AR core + features (NGC)

1) Extract the AR core SDK so that ARSDK/ exists.
2) Install features/models using `features/install_feature.sh`.

Note: AR feature installation uses the env var `NGC_CLI_API_KEY`.

## AFX core + features (NGC)

1) Extract the AFX core SDK. NVIDIA docs use:
   tar xvf --one-top-level Audio_Effects_SDK.tar.gz
2) Install feature libraries/models using `features/download_features.sh`.

Note: AFX feature download uses the env var `NGC_API_KEY`.

## Helper commands

Use:
- `studiocast-maxine init`
- `studiocast-maxine install-hints`
- `studiocast-probe`

Never commit API keys or downloaded SDK artifacts into the repo.
