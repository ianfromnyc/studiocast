#include "convert.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#ifndef STUDIOCAST_HAVE_LIBYUV
#define STUDIOCAST_HAVE_LIBYUV 0
#endif

#if STUDIOCAST_HAVE_LIBYUV
#include <libyuv/convert_argb.h>
#include <libyuv/convert_from_argb.h>
#endif

namespace studiocast::video {
namespace {

constexpr std::array<int, 256> MakeRgbTable(int coefficient) {
  std::array<int, 256> out{};
  for (int i = 0; i < 256; ++i)
    out[static_cast<std::size_t>(i)] = coefficient * i;
  return out;
}

constexpr std::array<int, 511> MakeRgbPairTable(int coefficient) {
  std::array<int, 511> out{};
  for (int i = 0; i < 511; ++i)
    out[static_cast<std::size_t>(i)] = coefficient * i;
  return out;
}

constexpr auto kYFromR = MakeRgbTable(66);
constexpr auto kYFromG = MakeRgbTable(129);
constexpr auto kYFromB = MakeRgbTable(25);
constexpr auto kUFromRPair = MakeRgbPairTable(-38);
constexpr auto kUFromGPair = MakeRgbPairTable(-74);
constexpr auto kUFromBPair = MakeRgbPairTable(112);
constexpr auto kVFromRPair = MakeRgbPairTable(112);
constexpr auto kVFromGPair = MakeRgbPairTable(-94);
constexpr auto kVFromBPair = MakeRgbPairTable(-18);

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

// BT.601 limited-range RGB -> YUV.
inline int RgbToY(int r, int g, int b) {
  // Y = (  66 R + 129 G +  25 B + 128) >> 8 + 16
  return ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
}
inline int RgbToU(int r, int g, int b) {
  // U = ( -38 R -  74 G + 112 B + 128) >> 8 + 128
  return ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
}
inline int RgbToV(int r, int g, int b) {
  // V = ( 112 R -  94 G -  18 B + 128) >> 8 + 128
  return ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

inline std::uint8_t RgbToYFast(int r, int g, int b) {
  return static_cast<std::uint8_t>(
      ((kYFromR[static_cast<std::size_t>(r)] +
        kYFromG[static_cast<std::size_t>(g)] +
        kYFromB[static_cast<std::size_t>(b)] + 128) >>
       8) +
      16);
}

inline std::uint8_t RgbPairSumToUFast(int r, int g, int b) {
  return static_cast<std::uint8_t>(
      ((kUFromRPair[static_cast<std::size_t>(r)] +
        kUFromGPair[static_cast<std::size_t>(g)] +
        kUFromBPair[static_cast<std::size_t>(b)] + 256) >>
       9) +
      128);
}

inline std::uint8_t RgbPairSumToVFast(int r, int g, int b) {
  return static_cast<std::uint8_t>(
      ((kVFromRPair[static_cast<std::size_t>(r)] +
        kVFromGPair[static_cast<std::size_t>(g)] +
        kVFromBPair[static_cast<std::size_t>(b)] + 256) >>
       9) +
      128);
}

#if STUDIOCAST_HAVE_LIBYUV
bool FitsInt(std::size_t value) {
  return value <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}
#endif

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
#if STUDIOCAST_HAVE_LIBYUV
  if (width <= 0 || height <= 0)
    return 0;
  return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
         4u;
#else
  (void)width;
  (void)height;
  return 0;
#endif
}

void Rgb24ToYuyvWithScratch(const std::uint8_t *src, int width, int height,
                            std::size_t src_stride, std::uint8_t *dst,
                            std::size_t dst_stride, std::uint8_t *scratch,
                            std::size_t scratch_size) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

#if STUDIOCAST_HAVE_LIBYUV
  const std::size_t argb_stride = static_cast<std::size_t>(width) * 4u;
  const std::size_t argb_bytes =
      argb_stride * static_cast<std::size_t>(height);
  if (scratch && scratch_size >= argb_bytes && FitsInt(src_stride) &&
      FitsInt(dst_stride) && FitsInt(argb_stride)) {
    const int src_stride_i = static_cast<int>(src_stride);
    const int dst_stride_i = static_cast<int>(dst_stride);
    const int argb_stride_i = static_cast<int>(argb_stride);
    if (libyuv::RAWToARGB(src, src_stride_i, scratch, argb_stride_i, width,
                          height) == 0 &&
        libyuv::ARGBToYUY2(scratch, argb_stride_i, dst, dst_stride_i, width,
                           height) == 0) {
      return;
    }
  }
#else
  (void)scratch;
  (void)scratch_size;
#endif

  for (int y = 0; y < height; ++y) {
    const std::uint8_t *s = src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *d = dst + static_cast<std::size_t>(y) * dst_stride;

    int x = 0;
    for (; x + 1 < width; x += 2) {
      const int r0 = s[0];
      const int g0 = s[1];
      const int b0 = s[2];
      const int r1 = s[3];
      const int g1 = s[4];
      const int b1 = s[5];
      s += 6;

      const int r_pair = r0 + r1;
      const int g_pair = g0 + g1;
      const int b_pair = b0 + b1;

      d[0] = RgbToYFast(r0, g0, b0);
      d[1] = RgbPairSumToUFast(r_pair, g_pair, b_pair);
      d[2] = RgbToYFast(r1, g1, b1);
      d[3] = RgbPairSumToVFast(r_pair, g_pair, b_pair);
      d += 4;
    }

    if (x < width) {
      const int r0 = s[0];
      const int g0 = s[1];
      const int b0 = s[2];
      const std::uint8_t y0 = RgbToYFast(r0, g0, b0);

      d[0] = y0;
      d[1] = static_cast<std::uint8_t>(RgbToU(r0, g0, b0));
      d[2] = y0;
      d[3] = static_cast<std::uint8_t>(RgbToV(r0, g0, b0));
    }
  }
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
