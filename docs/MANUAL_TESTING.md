# Manual Testing

Status: starter plan for hardware and desktop workflow validation.
Last updated: 2026-06-30.

This document tracks manual tests that cannot be covered deterministically in
CI because they require real V4L2 devices, v4l2loopback behavior, desktop
consumers, GPU runtimes, or user permissions.

## Scope

Primary scope:

- GUI control of the daemon-driven video pipeline.
- Physical camera input selection.
- v4l2loopback output selection and consumer detection.
- GUI preview behavior.
- Daemon IPC failure and recovery behavior.
- Effect availability as reported by the daemon.
- Config persistence across GUI and daemon restarts.

Out of scope for this first pass:

- Full model quality assessment.
- Long-duration soak testing.
- Packaging on every supported distro.
- Performance tuning beyond obvious hangs, tight loops, or runaway CPU.

## Test Record

Create one completed record per test pass.

```text
Date:
Tester:
Git commit:
Build type:
OS and kernel:
Desktop session:
GPU and driver:
Camera devices:
v4l2loopback version:
Daemon launch mode: manual / systemd user service
Consumer apps tested: OBS / browser / Zoom / Teams / Discord / other
Result: pass / fail / partial
Notes:
Artifacts:
```

Useful artifacts:

- `build/studiocastctl status`
- `build/studiocastctl debug-report --out studiocast-debug-report.txt`
- `v4l2-ctl --list-devices`
- `v4l2-ctl --all -d /dev/videoX`
- GUI screenshots for user-visible errors or disabled controls
- Daemon logs from the terminal or `journalctl --user -u studiocastd.service`

## Prerequisites

- Ubuntu 22.04 or 24.04 desktop session.
- StudioCast built:

```bash
cmake --build build --target studiocast studiocastd studiocastctl
```

- v4l2loopback installed and loaded.

```bash
./scripts/setup.sh --v4l2loopback --load-loopback
v4l2-ctl --list-devices
```

- At least one readable physical camera for physical input tests.
- A desktop consumer app such as OBS or a browser/WebRTC camera test page.
- Optional: a second readable loopback device for loopback-as-input tests.
- Optional: Maxine SDK or Open CUDA model packs for positive effect availability
  tests.

## Baseline Sanity

- [ ] Run `build/studiocastctl status` before starting the daemon.

Expected:

- Command returns quickly.
- Error is clear that the daemon socket is missing or unreachable.
- It does not hang.

- [ ] Start the daemon manually:

```bash
build/studiocastd
```

- [ ] Run:

```bash
build/studiocastctl status
v4l2-ctl --list-devices
```

Expected:

- Status reports the configured StudioCast virtual camera.
- The virtual device exists in `v4l2-ctl --list-devices`.
- With no consumer open, heavy video processing is not running.

## GUI And Daemon IPC

- [ ] Start the GUI with no daemon running.

Expected:

- GUI remains responsive.
- Start/effects controls that require the daemon are disabled or fail with an
  actionable message.
- Status text explains how to start `studiocastd`.
- Preview stays off.

- [ ] Start the daemon while the GUI is already open.

Expected:

- GUI recovers without restart on the next poll or refresh.
- Status text switches from unreachable to reachable.
- Device and effect state match daemon status.

- [ ] Kill the daemon while the GUI is open, then restart it.

Expected:

- GUI does not hang.
- Controls reflect daemon unreachable while it is down.
- Preview closes or reports unavailable.
- After daemon restart, controls and status recover without restarting the GUI.

- [ ] Change camera settings in the GUI, close the GUI, restart it, and compare:

```bash
build/studiocastctl status
```

Expected:

- GUI state, daemon status, and persisted config agree.
- Failed daemon/config writes are not shown as successful GUI state.

## Device Selection

- [ ] With one physical camera and one StudioCast loopback output, open the GUI.

Expected:

- Physical camera is offered as an input.
- Writable StudioCast loopback is offered as output.
- The same `/dev/videoX` cannot be selected as both input and output.
- Labels make loopback devices distinguishable from physical devices.

- [ ] Select automatic input and automatic output, then start the camera.

Expected:

- Auto-selection chooses a readable input and a separate writable output.
- If no safe separate pair exists, start fails with an actionable error.

- [ ] If a readable loopback input device is available, select it as input and
  the StudioCast output as output.

Expected:

- Loopback-as-input works only when it is not the same device as the output.
- The GUI prevents input/output conflicts.
- Daemon status reports the resolved input and output devices clearly.

- [ ] Remove or block access to the selected input camera, then refresh/start.

Expected:

- Missing or permission-denied device states are visible.
- User can choose another device and recover without restarting the app.

## Consumer-Gated Pipeline Lifecycle

