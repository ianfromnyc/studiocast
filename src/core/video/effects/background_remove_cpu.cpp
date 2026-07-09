#include "background_remove_cpu.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace studiocast::video::effects {

void BackgroundRemoveCpuEffect::Apply(const Rgb24FrameView &frame,
                                      EffectContext * /*ctx*/) {
  if (!frame.Valid())
    return;
  if (!mask_plan_.Configure(frame.width, frame.height))
    return;

  const int w = frame.width;
  const int h = frame.height;
  const std::size_t stride = frame.stride_bytes;

  const auto &xDistances = mask_plan_.x_distances();
  const auto &yDistances = mask_plan_.y_distances();
  const int feather = mask_plan_.feather();
  const int focusLeft = std::clamp(mask_plan_.focus_left(), 0, w);
  const int focusRightExclusive =
      std::clamp(mask_plan_.focus_right() + 1, 0, w);

  // "Chroma key" green background (RGB).
  const int bgR = 0;
  const int bgG = 255;
  const int bgB = 0;

  for (int y = 0; y < h; ++y) {
    std::uint8_t *row = frame.data + static_cast<std::size_t>(y) * stride;
    const int yDistance = yDistances[static_cast<std::size_t>(y)];

    if (yDistance >= feather) {
      for (int x = 0; x < w; ++x) {
        std::uint8_t *p = row + static_cast<std::size_t>(x) * 3u;
        p[0] = static_cast<std::uint8_t>(bgR);
        p[1] = static_cast<std::uint8_t>(bgG);
        p[2] = static_cast<std::uint8_t>(bgB);
      }
      continue;
    }

    if (yDistance > 0) {
      for (int x = 0; x < w; ++x) {
        const int d =
            std::max(xDistances[static_cast<std::size_t>(x)], yDistance);
        auto *p = row + static_cast<std::size_t>(x) * 3u;
        if (d >= feather) {
          p[0] = static_cast<std::uint8_t>(bgR);
          p[1] = static_cast<std::uint8_t>(bgG);
          p[2] = static_cast<std::uint8_t>(bgB);
        } else {
          const int a = d;
          const int ia = feather - d;
          p[0] = static_cast<std::uint8_t>(
              (static_cast<int>(p[0]) * ia + bgR * a) / feather);
          p[1] = static_cast<std::uint8_t>(
              (static_cast<int>(p[1]) * ia + bgG * a) / feather);
          p[2] = static_cast<std::uint8_t>(
              (static_cast<int>(p[2]) * ia + bgB * a) / feather);
        }
      }
      continue;
    }

    for (int x = 0; x < focusLeft; ++x) {
      const int d = xDistances[static_cast<std::size_t>(x)];
      auto *p = row + static_cast<std::size_t>(x) * 3u;
      if (d >= feather) {
        p[0] = static_cast<std::uint8_t>(bgR);
        p[1] = static_cast<std::uint8_t>(bgG);
        p[2] = static_cast<std::uint8_t>(bgB);
      } else {
        const int a = d;
        const int ia = feather - d;
        p[0] = static_cast<std::uint8_t>(
            (static_cast<int>(p[0]) * ia + bgR * a) / feather);
        p[1] = static_cast<std::uint8_t>(
            (static_cast<int>(p[1]) * ia + bgG * a) / feather);
        p[2] = static_cast<std::uint8_t>(
            (static_cast<int>(p[2]) * ia + bgB * a) / feather);
      }
    }
    for (int x = focusRightExclusive; x < w; ++x) {
      const int d = xDistances[static_cast<std::size_t>(x)];
      auto *p = row + static_cast<std::size_t>(x) * 3u;
      if (d >= feather) {
        p[0] = static_cast<std::uint8_t>(bgR);
        p[1] = static_cast<std::uint8_t>(bgG);
        p[2] = static_cast<std::uint8_t>(bgB);
      } else {
        const int a = d;
        const int ia = feather - d;
        p[0] = static_cast<std::uint8_t>(
            (static_cast<int>(p[0]) * ia + bgR * a) / feather);
        p[1] = static_cast<std::uint8_t>(
            (static_cast<int>(p[1]) * ia + bgG * a) / feather);
        p[2] = static_cast<std::uint8_t>(
            (static_cast<int>(p[2]) * ia + bgB * a) / feather);
      }
    }
  }
}

} // namespace studiocast::video::effects
