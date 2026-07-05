# StudioCast Pipeline Policy

This document defines the transfer and execution policy for StudioCast's live
audio/video pipelines. It is the stance reviewers should use when evaluating
pipeline changes, effect implementations, and performance work.

## Core Rule

The default live path should stay on the CPU when CPU work is sufficient. When a
heavy GPU-backed video section is necessary, the pipeline should cross the
CPU/GPU boundary once per frame:

1. Upload CPU frame data to GPU once before the first GPU stage.
2. Run all GPU-capable preprocessing, inference, matting, compositing, lighting,
   crop/scale, denoise, and final resize work on GPU.
3. Download once only when CPU-visible output is required, such as v4l2loopback
   frame submission or a genuinely CPU-only tail stage.

The intended shape is therefore:

```text
CPU-only frame path:
  capture/decode -> CPU effects/format conversion -> output

GPU-heavy frame path:
  capture/decode -> one upload -> GPU section -> one final readback -> output
```

The policy is not "always use the GPU." The policy is "do not bounce between CPU
and GPU once a heavy GPU section has started."

## Build Once, Reuse Until Reconfigured

Effect plans, model sessions, GPU buffers, format adapters, and stage bindings
should be built when the pipeline starts or when configuration changes. The
frame loop should consume that prepared plan.

Rebuild or invalidate the prepared plan only for clear events:

- Effect enablement, ordering, strength semantics, or engine preference changes.
- Model id, model path, provider, or runtime option changes.
- Capture or output geometry changes.
- Pixel format or backend capability changes.
- Explicit model-pack refresh or user-driven install/remove events.
- Runtime failure that disables, falls back, or reinitializes a stage.

Do not probe providers, rescan model packs, reparse manifests, or rebuild ORT
sessions on every frame when the selected configuration has not changed.

## Transfer Rules

All GPU paths should be designed around one contiguous per-frame GPU section.

- One upload at the start of the GPU section.
- Zero intermediate CPU readbacks for data that the next stage can consume on
  GPU.
- One final readback at the end, only if CPU-visible output is required.
- GPU resize should happen before the final readback when output dimensions
  differ.
- CPU fallback should stay CPU rather than uploading to GPU and then returning to
  CPU for most of the work.
- If a stage cannot avoid a CPU tail, the tail must be explicit, bounded,
  measured, and visible in diagnostics.

Intermediate downloads such as alpha/matte readbacks are treated as transfer
debt unless they are the documented CPU-tail behavior for that backend. If kept,
they must be counted separately from final RGB downloads.

## Backend Boundaries

The one-transfer rule applies to the selected heavy backend for the frame. Mixing
GPU runtimes is allowed only when the implementation can keep the frame on GPU
or when the extra transfer is explicitly treated as a known fallback.

Open CUDA:

- Open CUDA activity must cover every Open CUDA stage, not only virtual
  background.
- Virtual background, key light, auto frame, matting, and output scaling should
  share the same uploaded frame and per-frame matte when possible.
- CUDA ORT paths should prefer device tensors and IoBinding when a CUDA-capable
  execution provider is active.

Maxine:

- Maxine effects in the same frame should share staged GPU input, stream
  ownership, and matte outputs where SDK contracts allow it.
- Repeated RGB-to-BGR staging plus upload/download per Maxine effect is a target
  for consolidation.
- If the SDK requires duplicate matting or separate transfers, status should make
  that fact visible.

Open Video:

- CPU fallback models should remain CPU-only.
- CUDA-backed Open Video models should avoid CPU tensor packing and final tensor
  readback when a GPU tensor path is available.
- Shared analysis results, such as face detections and landmarks, should be
  cached by capture sequence so effects do not rerun the same inference in one
  frame.

## CPU Work That Is Still Acceptable

Some CPU work remains normal and does not violate this policy:

- Camera capture, MJPEG decode, YUYV/RGB conversion, and v4l2loopback write
  preparation when no heavy GPU stage is active.
- Final format conversion after the final GPU readback when the output device
  requires a CPU buffer.
- Small setup-time loops over devices, effects, model packs, or descriptors.
- CPU-only effect implementations selected by configuration or fallback.

These loops can still be optimized when measured, but they are not the primary
transfer-policy concern.

## Observability Requirements

Transfer behavior must be measurable before and after performance work.

The pipeline should expose, at least behind debug/status gates:

- GPU active frame count.
- CPU-to-GPU upload count.
- GPU-to-CPU final download count.
- Intermediate alpha/matte download count.
- CPU-tail stage count or names.
- Forced GPU synchronization count.
- Per-stage timing for capture, effects, scale, and write.
- Fallback state, active provider, and degraded effect state.

Diagnostics must not add new synchronization or transfer points to the normal
hot path.

## Scheduling Rules

The live path should prefer current frames over stale completeness.

- Drain queued capture frames and process the newest available frame.
- Use latest-frame-wins scheduling for expensive non-temporal analysis where the
  result can safely be stale-dropped.
- Keep temporal effects synchronous unless their frame dependencies are modeled
  explicitly.
- Prefer skipping optional effect work under pressure over delaying the whole
  output stream.

## Review Checklist

Before accepting a pipeline change, answer these questions:

- Does the no-effects path stay CPU-only with zero GPU transfers?
- If a heavy GPU section runs, is there exactly one upload and one final readback
  per frame?
- Are matting, compositing, key light, auto frame, denoise, and resize kept on
  the same side of the CPU/GPU boundary where practical?
- Are model/session/provider decisions cached until reconfiguration?
- Are CPU tails explicit, bounded, measured, and status-visible?
- Are fallback paths coherent, meaning CPU fallback stays CPU and GPU fallback
  stays GPU where possible?
- Can transfer counters prove the intended behavior?

## Priority Order

When improving the pipeline, use this order:

1. Remove per-frame setup work such as model-pack scans and provider probes.
2. Count all transfers and CPU tails accurately.
3. Consolidate GPU sections so enabled GPU effects share frame upload, matte,
   stream, and final readback.
4. Move CPU tails to GPU kernels when they sit inside an otherwise GPU-backed
   section.
5. Optimize scalar CPU conversion loops only after transfer behavior and setup
   churn are under control.

