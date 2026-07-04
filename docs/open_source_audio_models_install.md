# Open Audio (ONNX Runtime) model packs

StudioCast supports an **Open Audio** backend for microphone effects (noise removal + “studio voice”-style enhancement)
using **ONNX Runtime** and user-supplied model packs.

This is the **open-source fallback** path when NVIDIA Maxine Audio Effects are not available (or when the user explicitly
selects `OPEN_SOURCE`).

StudioCast does **not** ship any model binaries in git.

## 1) Install curated models (recommended)

The repo includes a helper script that downloads curated, **streaming-capable** FastEnhancer variants and installs
them as StudioCast model packs:

```bash
./scripts/install.sh open-audio-models
```

Default install location:

```text
${XDG_DATA_HOME:-~/.local/share}/studiocast/models/open_audio/<model_id>/
```

You can override:

```bash
./scripts/install.sh open-audio-models --dest /opt/studiocast/models/open_audio
```

List curated packs:

```bash
./scripts/install.sh open-audio-models --list
```

Curated pack IDs (FastEnhancer, VCTK-Demand v1, 16kHz):

- `fastenhancer_s_vd_v1` (small) — good default for general noise removal
- `fastenhancer_m_vd_v1` (medium) — higher quality, good default for studio-voice style enhancement
- `fastenhancer_l_vd_v1` (large) — best quality, most CPU/GPU cost

## 2) Validate discovery + ONNX Runtime session

List installed packs:

```bash
build/studiocast-open audio-list-models
```

Run a session smoke test (prints tensor names/shapes and verifies the model loads):

```bash
build/studiocast-open audio-self-test --model-id fastenhancer_s_vd_v1
```

If you suspect CUDA EP issues, force CPU:

```bash
build/studiocast-open audio-self-test --model-id fastenhancer_s_vd_v1 --cpu-only
```

Benchmark end-to-end processing time (per 10 ms frame):

```bash
build/studiocast-open audio-bench --effect noise_removal --model-id fastenhancer_s_vd_v1 --seconds 10
```

If you suspect CUDA EP issues, force CPU:

```bash
build/studiocast-open audio-bench --effect studio_voice --model-id fastenhancer_m_vd_v1 --seconds 10 --cpu-only
```

To emit per-frame timings:

```bash
build/studiocast-open audio-bench --effect noise_removal --model-id fastenhancer_s_vd_v1 --frames 200 --csv
```

## 3) Enable in the GUI / config

In the GUI:

- **Audio → Microphone Effects**
  - Backend: **AUTO** (preferred) or **OPEN_SOURCE**
  - Model: **Default (auto)** or any FastEnhancer variant
  - Enable **Noise Removal** and/or **Studio Voice**
  - Adjust strength sliders

In headless mode you can also patch config via:

```bash
build/studiocastctl audio-effects get
build/studiocastctl audio-effects set --file my_audio_effects_patch.json
```

## 4) Pack format (for your own models)

A model pack is a directory named by `<model_id>`:

```text
~/.local/share/studiocast/models/open_audio/<model_id>/
  model.json
  <some_model>.onnx
  LICENSE.txt
```

`model.json` schema (see `src/core/open_audio/model_pack_registry.*` for authoritative parsing):

- `id` (string, required): must match the directory name
- `display_name` (string, required)
- `onnx_filename` (string, required): relative to the pack directory
- `sample_rate` (int, optional): defaults to 16000
- `channels` (int, optional): defaults to 1
- `effects` (array of strings, optional): e.g. `["noise_removal","studio_voice"]`
  - If omitted, StudioCast assumes the model can be used for any supported mic effect.
- `onnx_io` (object, optional): override tensor names and frame sizing
  - `frame_samples` (int): required if the model’s input shape is dynamic/unknown
  - `audio_input` / `audio_output` (string): tensor names (often `wav_in` / `wav_out`)
  - `state_inputs` / `state_outputs` (array): cache tensor names (optional; StudioCast can infer if omitted)
  - `aux_inputs` (object, optional): auxiliary control inputs
    - `strength` (string or object): if present, StudioCast feeds the UI strength to the model directly
      - object form: `{ "name": "<tensor>", "range": [min,max], "shape": [1] }`

**Current constraints (Phase 12 implementation):**
- Models must be streaming-friendly **10 ms hop** (`frame_samples = sample_rate/100`, e.g. 160 @ 16 kHz).
- Multi-channel is not supported (mono only).
- Supported model sample rates: **16 kHz** and **48 kHz**.
  - 16 kHz models are supported via internal 48k↔16k resampling.
- If you have a model with a different hop size (e.g. 256), you can still use it by wrapping/re-exporting it to a
  10 ms hop, or by extending the buffering logic in `OpenAudioAudioProcessor`.

Use the self-test tool to inspect a model’s tensor names and shapes:

```bash
build/studiocast-open audio-self-test --model-path /path/to/model.onnx
```

## 5) Troubleshooting

If `audio-list-models` shows “Problems”:

- Ensure the directory name matches `model.json:id`
- Ensure `onnx_filename` exists
- Ensure `LICENSE.txt` exists

If ONNX Runtime fails to load the model:

- Confirm your build includes ONNX Runtime (see CMake options / `STUDIOCAST_HAVE_ONNXRUNTIME`)
- Try `--cpu-only` to bypass CUDA EP issues
- Run `build/studiocast-open audio-self-test ...` and attach its output in bug reports
