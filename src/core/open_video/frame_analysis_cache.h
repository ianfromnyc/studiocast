#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace studiocast::open_video {

// Shared analysis artifacts computed once per capture frame.
//
// Many video effects need the same underlying signal (foreground matte, face boxes,
// landmarks). StudioCast avoids re-running expensive inference multiple times per
// frame by caching analysis outputs keyed by capture_sequence.
//
// Phase M6: This is a lightweight contract used by upcoming Open Video tasks.
struct FaceDetection {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
  float score = 0.0f;
};

struct FaceLandmarks {
  // Normalized pixel coordinates in the input frame.
  //  - x,y are in pixel space (0..width-1 / 0..height-1).
  std::vector<std::pair<float, float>> points;
};

struct FrameAnalysisCache {
  std::uint64_t capture_sequence = 0;

  // Best-effort "begin frame" helper; clears cached results when the sequence changes.
  void BeginFrame(std::uint64_t seq) {
    if (seq == capture_sequence) return;
    capture_sequence = seq;
    Clear();
  }

  void Clear() {
    face_detections.reset();
    face_landmarks.reset();
  }

  std::optional<std::vector<FaceDetection>> face_detections;
  std::optional<FaceLandmarks> face_landmarks;
};

}  // namespace studiocast::open_video
