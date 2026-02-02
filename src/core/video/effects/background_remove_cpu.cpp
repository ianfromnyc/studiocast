#include "background_remove_cpu.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace studiocast::video::effects {

void BackgroundRemoveCpuEffect::Apply(const Rgb24FrameView& frame, EffectContext* /*ctx*/) {
  if (!frame.Valid()) return;

  const int w = frame.width;
  const int h = frame.height;
  const std::size_t stride = frame.stride_bytes;

  // Center-focus mask parameters.
  const int left = static_cast<int>(w * 0.25);
  const int right = static_cast<int>(w * 0.75);
  const int top = static_cast<int>(h * 0.15);
  const int bottom = static_cast<int>(h * 0.95);
  const int feather = std::max(8, std::min(w, h) / 20);

  // "Chroma key" green background (RGB).
  const int bgR = 0;
  const int bgG = 255;
  const int bgB = 0;

  for (int y = 0; y < h; ++y) {
    std::uint8_t* row = frame.data + static_cast<std::size_t>(y) * stride;

    int dy = 0;
    if (y < top) dy = top - y;
    else if (y > bottom) dy = y - bottom;

    for (int x = 0; x < w; ++x) {
      int dx = 0;
      if (x < left) dx = left - x;
      else if (x > right) dx = x - right;

      const int d = std::max(dx, dy);
      if (d <= 0) continue;

      std::uint8_t* p = row + static_cast<std::size_t>(x) * 3u;

      if (d >= feather) {
        p[0] = static_cast<std::uint8_t>(bgR);
        p[1] = static_cast<std::uint8_t>(bgG);
        p[2] = static_cast<std::uint8_t>(bgB);
      } else {
        // Blend toward background across the feather edge.
        const int a = d;
        const int ia = feather - d;

        p[0] = static_cast<std::uint8_t>((static_cast<int>(p[0]) * ia + bgR * a) / feather);
        p[1] = static_cast<std::uint8_t>((static_cast<int>(p[1]) * ia + bgG * a) / feather);
        p[2] = static_cast<std::uint8_t>((static_cast<int>(p[2]) * ia + bgB * a) / feather);
      }
    }
  }
}

}  // namespace studiocast::video::effects
