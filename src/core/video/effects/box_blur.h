#pragma once

#include <vector>

#include "core/video/effects/effect.h"

namespace studiocast::video::effects {

// Fast-ish separable box blur for RGB24.
//
// - Operates in-place on `frame`.
// - Uses `scratch` for intermediate storage to avoid per-frame allocations.
// - Radius of 0 is a no-op.
void BoxBlurRgb24InPlace(const Rgb24FrameView &frame, int radius,
                         std::vector<std::uint8_t> *scratch);

} // namespace studiocast::video::effects
