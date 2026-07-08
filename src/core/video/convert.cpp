#include "convert.h"

#include "convert_rgb_yuyv_internal.h"
#include "convert_yuyv_rgb_internal.h"

#include <cstddef>
#include <cstdint>

namespace studiocast::video {

void YuyvToRgb24(const std::uint8_t *src, int width, int height,
                 std::size_t src_stride, std::uint8_t *dst,
                 std::size_t dst_stride) {
  internal::YuyvToRgbDispatch(src, width, height, src_stride, dst, dst_stride);
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
