# StudioCast open_video model pack templates

This directory contains **metadata-only templates** for open-source video model packs.

StudioCast does **not** commit model binaries into git.

Installed model packs live at:

- `${XDG_DATA_HOME:-$HOME/.local/share}/studiocast/models/open_video/`

## Subject areas

- `segmentation/` — matte/alpha models (`task = "matting"`)
- `face_detection/` — face detection models
- `face_landmarks/` — facial landmark detectors
- `video_denoise/` — video denoising models
- `eye_contact/` — gaze/eye contact models

## What’s in a pack?

A pack is a directory containing:

- `model.json` — pack metadata (see `docs/open_video_model_packs.md`)
- `LICENSE.txt` — upstream licensing summary
- model artifacts (downloaded at install time), e.g.
  - `*.onnx`
  - `shape_predictor_68_face_landmarks.dat`
