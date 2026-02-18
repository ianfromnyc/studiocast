# Open-source video model packs (open_video)

StudioCast does **not** ship model binaries in git.

Instead, model packs are defined as **metadata + licensing** in `./scripts/model_packs/open_video/` and are installed into:

- `${XDG_DATA_HOME:-$HOME/.local/share}/studiocast/models/open_video/`

At runtime, StudioCast discovers installed packs by scanning that directory.

## Directory taxonomy

Packs are organized by **subject area**, then by **model pack**:

```
open_video/
  segmentation/
    <pack_dir>/
      model.json
      LICENSE.txt
      model.onnx
  face_detection/
    <pack_dir>/
      model.json
      LICENSE.txt
      ...
  face_landmarks/
    <pack_dir>/
      model.json
      LICENSE.txt
      ...
  video_denoise/
    <pack_dir>/
      model.json
      LICENSE.txt
      ...
  eye_contact/
    <pack_dir>/
      model.json
      LICENSE.txt
      ...
```

Notes:

- `./scripts/model_packs/open_video/**` contains **templates only** (metadata + license notes).
- Installed packs live under `~/.local/share/studiocast/models/open_video/**`.
- Multiple quality/latency variants are represented as multiple packs within the same subject.

### Curated quality tiers

StudioCast templates typically provide **Good / Better / Best** tiers within a subject.

For example, `face_detection/` includes YuNet in three tiers:

- **Good Quality (YuNet INT8)** — fastest; slightly reduced accuracy / compatibility depends on int8 kernel support
- **Better Quality (YuNet INT8 BQ)** — blockwise-quantized int8 variant (good accuracy, still fast)
- **Best Quality (YuNet FP32)** — reference fp32 model (highest compatibility)

Similarly, `video_denoise/` includes FastDVDnet sigma preset packs:

- **Good Quality (FastDVDnet sigma15)** — light denoise / detail-preserving (best for mild noise)
- **Better Quality (FastDVDnet sigma25)** — balanced default for typical webcam noise
- **Best Quality (FastDVDnet sigma50)** — strongest denoise (best for extreme low-light noise; may soften fine detail)



And `eye_contact/` includes gaze-correction-cam packs in Good/Better/Best variants:

- **Good Quality (Gaze Correction Cam v0.1.1 FP16)** — fastest on GPU; intended for CUDA/TensorRT (CPU EP may not support FP16).
- **Better Quality (Gaze Correction Cam v0.1.1 FP32)** — reference fp32 export; best compatibility (CPU EP and GPU EP).
- **Best Quality (Gaze Correction Cam v3 Placeholder)** — placeholder for upstream “v3” model updates; installer drops in the new ONNX files when available.

Eye contact packs typically depend on `face_landmarks/` (e.g. `dlib_68_ibug_300w`) to locate and crop eye regions.

## Tasks

`model.json` uses a `task` field to declare what the model is used for.

Current/planned tasks:

- `matting` (a.k.a. segmentation matte) — Virtual Background, Auto Frame (matte-derived ROI), Virtual Key Light.
- `face_detection` — Auto Frame (face ROI), Eye Contact (face ROI).
- `face_landmarks` — Eye Contact (eye region extraction / anchors).
- `video_denoise` — Video Noise Removal.
- `eye_contact` — Eye Contact (warp/flow model).

## `model.json` schema

StudioCast currently has a stable **v1 schema** for `task = "matting"` (used by the Open CUDA backend).

For the additional video tasks above, StudioCast uses a more flexible **v2 schema**.

### Schema v1 (matting)

V1 is used by Open CUDA `task="matting"` packs.

Required fields:

- `id` (string, stable identifier)
- `display_name` (string)
- `task` = `"matting"`
- `onnx_filename` (string)
- `input` (object): `layout`, `dtype`, `width`, `height`, `channels`
- `output` (object): `name`, `kind` (currently `"alpha"`), `dtype`
- `preprocess` (object): `kind` (`"linear"`), `scale`, `mean[]`, `std[]`

Example (see `./scripts/model_packs/open_video/segmentation/**/model.json`).

### Schema v2 (multi-task)

V2 is designed for tasks that:

- have **multiple inputs/outputs**,
- require **non-ONNX artifacts** (e.g. dlib landmark predictor),
- ship as **multiple model files** (e.g. L/R eye models),
- require task-specific post-processing.

V2 fields are:

- `schema_version` = `2`
- `id`, `display_name`, `task`
- `files[]` (optional but recommended):
  - `name` (filename inside the pack directory)
  - `kind` (`"onnx"`, `"dlib_shape_predictor"`, `"mediapipe_task"`, `"data"`, ...)
  - `sha256` (optional; installer may verify)
  - `role` (optional; e.g. `"left"`, `"right"`, `"main"`)
- `onnx` (optional):
  - `main` (filename from `files[]`)
  - `inputs[]` / `outputs[]` (name/layout/dtype/shape)
  - `preprocess` / `postprocess` (task-specific)
- `origin` (optional): where the model comes from (repo, paper, hosted asset URL, etc.)

The v2 schema is intentionally permissive; the runtime validates only what the corresponding effect requires.

## Licensing

Each pack directory **must** include `LICENSE.txt` summarizing:

- upstream repository / model source
- license type
- any notable restrictions (e.g. non-commercial, attribution)

StudioCast is non-commercial, but **you are still responsible** for complying with third‑party license terms.
