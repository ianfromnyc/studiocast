# StudioCast GUI/Daemon Roadmap

This roadmap is grounded in:

- `docs/GUI_POLICY.md`
- `docs/PIPELINE_POLICY.md`
- `docs/ARCHITECTURE.md`

The immediate goal is to remove fragile GUI ownership, duplicated status parsing,
blocking local probes, stale write-through state, and status paths that can force
expensive backend work. Work is ordered by dependency: first make daemon IPC and
write semantics reliable, then centralize status flow, then move readiness and
diagnostics behind daemon-owned contracts.

## Phase 0: Stabilize IPC And Write Semantics

These fixes unblock reliable GUI state. Do these before larger GUI refactors so
the shared status path has a stable daemon to talk to.

1. Harden daemon IPC failure handling.
   - Fix `SIGPIPE` risk when replying to clients that disconnect early.
   - Reap, detach, or pool daemon client threads instead of accumulating a
     joinable thread object per accepted IPC connection.
   - Relevant files: `src/core/ipc/daemon_server.cpp`,
     `src/daemon/studiocastd_main.cpp`.
   - Verify with repeated early-disconnect clients and high-volume `GET_STATUS`
     calls; daemon must stay alive and thread count must stay bounded.

2. Preserve hidden audio effect fields in `SET_AUDIO_CONFIG`.
   - Partial GUI writes must patch the existing audio effect config, not replace
     omitted fields with defaults.
   - Relevant files: `src/daemon/studiocastd_main.cpp`,
     `src/core/audio/effects/broadcast_audio_effects_json.cpp`.
   - Verify by seeding model IDs, AEC, superres, and speaker settings, sending a
     one-field GUI audio patch, and asserting every omitted field is unchanged.

3. Resync video controls after rejected daemon writes.
   - `SET_VIDEO_EFFECTS_JSON` failures must revert visible controls to daemon
     state instead of leaving local `effects_` as false UI state.
   - Relevant file: `src/gui/pages/video_page.cpp`.
   - Verify with a fake daemon that rejects effect writes; controls must return
     to the last daemon-accepted state.

4. Debounce high-frequency GUI writes.
   - Slider drags should not send a full daemon patch for every `valueChanged`
     tick.
   - Apply to video effect sliders and audio effect sliders.
   - Relevant files: `src/gui/pages/video_page.cpp`,
     `src/gui/pages/audio_page.cpp`.
   - Verify a long drag produces one or a small bounded number of writes.

5. Replace UI-thread blocking helper calls with bounded async flows.
   - Convert video install-hint helper processes away from
     `waitForFinished(...)` on the GUI thread.
   - Give IPC client reads/writes a total monotonic deadline, not a fresh timeout
     per wait/read loop.
   - Relevant files: `src/gui/pages/video_page.cpp`,
     `src/core/ipc/daemon_client.cpp`.
   - Verify slow helper scripts and trickle-response fake daemons do not freeze
     the GUI beyond the configured bound.

## Phase 1: Centralize GUI Status Flow

Do this after Phase 0 so status updates are dependable. The target is one shared
GUI status path and typed parsing through `DaemonStatusSnapshot`.

1. Feed `VideoPage` and `AudioPage` from the shared `StatusPoller`.
   - Store page pointers in `MainWindow` and forward snapshots to all pages.
   - Remove page-owned routine status timers once shared delivery covers their
     needs.
   - Relevant files: `src/gui/main_window.cpp`,
     `src/gui/pages/video_page.cpp`, `src/gui/pages/audio_page.cpp`,
     `src/gui/status/status_poller.cpp`.
   - Verify the GUI issues one routine `GET_STATUS` per poll interval.

2. Move page-local JSON parsing into shared status parsing.
   - Promote needed video/audio fields into `DaemonStatusSnapshot`.
   - Remove page-local parsers except for short-lived migration code.
   - Include advanced-page raw JSON parsing only where it remains clearly
     diagnostic and non-authoritative.
   - Relevant files: `src/gui/status/*`, `src/gui/pages/video_page.cpp`,
     `src/gui/pages/audio_page.cpp`, `src/gui/pages/advanced_page.cpp`.
   - Verify with focused snapshot tests for daemon unavailable/restart, missing
     configured models/devices, write rejection, and preserved selections.

3. Consolidate daemon-call helpers.
   - Avoid each page growing its own request wrapper, timeout policy, and error
     behavior.
   - Shared helpers should support mutation success refresh and failure resync.
   - Relevant files: `src/gui/pages/*`, `src/core/ipc/daemon_client.cpp`.

## Phase 2: Move Availability And Readiness To The Daemon

Do this after Phase 1 so the GUI has typed fields to consume. The GUI should
render daemon state; it should not infer production readiness from local probes.

1. Add daemon-owned video effect readiness.
   - Status should expose per-effect readiness keyed by contract effect ID:
     requested model, resolved/default model, backend, available/blocked/degraded
     state, and reason.
   - This is especially important while idle, before preview or camera startup.
   - Relevant files: `src/daemon/studiocastd_main.cpp`,
     `src/core/video/effects/*`, `src/core/video/*`,
     `src/gui/status/*`, `src/gui/pages/video_page.cpp`.
   - Verify a configured missing Open Video model is reported as missing while
     the camera is idle, without starting the pipeline.

