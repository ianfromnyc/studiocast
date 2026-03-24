# Open Video shared inference contract

StudioCast’s open-source video effects can require **multiple ML inferences per frame**:

- segmentation / matting (for virtual background, auto-frame fallback)
- face detection (for auto frame, key light weighting, eye contact)
- face landmarks (for eye contact)
- video denoise (for video noise removal)

Running the same model twice in a single frame is wasted compute and increases latency. We don't want that.

## Design goal

**One inference per task per frame**, shared across effects in the pipeline.

StudioCast achieves this by keeping a small per-frame cache keyed by a monotonically increasing
`capture_sequence` id.

## Current implementation

Phase M6 introduces a lightweight contract:

- `src/core/open_video/frame_analysis_cache.h`

It provides:

- `FrameAnalysisCache::BeginFrame(seq)` to clear stale data
- cached slots for:
  - `face_detections`
  - `face_landmarks`

Future phases will extend this contract with additional cached artifacts (e.g. eye-crop tensors
or denoise frame history) and wire it into the video pipeline.

## Expected usage pattern

At the start of a camera frame:

```cpp
cache.BeginFrame(capture_sequence);
```

Then each effect can do:

```cpp
if (!cache.face_detections) {
  cache.face_detections = RunFaceDetector(...);
}
// Use cache.face_detections
```

## Notes

- Caching is **best-effort**: if a model is missing, too slow, or fails, the cache entry can remain empty.
- Effects must always implement a safe fallback path.
