# Latency Policy

## Guiding Mantra

Every millisecond in the live path belongs to the user.

StudioCast should not add work to live audio or video paths unless that work is
required to produce the current frame or audio block, or unless measurement proves
that the reliability gain is worth the cost. Diagnostics, safety checks, retries,
fallbacks, and recovery logic are valuable only when they do not quietly make
normal operation slower.

## What Counts As Latency

Latency is any delay that makes camera or microphone output reach the user's app
later than it otherwise would. This includes:

- Glass-to-glass video delay.
- Microphone-to-sink audio delay.
- Frame pacing jitter.
- Audio glitches caused by late or blocked processing.
- Queue buildup that makes output stale.
- GPU/CPU synchronization stalls.
- Startup checks that delay entering a call.
- Recovery logic that blocks the live path after a failure.

Startup latency matters, but live-path latency matters more. A slower, clearer
startup failure is often acceptable. A slower per-frame or per-audio-block path is
not acceptable unless the user explicitly enabled the feature that requires it.

## Default Bias

- Prefer doing work at startup, model-load time, session-creation time, or through
  explicit diagnostics commands.
- Prefer bounded work over best-effort completeness.
- Prefer latest-frame-wins behavior over queueing stale frames.
- Prefer fail-open behavior for optional effects over blocking capture or output.
- Prefer measured fixes over speculative rewrites.
- Prefer small, targeted changes over broad architecture churn.

The goal is not to iterate endlessly. The goal is to remove proven sources of
user-visible failure, latency, instability, runtime breakage, or maintenance risk.

## Hot-Path Rules

The hot path includes video capture, frame conversion, effect processing, output
writes, audio capture, audio processing, audio playback, and real-time callbacks.

Do not add these to the normal hot path unless there is a measured and documented
reason:

- Per-frame or per-block subprocess calls.
- Per-frame filesystem, network, package-manager, or device-probing work.
- Per-frame dynamic allocation in tight loops.
- Per-frame string formatting, JSON construction, or verbose logging.
- Extra syscalls such as `poll()` or `ioctl()` on every frame without benchmark
  evidence.
- Unbounded waits, retries, queues, futures, or condition-variable handoffs.
- Locks that can be held by GUI, diagnostics, startup, shutdown, or logging code.
- CPU/GPU transfers that are only needed for reporting, previews, or debugging.
- Model checksum, manifest, or file-stat validation while streaming.

If a hot-path check is unavoidable, it needs an explicit latency budget, a clear
reason it cannot run elsewhere, and a regression test or benchmark.

## Queueing And Dropping

StudioCast should prefer fresh output over complete output.

- Video inference queues must be bounded.
- When processing falls behind, drop stale frames instead of building backlog.
- Effects should consume the newest available frame when that preserves visual
  correctness.
- Audio queues must stay bounded and should favor glitch prevention over
  accumulating delay.
- Shutdown and recovery paths must not wait for a backlog to drain before the
  user can recover.

Any queue added to the pipeline must declare:

- Its maximum size.
- What happens when full.
- Whether it preserves order or latest-frame-wins.
- How it behaves during shutdown.
- How tests prove it cannot grow without bound.

## Diagnostics And Status

Diagnostics are important, but they must not become hidden pipeline work.

Acceptable defaults:

- Startup-time provider checks.
- Session-creation warnings.
- GUI-visible cached status snapshots.
- Atomic counters that are already needed for control flow.
- Rate-limited logs on state transitions.
- Explicit debug modes enabled by environment variable or command-line flag.

Avoid by default:

- Formatting status text inside real-time audio callbacks.
- Building detailed JSON on every frame.
- Rechecking CUDA, TensorRT, Pulse, v4l2loopback, model manifests, or package
  metadata while streaming.
- Polling helper processes for status from the live path.
- Logging repeated per-frame failures without rate limiting.

Status should answer "what is active and why did fallback happen" without adding
work to every frame or audio block.

## Failure Handling

Optional features should not take down the pipeline.

- Optional video effects should fail open to pass-through output when possible.
- Capture failures, format-conversion failures, and output-write failures remain
  fatal when StudioCast cannot produce valid output.
- Circuit breakers should run on state transitions, not by doing expensive work
  per frame.
- Recovery should use cooldowns and cached failure reasons rather than repeated
  expensive probes.
- Timeouts should be stop-aware and bounded, but must be designed so the normal
  successful path stays cheap.

