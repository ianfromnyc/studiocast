#include "convert_rgb_bgr_internal.h"

#include <cstddef>
#include <cstdint>

namespace studiocast::video::internal {

void Rgb24Bgr24Scalar(const std::uint8_t *src, std::uint8_t *dst, int width,
                      int height, std::size_t src_stride,
                      std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  for (int y = 0; y < height; ++y) {
    const std::uint8_t *s = src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *d = dst + static_cast<std::size_t>(y) * dst_stride;

    for (int x = 0; x < width; ++x) {
      const std::size_t i = static_cast<std::size_t>(x) * 3u;
      const std::uint8_t r = s[i + 0];
      const std::uint8_t g = s[i + 1];
      const std::uint8_t b = s[i + 2];
      d[i + 0] = b;
      d[i + 1] = g;
      d[i + 2] = r;
    }
  }
}

} // namespace studiocast::video::internal
