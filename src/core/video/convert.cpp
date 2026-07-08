#include "convert.h"

#include "convert_rgb_yuyv_internal.h"

#include <cstddef>
#include <cstdint>

namespace studiocast::video {
namespace {

inline std::uint8_t ClampByte(int v) {
  if (v < 0)
    return 0;
  if (v > 255)
    return 255;
  return static_cast<std::uint8_t>(v);
}

// BT.601 limited-range YUV -> RGB conversion.
inline void YuvToRgb(int y, int u, int v, int *outR, int *outG, int *outB) {
  // y: [16..235], u/v: [16..240] typically
  int c = y - 16;
  int d = u - 128;
  int e = v - 128;

  if (c < 0)
    c = 0;

  const int r = (298 * c + 409 * e + 128) >> 8;
  const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
  const int b = (298 * c + 516 * d + 128) >> 8;

  *outR = r;
  *outG = g;
  *outB = b;
}

} // namespace

void YuyvToRgb24(const std::uint8_t *src, int width, int height,
                 std::size_t src_stride, std::uint8_t *dst,
                 std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  for (int y = 0; y < height; ++y) {
    const std::uint8_t *s = src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *d = dst + static_cast<std::size_t>(y) * dst_stride;

    for (int x = 0; x < width; x += 2) {
      const int y0 = s[0];
      const int u = s[1];
      const int y1 = s[2];
      const int v = s[3];
      s += 4;

      int r0, g0, b0;
      int r1, g1, b1;
      YuvToRgb(y0, u, v, &r0, &g0, &b0);
      YuvToRgb(y1, u, v, &r1, &g1, &b1);

      d[0] = ClampByte(r0);
      d[1] = ClampByte(g0);
      d[2] = ClampByte(b0);
      d += 3;

      if (x + 1 < width) {
        d[0] = ClampByte(r1);
        d[1] = ClampByte(g1);
        d[2] = ClampByte(b1);
        d += 3;
      }
    }
  }
}

void Rgb24ToYuyv(const std::uint8_t *src, int width, int height,
                 std::size_t src_stride, std::uint8_t *dst,
                 std::size_t dst_stride) {
  Rgb24ToYuyvWithScratch(src, width, height, src_stride, dst, dst_stride,
                         nullptr, 0);
}

std::size_t Rgb24ToYuyvScratchBytes(int width, int height) {
  return internal::Rgb24ToYuyvDispatchScratchBytes(width, height);
}

void Rgb24ToYuyvWithScratch(const std::uint8_t *src, int width, int height,
                            std::size_t src_stride, std::uint8_t *dst,
                            std::size_t dst_stride, std::uint8_t *scratch,
                            std::size_t scratch_size) {
  internal::Rgb24ToYuyvDispatchWithScratch(src, width, height, src_stride, dst,
                                           dst_stride, scratch, scratch_size);
}

void MirrorRgb24InPlace(std::uint8_t *rgb, int width, int height,
                        std::size_t stride) {
  if (!rgb || width <= 0 || height <= 0)
    return;

  for (int y = 0; y < height; ++y) {
    std::uint8_t *row = rgb + static_cast<std::size_t>(y) * stride;

    for (int x = 0; x < width / 2; ++x) {
      const std::size_t li = static_cast<std::size_t>(x) * 3u;
      const std::size_t ri = static_cast<std::size_t>(width - 1 - x) * 3u;

      const std::uint8_t lr = row[li + 0];
      const std::uint8_t lg = row[li + 1];
      const std::uint8_t lb = row[li + 2];

      row[li + 0] = row[ri + 0];
      row[li + 1] = row[ri + 1];
      row[li + 2] = row[ri + 2];

      row[ri + 0] = lr;
      row[ri + 1] = lg;
      row[ri + 2] = lb;
    }
  }
}

void Rgb24ToBgr24(const std::uint8_t *src, std::uint8_t *dst, int width,
                  int height, std::size_t src_stride, std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  for (int y = 0; y < height; ++y) {
    const std::uint8_t *s = src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *d = dst + static_cast<std::size_t>(y) * dst_stride;

    for (int x = 0; x < width; ++x) {
      const std::size_t i = static_cast<std::size_t>(x) * 3u;
      d[i + 0] = s[i + 2];
      d[i + 1] = s[i + 1];
      d[i + 2] = s[i + 0];
    }
  }
}

} // namespace studiocast::video