2. Remove local model-pack scans from routine GUI refresh.
   - `VideoPage` must not call `ModelPackRegistry::ScanDefault()` from routine
     UI enablement/status paths.
   - Model availability should come from daemon-owned cached status fields.
   - Relevant file: `src/gui/pages/video_page.cpp`.
   - Verify routine GUI polling does not touch model directories.

3. Move audio lifecycle/readiness authority to daemon status.
   - Release UI enablement must not be derived from local `pactl` module scans.
   - Missing configured devices may be displayed for preservation, but unsafe or
     unavailable selections must be clearly non-writable unless the daemon accepts
     them.
   - Relevant files: `src/gui/pages/audio_page.cpp`,
     `src/daemon/studiocastd_main.cpp`, `src/core/audio/*`.
   - Verify fake local Pulse state that conflicts with daemon status does not
     change release UI authority.

## Phase 3: Split Lightweight Status From Diagnostics

Do this after readiness fields are explicit. The daemon can still own
diagnostics, but routine GUI polling must not force provider probes, model
rescans, CUDA initialization, or unbounded helper commands.

1. Keep `GET_STATUS` lightweight.
   - `GET_STATUS` should serialize cached runtime/config/readiness state only.
   - Expensive work belongs behind explicit commands such as `GET_DIAGNOSTICS`,
     `REFRESH_MODELS`, or daemon-owned background refresh with TTL/backoff.
   - Relevant files: `src/daemon/studiocastd_main.cpp`,
     `src/core/open_video/*`, `src/core/audio/open_audio/*`.
   - Verify diagnostic functions are not called by routine status polling.

2. Bound or cache Pulse/PipeWire subprocess diagnostics.
   - Avoid unbounded `pactl` subprocess work in daemon status serialization and
     GUI page refresh.
   - Add subprocess deadlines, cached results, and failed-probe backoff.
   - Relevant files: `src/core/audio/virtual_mic.cpp`,
     `src/core/audio/pulse/pactl.cpp`, `src/core/util/exec.cpp`,
     `src/daemon/studiocastd_main.cpp`.
   - Verify a sleeping `pactl` shim cannot stall routine GUI status updates.

## Phase 4: Align Effect Schemas And Persistence

Do this after mutation and status semantics are stable. The objective is one
clear contract per surface, with no conflicting validation behavior.

1. Align video effect contract, persistence, and daemon IPC semantics.
   - Persistence and IPC should accept the same valid effect combinations or
     explicitly mark legacy schema behavior.
   - Preserve selected models and hidden fields across save/load and patch
     operations.
   - Relevant files: `src/core/video/effects/*`,
     `src/core/config/daemon_config.cpp`,
     `src/daemon/studiocastd_main.cpp`.
   - Verify valid contract combinations round-trip through save/load,
     `SET_VIDEO_EFFECTS_JSON`, and `GET_CONFIG`.

2. Align audio effect contract coverage.
   - Audio parser/serializer behavior should be driven by the same contract
     concepts the GUI and daemon claim to support.
   - Partial patches must preserve unmentioned fields.
   - Relevant files: `src/core/audio/effects/*`,
     `src/daemon/studiocastd_main.cpp`.

## Phase 5: Policy-Critical Tests

Add focused tests alongside the phases above instead of waiting until the end.
Minimum coverage:

- Daemon IPC survives early disconnects and high-volume polling.
- GUI mutation failure triggers visible resync.
- Daemon unavailable/restart keeps or restores last-good visible state
  intentionally.
- Audio config patches preserve hidden effect fields.
- Video model/effect readiness comes from daemon status, not local scans.
- Routine `GET_STATUS` does not run model scans, provider probes, CUDA
  initialization, or unbounded Pulse/PipeWire subprocesses.
- Release builds do not directly mutate production virtual audio devices outside
  daemon IPC.
- Preview remains opt-in and does not become a second effect pipeline.

## Do Not Change

- Do not make the GUI the production authority for pipeline state, model
  availability, effect readiness, or release virtual audio device mutation.
- Do not add new page-local daemon JSON parsers or page-local routine status
  pollers. New fields belong in `DaemonStatusSnapshot`.
- Do not use GUI-local model scans, provider probes, Pulse/PipeWire scans, or
  filesystem probes as authoritative production readiness.
- Do not let routine GUI polling trigger model rescans, ONNX provider probes,
  CUDA initialization, CPU/GPU synchronization, or hot-path diagnostics.
- Do not convert camera preview into a second processing pipeline. Keep preview
  opt-in and limited to reading daemon-resolved output for display.
- Do not change the camera start ordering: save camera config, save video
  effects, then enable the daemon camera.
- Do not remove debug-only direct Pulse fallback guards by making those paths
  available in release builds.
- Do not replace partial write semantics with whole-object defaults. GUI writes
  must preserve fields they do not expose.
- Do not use blocking `QProcess::waitForFinished`, unbounded `popen`, or
  long-running helper commands on the GUI thread.
- Do not remove existing shared status snapshot tests; extend them as new daemon
  status fields are added.

## Known Good Behavior To Preserve

- Camera start currently follows the required save-config, save-effects,
  enable-daemon sequence.
- Preview currently reads daemon-resolved output and does not appear to run a
  second effect pipeline.
- Release virtual audio creation/destruction mostly routes through daemon IPC,
  with direct Pulse fallbacks guarded for debug paths.
- Existing `DaemonStatusSnapshot` tests already cover broad status parsing; keep
  using that shared parser as the center of GUI state.
