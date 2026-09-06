#pragma once

#include <string_view>

#include "core/video/v4l2_capture.h"

namespace studiocast::video {

// Centralized policy for deciding whether a capture failure should be treated
// as recoverable.
//
// This is kept header-only so it can be used both in runtime code (camera
// pipeline) and in `studiocast-probe --self-test` without pulling in
// device-dependent code.
inline bool IsRecoverableCaptureAcquireFailure(std::string_view err) {
  // A timeout simply means no frame arrived during the wait window; many
  // devices/drivers can occasionally do this transiently.
  if (err == "Timed out waiting for camera frame.")
    return true;

  // Empty string is reserved for “no detail available”; treat conservatively as
  // recoverable.
  if (err.empty())
    return true;

  return false;
}

// True when a failed acquire inside the frame-drain loop must stop the whole
// capture loop.
//
// The drain loop asks for a frame with no wait, so a recoverable failure there
// only says the queue is empty: the loop keeps the frame it has and goes on.
// Every other failure is a frame the driver gave and the capture refused, and
// the buffer stays dequeued. A loop that reads such a failure as an empty
// queue runs one buffer short from then on, with nothing in the log. The two
// callers of `AcquireFrame` must agree on what a fatal acquire means, and this
// is that agreement.
inline bool CaptureDrainFailureStopsCapture(std::string_view err) {
  return !IsRecoverableCaptureAcquireFailure(err);
}

inline bool ShouldFallbackToRawAfterMjpegDecodeFailure(
    const CaptureFormat &capture, bool fallback_already_attempted) {
  return capture.format == CapturePixelFormat::mjpeg &&
         !fallback_already_attempted;
}

} // namespace studiocast::video