- [ ] Start the daemon and enable video with no OBS/browser/preview consumer.

Expected:

- Virtual output device is present and available.
- Pipeline state is idle because no consumer is present.
- CPU usage remains low.

- [ ] Open OBS or another consumer and select the StudioCast virtual camera.

Expected:

- Daemon detects a consumer.
- Pipeline starts after the configured grace period.
- GUI status shows consumer count and running/starting state.
- Consumer receives frames.

- [ ] Close the consumer.

Expected:

- Daemon detects the consumer is gone.
- Pipeline stops after the configured stop grace/min-run window.
- GUI status reflects idle/no-consumer state.

- [ ] While a consumer is active, change input device, output device,
  resolution, FPS, and effects one at a time.

Expected:

- Each change either succeeds and is reflected in daemon status, or fails with
  an actionable error.
- Restarts are bounded.
- No tight retry loop, runaway CPU, or GUI freeze occurs.
- Consumer recovers frames after each successful restart.

## GUI Preview

- [ ] Open the GUI with preview unchecked and video enabled.

Expected:

- GUI preview does not open the virtual camera.
- Preview does not count as a consumer until requested.

- [ ] Check the Preview box.

Expected:

- Preview opens the daemon-resolved virtual output device.
- Daemon consumer count increases.
- Pipeline starts if no other consumer is present.
- Preview shows processed output frames, not the raw physical camera feed.

- [ ] Uncheck the Preview box.

Expected:

- Preview stops.
- File descriptors are released.
- If no other consumer remains, pipeline returns to idle after the grace period.

Optional evidence:

```bash
lsof /dev/videoX
build/studiocastctl status
```

- [ ] Toggle preview repeatedly while starting/stopping OBS.

Expected:

- Retries are bounded.
- Status remains understandable during exclusive-caps transitions.
- No persistent stuck preview state remains after consumers close.

## Effects Availability

- [ ] Run on a machine without Maxine SDK and without Open CUDA model packs.

Expected:

- GUI uses daemon-reported availability.
- Unsupported effect controls are disabled or marked unavailable.
- User-visible reason/hints explain missing runtime or models.
- Saved effect intent is not silently discarded.

- [ ] If Open CUDA model packs are installed, select Open CUDA and enable a
  supported background effect.

Expected:

- GUI lists daemon-reported installed models.
- Missing models are reported as missing, not guessed client-side.
- Pipeline starts with the selected effect when a consumer is present.

- [ ] If Maxine SDK is installed, select Maxine and enable supported effects.

Expected:

- GUI reflects daemon-reported Maxine support.
- Blocked effects show accurate reasons.
- Unsupported controls cannot create a mismatched GUI/backend state.

- [ ] Change effects while the pipeline is running.

Expected:

- Updates are applied atomically enough that GUI state, daemon config, and
  resulting video output stay consistent.
- Failure leaves the previous working daemon state intact or clearly reports
  what changed.

## Error And Recovery Cases

- [ ] Start GUI and daemon, then unplug the physical camera during streaming.

Expected:

- User-visible error is actionable.
- Daemon and GUI remain responsive.
- Reconnecting or selecting another input recovers without app restart.

- [ ] Remove the v4l2loopback module or make the output device unavailable.

Expected:

- Virtual device missing/unavailable state is visible.
- Start fails without claiming success.
- Reloading v4l2loopback and refreshing allows recovery.

- [ ] Run with insufficient device permissions.

Expected:

- Permission errors are surfaced as permission problems.
- Suggested fix is understandable to a non-developer tester.

- [ ] Generate a support bundle after a failure:

```bash
build/studiocastctl debug-report --out studiocast-debug-report.txt
```

Expected:

- Debug report includes enough device, daemon, status, and config information
  to diagnose the failure.

## Pass Criteria For Production Readiness

- No GUI hangs during daemon unreachable, stale socket, daemon restart, or
  malformed/failing command paths.
- No heavy video processing when no consumer is present and preview is off.
- Preview acts as an explicit consumer only when enabled by the user.
- Input/output device conflicts are prevented.
- Status text accurately reflects daemon state and is useful to non-developers.
- Persisted config matches GUI state after GUI and daemon restart.
- Effects availability comes from daemon diagnostics.
- Recoverable failures can be recovered from the UI without restarting the app.
- Support artifacts are sufficient to diagnose failures.

## Known Manual Gaps

- Exclusive-caps state transitions need validation on real v4l2loopback devices.
- Browser/WebRTC behavior may differ from OBS and should be tested separately.
- GPU effect availability needs separate passes for no GPU, unsupported GPU,
  Open CUDA installed, and Maxine installed.
- Long-running stability and thermal/performance behavior need a separate soak
  plan.
