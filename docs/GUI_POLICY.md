# StudioCast GUI Policy

Date: 2026-07-05

This document defines how the StudioCast GUI should interact with effect
pipelines, daemon state, virtual devices, model backends, and the rest of the
application. It is a development policy, not a design mockup.

The short version: the GUI is a controller and presentation layer. The daemon is
the runtime authority.

## Current Interaction Map

The GUI target, `studiocast`, is a Qt Widgets application under `src/gui`. It
does not own the production camera or audio pipelines directly.

At runtime the GUI interacts with the application through these boundaries:

- `studiocastd` owns camera service state, audio service state, effect
  configuration, engine availability, pipeline lifecycle, virtual device state,
  persisted config, and status JSON.
- `src/core/ipc/daemon_client.*` is the GUI's production control channel to the
  daemon.
- `GET_STATUS` is the main read path for runtime state, readiness, active
  backends, diagnostics, model availability, consumer state, and pipeline
  errors.
- `SET_VIDEO_CONFIG` changes camera input/output/format intent.
- `SET_VIDEO_EFFECTS_JSON` changes canonical video effects.
- `SET_ENABLED` changes camera pipeline enablement intent.
- `SET_AUDIO_CONFIG` changes microphone source, speaker target, virtual audio
  lifecycle intent, routing intent, and canonical audio effects.

The GUI also performs limited local reads for immediate UI support:

- Camera setup may probe v4l2loopback devices for selectors and setup hints.
- Camera preview may open the daemon-resolved virtual camera output only after
  the user explicitly enables preview.
- Audio pages may list Pulse sources/sinks for selectors and diagnostics.
- Support and diagnostics pages may collect copyable local status.

Those local reads do not make the GUI authoritative. They are convenience and
diagnostic inputs only.

## Authority Rules

The daemon MUST remain the single source of truth for:

- Whether camera, microphone, or speaker processing is running, starting, idle,
  disabled, failed, or waiting for a consumer.
- Whether virtual devices are present and usable.
- Which engine is active after an `auto` preference resolves.
- Which effects are available, blocked, degraded, or disabled.
- Why an effect or backend is unavailable.
- Which model packs are installed, missing, invalid, or selected.
- Safe audio source and speaker target resolution.
- Persisted camera, audio, and effect configuration.

The GUI MAY keep local widget state while a user is editing, but committed state
MUST be reconciled against daemon status after every write.

The GUI MUST NOT:

- Run production camera, microphone, speaker, or effect pipelines inside Qt
  widgets.
- Compute authoritative effect availability from local guesses.
- Treat a selected backend preference as proof that the backend is active.
- Silently replace missing persisted sources, sinks, or models with defaults.
- Start or destroy production virtual audio devices directly in release builds
  when daemon IPC is unavailable.
- Add CPU or GPU effect processing in GUI preview that differs from daemon
  output.

## Canonical Schemas

Video effects use `BroadcastCameraEffects`.

- Contract IDs and parameter IDs are defined in
  `src/core/video/effects/broadcast_effect_contract.h`.
- The GUI writes video effects through `SET_VIDEO_EFFECTS_JSON`.
- The GUI should serialize through
  `BroadcastCameraEffectsContractToJson(...)` rather than assembling ad hoc
  JSON.
- The daemon applies patches with
  `ApplyBroadcastCameraEffectsPatchJsonText(...)`.

Audio effects use `BroadcastAudioEffects`.

- The GUI writes audio effects inside `SET_AUDIO_CONFIG` as `audio_effects`.
- The GUI must preserve fields it does not expose when patching audio effects.
- The daemon validates safety and persists the resulting config.

Legacy video fields and compatibility commands may exist for older clients, but
new GUI work MUST use the canonical schemas.

## Pipeline Interaction Rules

Camera:

- The GUI may let users choose input device, virtual output device, width,
  height, FPS, engine preference, and effect parameters.
- Starting the camera MUST save video config, save video effects, then request
  daemon enablement.
- The daemon decides when the heavy camera pipeline actually runs based on
  enablement, virtual camera availability, consumer detection, and preview
  consumption.
- Setup controls that would restart or reconfigure the pipeline SHOULD be locked
  or clearly guarded while camera processing is running.
- Preview MUST be opt-in. It opens the virtual output camera and therefore
  counts as a consumer. The GUI must never hide that behavior.

Microphone:

- The GUI may list physical input sources and show `Auto`.
- StudioCast virtual sources, Pulse monitor sources, and unsafe loopback
  candidates MUST be filtered out of normal selection.
- Source changes MUST go through `SET_AUDIO_CONFIG`.
- The daemon resolves `auto` to a safe physical source or reports a status/config
  error.
- Effect controls select intent. The daemon chooses Maxine, Open Audio,
  pass-through, fallback, or disabled behavior.

Speakers:

- The GUI may list safe physical output sinks and show `Auto`.
- StudioCast virtual speaker targets and unsafe sinks MUST be filtered out of
  normal selection.
- Speaker enable, stop, target changes, and destroy intent MUST go through
  `SET_AUDIO_CONFIG` in release builds.
- The GUI must distinguish pass-through loopback routing from processed speaker
  pipeline routing using daemon status.

