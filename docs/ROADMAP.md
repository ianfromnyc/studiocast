# Roadmap

StudioCast's next work should protect the live audio/video path first. Every
pipeline change must either reduce user-visible failure, reduce measured latency,
or make an existing failure mode diagnosable without adding steady-state cost.

## Latency Policy

See [Latency Policy](LATENCY_POLICY.md) for the full policy. The short version:

- Do not add per-frame diagnostics, logging, allocations, subprocess calls, or
  extra syscalls to the normal hot path unless a benchmark shows the cost is
  negligible and the reliability gain is necessary.
- Prefer startup-time, session-creation-time, or env-gated diagnostics.
- Any hot-path safety mechanism must have an explicit latency budget and a test
  that proves it does not queue unbounded work.
- Debug counters for GPU transfers, Pulse health, or v4l2loopback behavior should
  be off by default unless they are simple atomic counters already needed for
  control flow.

## Fix Now

1. Optional video effects must fail open.
   - Add an effect-level circuit breaker for optional Maxine video effects.
   - Keep capture, conversion, and output write failures fatal when output cannot
     continue.
   - On transient effect failure, continue pass-through output and report the
     blocked effect, reason, and cooldown in status.
   - Tests: fake denoise, relight, virtual background, and auto-frame failures;
     assert frames continue and status reports the degraded effect.

2. Harden ONNX Runtime CUDA and TensorRT startup diagnostics.
   - Distinguish advertised providers from loadable providers before users start
     a call.
   - Centralize CUDA/TensorRT runtime discovery and safe provider-option handling
     in the shared ONNX layer so Open Video/Open CUDA and Open Audio do not drift.
   - Keep all probing at startup, diagnostics, or session creation time.
   - Tests: CPU-only ORT, CUDA provider advertised but runtime unavailable,
     TensorRT requested with CUDA fallback, and warning propagation.

3. Add an audio start handshake.
   - Make `AudioPipeline::Start()` report success only after Pulse capture and
     playback streams have opened, or return the open error immediately.
   - Preserve existing stop-interrupt behavior for blocked Pulse open/read/write
     and flush paths.
   - Tests: open failure returns immediately, delayed open is interruptible, and
     existing blocked-I/O shutdown tests still pass.

## Design First, Then Implement

1. v4l2loopback write timeout without normal-path latency regression.
   - Goal: prevent stuck writes from blocking capture, effects, pacing, and
     shutdown.
   - Avoid blindly adding a `poll()` syscall before every frame unless measured.
   - Evaluate nonblocking writer mode, bounded write waits, or a stop-aware safe
     writer mode.
   - Tests: fake `EAGAIN`, blocked write, stop during write, and output recovery.

2. Open CUDA and Maxine transfer accounting.
   - Add env-gated counters for Open CUDA active frames, uploads, final downloads,
     alpha downloads, CPU-tail stages, and Maxine duplicate matting.
   - Use the numbers to decide whether to unify GPU sections, move key-light or
     auto-frame CPU tails to GPU, or share Maxine Green Screen mattes.
   - Do not add smoothing or matte-cache changes until transfer data or visible
     jitter justifies them.
   - Tests: virtual background only, key light only, auto frame only, combined
     effects, and generation/cache invalidation.

3. Runtime status without hot-path work.
   - Surface Open Audio active provider, CPU fallback, disabled state, selected
     model, and last runtime warning.
   - Add Pulse health counters only if they are callback/atomic based or debug
     gated.
   - Avoid formatting strings or JSON from the real-time audio loop.
   - Tests: forced CUDA runtime failure, repeated failure disables model, and GUI
     status snapshots parse the new fields.

## Release Hardening

- Extend package smoke tests with fast metadata checks: version consistency,
  desktop IDs, staged helper binaries, service files, source archive contents,
  model manifest paths, and safe `--help` command smoke.
- Keep full AppImage/package creation separate from quick CI metadata checks.
- Include active model id/path/checksum or mtime in runtime status so model-pack
  replacement under the same id is visible.

## Defer

- No GStreamer rewrite.
- No audio helper-process rewrite unless a specific crash-prone model runtime
  needs isolation.
- No URL-only model downloads.
- No Maxine integration rewrite based on the reference project.
- No matte smoothing/cache work unless it is tied to measured jitter, measured
  transfer reduction, or a clear user-visible artifact.
