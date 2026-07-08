#include "core/video/convert.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace studiocast::tests {
namespace {

inline int RgbToY(int r, int g, int b) {
  return ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
}

inline int RgbToU(int r, int g, int b) {
  return ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
}

inline int RgbToV(int r, int g, int b) {
  return ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

} // namespace

bool TestRgb24ToYuyvMatchesBt601WithinChromaRounding() {
  constexpr int width = 17;
  constexpr int height = 11;
  constexpr std::size_t src_stride = width * 3 + 5;
  constexpr std::size_t dst_stride = width * 2 + 8;

  std::vector<std::uint8_t> src(src_stride * height);
  std::vector<std::uint8_t> dst(dst_stride * height, 0xcd);

  for (std::size_t i = 0; i < src.size(); ++i)
    src[i] = static_cast<std::uint8_t>((i * 37 + (i / 5) * 17 + 91) & 0xff);

  video::Rgb24ToYuyv(src.data(), width, height, src_stride, dst.data(),
                     dst_stride);

  int max_chroma_delta = 0;
  for (int y = 0; y < height; ++y) {
    const std::uint8_t *s =
        src.data() + static_cast<std::size_t>(y) * src_stride;
    const std::uint8_t *d =
        dst.data() + static_cast<std::size_t>(y) * dst_stride;

    for (int x = 0; x < width; x += 2) {
      const int r0 = s[static_cast<std::size_t>(x) * 3u + 0];
      const int g0 = s[static_cast<std::size_t>(x) * 3u + 1];
      const int b0 = s[static_cast<std::size_t>(x) * 3u + 2];

      int r1 = r0;
      int g1 = g0;
      int b1 = b0;
      if (x + 1 < width) {
        r1 = s[static_cast<std::size_t>(x + 1) * 3u + 0];
        g1 = s[static_cast<std::size_t>(x + 1) * 3u + 1];
        b1 = s[static_cast<std::size_t>(x + 1) * 3u + 2];
      }

      const int expected_y0 = RgbToY(r0, g0, b0);
      const int expected_y1 = RgbToY(r1, g1, b1);
      const int expected_u =
          (RgbToU(r0, g0, b0) + RgbToU(r1, g1, b1)) / 2;
      const int expected_v =
          (RgbToV(r0, g0, b0) + RgbToV(r1, g1, b1)) / 2;

      const std::size_t out = static_cast<std::size_t>(x) * 2u;
      if (d[out + 0] != expected_y0 || d[out + 2] != expected_y1) {
        std::cerr << "YUYV luma mismatch at " << x << "," << y << "\n";
        return false;
      }

      max_chroma_delta =
          std::max(max_chroma_delta, std::abs(static_cast<int>(d[out + 1]) -
                                             expected_u));
      max_chroma_delta =
          std::max(max_chroma_delta, std::abs(static_cast<int>(d[out + 3]) -
                                             expected_v));
    }
  }

  if (max_chroma_delta > 1) {
    std::cerr << "YUYV chroma delta exceeded rounding tolerance: "
              << max_chroma_delta << "\n";
    return false;
  }

  return true;
}

} // namespace studiocast::tests
