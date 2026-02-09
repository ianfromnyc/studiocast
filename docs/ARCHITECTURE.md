# Architecture (draft)

Phase 0 is mostly scaffolding, but there are already some hard “single source of truth” decisions that new
contributors should follow.

Planned components:
- GUI (Qt)
- Audio Service (PipeWire graph node)
- Video Service (V4L2 input + v4l2loopback output)
- Effects Engine abstraction layer (Maxine backends loaded at runtime)
- SDK Manager (downloads/installs user-obtained Maxine assets)

## Canonical effect model: `BroadcastCameraEffects`

The single canonical effect schema across **config persistence**, **IPC**, **daemon pipeline**, **GUI rendering**, and
**CLI JSON** is `BroadcastCameraEffects`.

Authoritative contract (stable effect IDs + parameter IDs):

- `src/core/video/effects/broadcast_effect_contract.h`

Hard rule: do not rename existing effect IDs or parameter IDs. If an effect must change semantics, add a new ID.

## GUI/daemon control plane (JSON)

The daemon exposes a small IPC protocol over a Unix socket.

- `GET_CONFIG` returns the canonical Broadcast effects JSON (effect IDs as keys).
  - Implementation: `studiocastd` uses
    `studiocast::video::BroadcastCameraEffectsContractToJson(...)` in
    `src/daemon/studiocastd_main.cpp`.

- `SET_VIDEO_EFFECTS_JSON <json>` applies a JSON patch against `BroadcastCameraEffects`.
  - Implementation: `studiocastd` uses
    `ApplyBroadcastCameraEffectsPatchJsonText(...)` in `src/daemon/studiocastd_main.cpp`.
  - Legacy compatibility: if the Broadcast patch parser fails, the daemon will try the legacy `CameraEffects` patch
    parser and return a warning if it had to do so.

CLI helpers:

- `studiocastctl effects get` → `GET_CONFIG`
- `studiocastctl effects set --file <effects.json|->` → `SET_VIDEO_EFFECTS_JSON` (file-based to avoid shell quoting)

## Availability: daemon `MaxineManager` is authoritative

Effect availability must be computed **only** by the daemon and exposed via `GET_STATUS`.

- The daemon calls `MaxineManager::Diagnose(...)` and includes the diagnostics JSON in status.
- The GUI should treat daemon status as the single source of truth for:
  - whether Maxine is usable on this machine
  - which effects are available vs blocked (with reason codes)
  - what remediation/install hints to show

## Open CUDA model packs (XDG)

The `open_cuda` backend (ONNX Runtime CUDA EP) discovers user-supplied model packs at runtime.

Default root:

- `~/.local/share/studiocast/models/open_cuda/<model_id>/`

Each pack contains:

- `model.json` (schema v1; see below)
- `model.onnx` (filename configurable via `onnx_filename`)
- `LICENSE.txt`

### `model.json` schema v1 (draft)

Required top-level fields:

- `id`, `display_name`
- `task`: currently only `"matting"`
- `onnx_filename`
- `input`: `{ name, layout, dtype, width, height, channels }`
- `output`: `{ name, kind, dtype }` where `kind` is `"alpha"`
- `preprocess`: `{ mean[3], std[3], color, range }` where `color` is `"rgb"` and `range` is `"0..1"`

## No CPU fallback (product rule)

Effects are GPU-only (Maxine + small CUDA post-process where needed). If Maxine, drivers, supported GPU, or feature
models are missing, the pipeline must not run effects and the UI must communicate the “unavailable” state.