Engines and models:

- Engine preference is user intent. Active backend is daemon status.
- `auto` may resolve to Maxine, Open CUDA/Open Video, Open Audio, pass-through,
  mixed backend state, or disabled effects.
- The GUI may display daemon-reported install hints and model lists.
- The GUI must not claim a model is usable unless daemon diagnostics report it.
- Missing configured model IDs or paths should remain visible as missing until
  the user changes them.

## Status And Polling

Status polling must be bounded and UI-safe.

- Daemon calls from the GUI should use short connect and I/O timeouts.
- The UI must remain responsive when the daemon is stopped, starting, wedged,
  restarted, or returning unreadable JSON.
- New page-specific polling should be avoided when a shared parsed snapshot can
  serve the need.
- New status fields should be added to `DaemonStatusSnapshot` when they are
  shared by more than one page or are part of the product readiness model.
- Page-local JSON parsing is acceptable only for narrow transitional code. New
  long-lived features should prefer shared status parsing.
- Poll intervals should reflect the cost of the underlying status path. Heavy
  diagnostics must be cached by the daemon or explicitly gated.

Current code still has both shared status parsing and page-local parsers. Future
work should reduce duplication by moving common daemon state into
`DaemonStatusSnapshot` and focused GUI control helpers.

## Write Semantics

GUI controls are generally write-through, but writes are not successful until
the daemon accepts them.

For every daemon mutation:

- Validate obvious UI-side conflicts before sending the request.
- Send the smallest coherent patch that preserves unrepresented settings.
- On failure, report an actionable error and resync the visible control from the
  last daemon state.
- On success, poll or refresh daemon status so the UI shows the authoritative
  applied state.
- Do not leave rejected values displayed as if they were active.

When a write affects a live pipeline, the GUI should assume the daemon may need
to restart, degrade, or reject pipeline stages. The GUI should report the daemon
result rather than predicting the outcome locally.

## Diagnostics And User-Facing State

The default UI should translate daemon status into plain readiness states:

- Ready
- Waiting for app
- Processing
- Starting
- Needs setup
- Missing virtual device
- Missing physical device
- Missing model
- Backend unavailable
- Service unavailable
- Needs attention

Raw daemon JSON, socket paths, v4l2loopback details, Pulse module details,
engine diagnostics, and setup commands should remain copyable, but they should
live behind Support, Diagnostics, Advanced, or expandable details unless they
are the direct next action.

Disabled controls should have a visible reason near the affected control or in a
tooltip/detail panel. The reason should come from daemon status whenever
possible.

## UI Thread And Process Rules

The GUI must not freeze while gathering diagnostics or running helper tools.

- Short daemon IPC calls are acceptable on the UI thread only with strict
  timeouts.
- Long-running helper commands, model installs, package operations, and broad
  diagnostics must run asynchronously or behind an explicit progress state.
- Repeated failed preview opens, daemon calls, or device probes must have
  backoff or bounded retry behavior.
- Polling and diagnostics must not trigger pipeline hot-path work beyond what
  the daemon already caches or reports.

## Release Versus Debug Behavior

Release builds are daemon-managed.

- Production virtual audio device mutation must go through daemon IPC.
- Direct `pactl` mutation is allowed only for debug/developer fallback paths and
  must be visually separated from daemon-managed workflows.
- Read-only local Pulse/v4l2 diagnostics are allowed in release builds if they
  do not contradict daemon status.
- Destructive actions must ask for confirmation and must leave status unchanged
  when cancelled.

## Interaction With Pipeline Performance Policy

The GUI must not undermine pipeline performance policy.

- Do not add GUI status calls that force per-frame model scans, provider probes,
  GPU synchronization, or CPU/GPU transfers.
- Do not make preview a second processing pipeline.
- Do not duplicate effect planning in the GUI.
- Do expose daemon-reported degraded effects, fallback state, active provider,
  transfer/debug counters, and per-stage timing when those diagnostics exist.
- If a pipeline feature needs new observability, add it to daemon status first,
  then render it in the GUI.

## Development Checklist

Before merging a GUI change that touches pipelines, effects, engines, models, or
virtual devices, answer these questions:

- Does the production mutation go through the daemon?
- Is the daemon still authoritative for active state and availability?
- Does the GUI preserve config fields it does not expose?
- Does failure leave the previous daemon state visible?
- Are missing devices/models preserved visibly instead of silently replaced?
- Does the UI remain responsive when daemon IPC fails?
- Are unsafe audio sources/sinks filtered or rejected?
- Is preview opt-in and clearly treated as a consumer?
- Are raw diagnostics still available for support?
- Is new status parsing shared when more than one page needs it?

Recommended verification for code changes:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSTUDIOCAST_ENABLE_OPEN_CUDA=OFF \
  -DSTUDIOCAST_ENABLE_OPEN_AUDIO=OFF \
  -DSTUDIOCAST_ENABLE_DLIB=OFF
cmake --build build
ctest --test-dir build --output-on-failure
./build/studiocast-probe --self-test
```

For GUI behavior that depends on hardware, virtual devices, desktop consumers,
or model packs, use `docs/MANUAL_TESTING.md` as the regression checklist.