Adding a timeout is not automatically latency-neutral. For example, adding a
blocking wait or a `poll()` before every v4l2loopback write must be justified with
measurement against the current writer path.

## Passthrough Mode

Passthrough is the baseline performance contract.

When effects are disabled, StudioCast should avoid:

- Model initialization.
- GPU uploads or downloads.
- Matte generation.
- Effect queues.
- Per-frame diagnostics for disabled subsystems.
- Extra format conversions that are not required by the selected devices.

Any change that makes passthrough slower needs a strong reason and a before/after
measurement.

## Effects-Active Mode

Effects are allowed to cost latency because the user enabled them, but the cost
must be bounded and visible.

- Avoid duplicate inference when effects can share intermediate results.
- Avoid CPU round trips when a GPU result can stay on GPU for the next stage.
- Cache only when invalidation rules are clear and memory growth is bounded.
- Add temporal smoothing only when it fixes a measured or visible artifact.
- Prefer skipping effect work under pressure over delaying the full output stream.
- Keep effect failure isolated so one optional effect does not block unrelated
  output.

Performance work in effects-active mode should be driven by measurements such as
GPU upload/download counts, inference time, queue depth, frame age, and drop rate.

## Audio-Specific Rules

Audio has less tolerance for jitter than video.

- Real-time callbacks should do the least work possible.
- Avoid allocations, string formatting, locks, blocking waits, and provider probes
  in audio callbacks.
- Keep capture and playback buffers bounded.
- Preserve stop-interrupt behavior for blocked Pulse operations.
- Report open failures during start rather than pretending the pipeline started.
- Do not introduce helper-process isolation unless a specific crash-prone runtime
  justifies the extra IPC latency and operational complexity.

Audio diagnostics should be callback-safe, cached, atomic, or debug-gated.

## GPU And CPU Transfer Rules

GPU acceleration helps only if transfer costs do not erase the gain.

- Count uploads, downloads, alpha downloads, CPU-tail stages, and forced sync
  points before refactoring GPU code.
- Avoid downloading intermediate data just to upload it again.
- Keep matting, compositing, key-light, auto-frame, and final output decisions on
  one side of the GPU/CPU boundary when practical.
- Do not add new GPU synchronization points for diagnostics.
- Do not rewrite working GPU sections until transfer accounting shows the target.

The policy is measure, then consolidate. Not consolidate first and hope.

## Runtime And Packaging Checks

Runtime checks should prevent broken calls without slowing active calls.

- ONNX Runtime CUDA and TensorRT provider checks belong at startup or session
  creation.
- Model manifest and checksum verification belong at install, startup, or explicit
  model-selection time.
- Package smoke checks belong in CI and release validation.
- v4l2loopback capability and busy-device diagnostics should be startup/status
  checks unless a measured write-path guard is required.

The live path should consume already-validated configuration, not rediscover the
machine on every frame.

## Measurement Requirements

Any latency-sensitive change should include a short measurement note. For larger
changes, record:

- Hardware and driver context.
- Mode: passthrough, single effect, combined effects, audio model active, or
  fallback.
- Input and output format.
- Average, p95, and p99 frame or block processing time when available.
- Queue depth or frame age where relevant.
- Drop count where relevant.
- Before/after comparison against the previous implementation.

If a change cannot be measured yet, treat it as design-first unless it fixes a
clear correctness failure.

## Decision Framework

Use this order when deciding what to build next:

1. Fix now: prevents black output, dead audio, call-start failure, device lockup,
   unbounded queueing, or runtime fallback confusion without adding steady-state
   latency.
2. Design first: may improve resilience but could add hot-path cost, such as
   write timeouts, extra polling, helper processes, cache layers, or smoothing.
3. Measure first: likely performance work where the bottleneck is not proven.
4. Defer: broad rewrites, duplicate architecture, wishlist features, or changes
   that improve elegance more than user-visible behavior.

The burden of proof is higher for anything that runs per frame, per audio block,
or while a call is active.

## Review Checklist

Before accepting a pipeline change, answer:

- Does this run in the live path?
- What is the worst-case wait?
- Can it allocate, block, log, probe, or call into another process?
- What happens when the queue is full?
- What happens during shutdown?
- Does passthrough remain as cheap as before?
- Is the effect-active cost visible, bounded, and tied to an enabled feature?
- Are diagnostics startup-time, cached, atomic, rate-limited, or debug-gated?
- Is there a test or benchmark that would catch a latency regression?

If the answer is unclear, design first.
