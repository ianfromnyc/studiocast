# Changelog

## v0.2.7

This release focuses on live video performance, virtual camera format control,
and more resilient audio/video fallback behavior.

### Highlights

- Added virtual camera output format selection for `rgb24` and `yuyv` in the
  GUI, daemon config, daemon CLI, `studiocastctl video set`, and status output.
- Reworked hot RGB/YUYV/RGB-BGR conversion paths with runtime-selected scalar,
  optional `libyuv`, SSSE3, SSE4.1, and AVX2 backends.
- Improved CPU resize, RGB output prep, background-mask generation, and GPU
  scaler decisions to reduce unnecessary work on live frames.
- Made microphone source handling more explicit and robust, including safe
  auto-selection, unavailable-source reporting, and pass-through fallback when
  Maxine microphone effects fail.
- Expanded video, audio, daemon status, installer, and Maxine path test coverage
  around the new behavior.

### Added

- New persisted config key: `video.output_format`, defaulting to `rgb24`.
- New daemon option: `studiocastd --output-format rgb24|yuyv`.
- New CLI setting: `studiocastctl video set output_format=rgb24|yuyv`.
- GUI video setup control for `RGB24 / RGB3` and `YUYV 4:2:2` output.
- Status fields for requested output format, negotiated capture/output formats,
  capture fallback state, capture fallback reason, microphone source
  availability, source errors, and source warnings.
- Developer benchmark builds behind `STUDIOCAST_BUILD_BENCHMARKS=ON`, including
  RGB/YUYV/RGB-BGR conversion and CUDA transfer benchmark tools.

### Changed

- Virtual camera output now honors the requested FourCC instead of silently
  cycling through alternate output formats.
- V4L2 capture negotiation now treats 720p and larger YUYV requests as
  MJPEG-worthy when MJPEG preference is enabled.
- MJPEG capture decode failures can fall back once to raw YUYV and report that
  fallback through daemon/GUI status.
- Camera idle and preview status preserve configured input/output device names
  instead of hiding them while the heavy pipeline is stopped.
- Maxine SDK library discovery now gives explicit SDK roots priority over
  unrelated loader-path libraries, while still keeping system-loader fallback.
- `studiocastctl debug-report` now includes bounded PulseAudio snapshots for
  `pactl info`, defaults, sources, sinks, and loaded modules.

### Fixed

- Preserved configured but disconnected microphone sources and reported them as
  unavailable instead of silently changing user configuration.
- Avoided unsafe StudioCast/monitor sources during automatic microphone source
  selection.
- Fell back to pass-through audio with cooldown after Maxine microphone setup
  failures to avoid repeated restart churn.
- Skipped inactive standalone GPU scaler transfers when CPU resize is allowed
  and no same-backend GPU effect or deferred GPU output needs reuse.
- Reduced redundant per-frame work in CPU resize, padded RGB output prep, and
  CPU background removal.
- Tightened CUDA context validation for the retained primary context and avoids
  repeated validation after success.

### Performance

Local benchmark validation selected AVX2 for RGB/YUYV paths and SSSE3 for
RGB/BGR on the test machine. These numbers are local measurements, not fixed
runtime guarantees.

| Path | Local before -> after | Change |
|---|---:|---:|
| RGB24 -> YUYV selected dispatch | 11.047 ms -> 3.387 ms | 69.3% lower |
| YUYV -> RGB24 selected dispatch | 29.904 ms -> 11.286 ms | 62.3% lower |
| RGB24/BGR24 selected dispatch | 4.328 ms -> 1.351 ms | 68.8% lower |
| RGB24 resize planning, 1280x720 -> 1920x1080 | 129.998 ms -> 92.306 ms | 29.0% lower |
| RGB24 resize hot loop, 1280x720 -> 1920x1080 | 97.037 ms -> 45.092 ms | 53.5% lower |

### Compatibility Notes

- Existing configs that omit `video.output_format` continue to default to
  `rgb24`; invalid persisted values fall back to `rgb24`.
- Changing the virtual camera output format restarts the camera pipeline.
- Output negotiation is stricter about the requested format. Systems whose
  loopback device rejects `rgb24` may need to choose `yuyv` explicitly.
- Debug reports now include local PulseAudio device/module snapshots. Review
  reports before sharing them publicly.
- The top-level `VERSION` file still reads `0.2.6` on this branch; update it
  through the release/version-bump flow before tagging `v0.2.7`.

### Verification

- Focused CTest subset passed locally:
  `studiocast-audio-tests`, `studiocast-maxine-paths-tests`,
  `studiocast-daemon-status-tests`, `studiocast-installer-backend-tests`,
  `studiocast-video-tests`, and `studiocast-v4l2loopback-diagnostics-tests`.
- `./build/studiocast-probe --self-test` passed with `SELFTEST OK`.
- PR verification also included building `studiocast-video-tests`,
  `studiocastd`, `studiocastctl`, and `studiocast`, running
  `ctest --test-dir build -R '^studiocast-video-tests$' --output-on-failure`,
  and running `git diff --check`.
