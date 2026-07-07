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

inline bool ShouldFallbackToRawAfterMjpegDecodeFailure(
    const CaptureFormat &capture, bool fallback_already_attempted) {
  return capture.format == CapturePixelFormat::mjpeg &&
         !fallback_already_attempted;
}

} // namespace studiocast::video
