#include "convert_rgb_yuyv_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace studiocast::video::internal {
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

inline int RgbToU(int r, int g, int b) {
  return ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
}

inline int RgbToV(int r, int g, int b) {
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

} // namespace

void Rgb24ToYuyvScalar(const std::uint8_t *src, int width, int height,
                       std::size_t src_stride, std::uint8_t *dst,
                       std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

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

} // namespace studiocast::video::internal
