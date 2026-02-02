#include "background_blur_cpu.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/video/effects/box_blur.h"

namespace studiocast::video::effects {
namespace {

inline int ClampInt(int v, int lo, int hi) {
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}

}  // namespace

BackgroundBlurCpuEffect::BackgroundBlurCpuEffect(int strength) {
  strength_ = ClampInt(strength, 1, 64);
}

void BackgroundBlurCpuEffect::Apply(const Rgb24FrameView& frame, EffectContext* /*ctx*/) {
  if (!frame.Valid()) return;

  const int w = frame.width;
  const int h = frame.height;
  const std::size_t srcStride = frame.stride_bytes;
  const std::size_t tightStride = static_cast<std::size_t>(w) * 3u;

  const std::size_t tightSize = tightStride * static_cast<std::size_t>(h);
  if (blurred_.size() != tightSize) blurred_.assign(tightSize, 0);

  // Copy source into tight buffer.
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* srcRow = frame.data + static_cast<std::size_t>(y) * srcStride;
    std::uint8_t* dstRow = blurred_.data() + static_cast<std::size_t>(y) * tightStride;
    std::memcpy(dstRow, srcRow, tightStride);
  }

  // Blur the tight copy in-place.
  Rgb24FrameView blurView;
  blurView.data = blurred_.data();
  blurView.width = w;
  blurView.height = h;
  blurView.stride_bytes = tightStride;

  BoxBlurRgb24InPlace(blurView, strength_, &scratch_);

  // Center-focus mask parameters.
  const int left = static_cast<int>(w * 0.25);
  const int right = static_cast<int>(w * 0.75);
  const int top = static_cast<int>(h * 0.15);
  const int bottom = static_cast<int>(h * 0.95);
  const int feather = std::max(8, std::min(w, h) / 20);  // ~5% of smaller dim

  // Composite blurred background onto the original.
  for (int y = 0; y < h; ++y) {
    std::uint8_t* outRow = frame.data + static_cast<std::size_t>(y) * srcStride;
    const std::uint8_t* blurRow = blurred_.data() + static_cast<std::size_t>(y) * tightStride;

    int dy = 0;
    if (y < top) dy = top - y;
    else if (y > bottom) dy = y - bottom;

    for (int x = 0; x < w; ++x) {
      int dx = 0;
      if (x < left) dx = left - x;
      else if (x > right) dx = x - right;

      const int d = std::max(dx, dy);
      const std::size_t off = static_cast<std::size_t>(x) * 3u;

      if (d <= 0) {
        // inside focus region: keep original
        continue;
      }

      const std::uint8_t* b = blurRow + off;
      std::uint8_t* o = outRow + off;

      if (d >= feather) {
        o[0] = b[0];
        o[1] = b[1];
        o[2] = b[2];
      } else {
        // Linear blend across the feather edge.
        const int a = d;               // 0..feather
        const int ia = feather - d;    // inverse
        o[0] = static_cast<std::uint8_t>((static_cast<int>(o[0]) * ia + static_cast<int>(b[0]) * a) / feather);
        o[1] = static_cast<std::uint8_t>((static_cast<int>(o[1]) * ia + static_cast<int>(b[1]) * a) / feather);
        o[2] = static_cast<std::uint8_t>((static_cast<int>(o[2]) * ia + static_cast<int>(b[2]) * a) / feather);
      }
    }
  }
}

}  // namespace studiocast::video::effects
