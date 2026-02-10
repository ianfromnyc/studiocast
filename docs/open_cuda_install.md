# Open CUDA backend (ONNX Runtime CUDA) — install & model packs

The **Open CUDA** backend (`open_cuda`) is the non-Maxine path for GPU-only video effects.

- It does **not** depend on NVIDIA Maxine SDK assets.
- It runs inference via **ONNX Runtime** with the **CUDA Execution Provider** (CUDA EP).
- There is **no CPU fallback** in this backend: if CUDA / ONNX Runtime / model packs are missing, effects are marked unavailable.

This document covers prerequisites, model pack layout, and common troubleshooting.

## 1) Prerequisites

### NVIDIA driver / CUDA

You need a working NVIDIA driver stack such that the CUDA driver API is available.

Quick sanity check:

```bash
nvidia-smi
```

If `nvidia-smi` fails, fix the driver install first. The Open CUDA backend will be blocked with `cuda_unavailable` in daemon status.

### ONNX Runtime with CUDA EP

StudioCast discovers ONNX Runtime via CMake. For Open CUDA you need an ONNX Runtime build that includes the **CUDA execution provider**.

Repo-provided helper (Ubuntu 22.04+):

```bash
./scripts/bootstrap_ubuntu22.sh
```

This script:

- installs build prerequisites (Qt/CMake/Ninja/etc.),
- downloads the official ONNX Runtime **GPU** tarball,
- installs it under `/opt/studiocast/onnxruntime/<version>/...`,
- and provides a `pkg-config` entry (`onnxruntime.pc`) so CMake can find it.

If you install ONNX Runtime another way, make sure this works:

```bash
pkg-config --modversion onnxruntime
```

## 2) Build (ensure Open CUDA is enabled)

On Linux, `STUDIOCAST_ENABLE_OPEN_CUDA` defaults to **ON**.

If you want to force it:

```bash
cmake -S . -B <build-dir> -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSTUDIOCAST_ENABLE_OPEN_CUDA=ON
cmake --build <build-dir> --target studiocastd studiocastctl studiocast-open
```

If the daemon reports `onnxruntime_not_found`, it usually means ONNX Runtime wasn’t found at build time.

## 3) Install a model pack

Open CUDA discovers **user-supplied model packs** at runtime.

The default model root is:

```
~/.local/share/studiocast/models/open_cuda/
```

To print the authoritative path on your machine:

```bash
./<build-dir>/studiocast-open paths
```

### Model pack layout

Each model pack is a directory named by `<model_id>`:

```text
~/.local/share/studiocast/models/open_cuda/<model_id>/
  model.onnx
  model.json
  LICENSE.txt
  README.md        (optional)
```

You can validate discovery with:

```bash
./<build-dir>/studiocast-open list-models
```

### `model.json` (metadata)

`model.json` describes how StudioCast should feed the model (layout, tensor names, normalization, etc.).

Minimal example (you must adjust the tensor names and shape to match your ONNX model):

```json
{
  "id": "modnet",
  "display_name": "MODNet",
  "task": "matting",
  "onnx_filename": "model.onnx",
  "input": {
    "name": "input",
    "layout": "nchw",
    "dtype": "float32",
    "width": 512,
    "height": 512,
    "channels": 3
  },
  "output": {
    "name": "alpha",
    "kind": "alpha",
    "dtype": "float32"
  },
  "preprocess": {
    "mean": [0.5, 0.5, 0.5],
    "std": [0.5, 0.5, 0.5],
    "color": "rgb",
    "range": "0..1"
  }
}
```

Notes:

- `onnx_filename` must be a **safe relative path** within the pack directory.
- `input.name` and `output.name` must match the **actual tensor names** in your ONNX graph.
- `width`/`height` must match what the model expects.

## 4) Verify the backend is runnable

### Verify model pack discovery

```bash
./<build-dir>/studiocast-open list-models
```

You should see your `<model_id>` under “Valid model packs”.

### Verify daemon status (`GET_STATUS`)

Start the daemon:

```bash
./<build-dir>/studiocastd
```

In another terminal, check status JSON and look for `open_cuda.ok`:

```bash
./<build-dir>/studiocastctl status
```

The status payload includes:

- `engines.open_cuda` (preferred)
- a convenience top-level `open_cuda` alias

When runnable, you should see:

- `open_cuda.ok: true`
- `open_cuda.available_effects` containing the virtual background effect IDs

### Force Open CUDA + enable blur (no Maxine required)

This uses the canonical effect IDs (see `src/core/video/effects/broadcast_effect_contract.h`).

```bash
./<build-dir>/studiocastctl effects enable virtual_background.blur \
  --engine open_cuda \
  --strength 0.25
```

If everything is installed correctly, the pipeline will select the Open CUDA engine for virtual background blur.

## Troubleshooting

### `open_cuda.ok` is false and `blocked_effects` contains `onnxruntime_not_found`

- Your build was compiled without ONNX Runtime.
- Install ONNX Runtime (GPU build) and rebuild.

On Ubuntu 22.04+, the intended path is:

```bash
./scripts/bootstrap_ubuntu22.sh
```

### `open_cuda.ok` is false and `blocked_effects` contains `cuda_unavailable`

- NVIDIA driver / CUDA runtime is not available to the process.
- Verify `nvidia-smi` works.

### `open_cuda.ok` is false and `blocked_effects` contains `missing_model_packs`

- No usable model packs were discovered.
- Run `studiocast-open list-models` to see “Problems” (e.g., missing files or invalid JSON).

### Model pack is listed under “Problems”

Common causes:

- `model.json` is invalid JSON
- `model.onnx` is missing
- `onnx_filename` points outside the pack directory (rejected for safety)
