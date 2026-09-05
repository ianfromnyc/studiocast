#include "convert_yuyv_rgb_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace studiocast::video::internal {
namespace {

inline std::uint8_t ClampByte(int v) {
  if (v < 0)
    return 0;
  if (v > 255)
    return 255;
  return static_cast<std::uint8_t>(v);
}

constexpr std::array<int, 256> MakeYuvYTable() {
  std::array<int, 256> out{};
  for (int i = 0; i < 256; ++i) {
    int c = i - 16;
    if (c < 0)
      c = 0;
    out[static_cast<std::size_t>(i)] = 298 * c;
  }
  return out;
}

constexpr std::array<int, 256> MakeCenteredTable(int coefficient) {
  std::array<int, 256> out{};
  for (int i = 0; i < 256; ++i)
    out[static_cast<std::size_t>(i)] = coefficient * (i - 128);
  return out;
}

constexpr auto kYuvY = MakeYuvYTable();
constexpr auto kYuvRFromV = MakeCenteredTable(409);
constexpr auto kYuvGFromU = MakeCenteredTable(-100);
constexpr auto kYuvGFromV = MakeCenteredTable(-208);
constexpr auto kYuvBFromU = MakeCenteredTable(516);

inline std::uint8_t ScaleAndClampYuv(int value) {
  return ClampByte((value + 128) >> 8);
}

} // namespace

void YuyvToRgbScalar(const std::uint8_t *src, int width, int height,
                     std::size_t src_stride, std::uint8_t *dst,
                     std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  for (int y = 0; y < height; ++y) {
    const std::uint8_t *s = src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *d = dst + static_cast<std::size_t>(y) * dst_stride;

    int x = 0;
    for (; x + 1 < width; x += 2) {
      const int y0 = s[0];
      const int u = s[1];
      const int y1 = s[2];
      const int v = s[3];
      s += 4;

      const int r_chroma = kYuvRFromV[static_cast<std::size_t>(v)];
      const int g_chroma = kYuvGFromU[static_cast<std::size_t>(u)] +
                           kYuvGFromV[static_cast<std::size_t>(v)];
      const int b_chroma = kYuvBFromU[static_cast<std::size_t>(u)];

      const int y_term0 = kYuvY[static_cast<std::size_t>(y0)];
      d[0] = ScaleAndClampYuv(y_term0 + r_chroma);
      d[1] = ScaleAndClampYuv(y_term0 + g_chroma);
      d[2] = ScaleAndClampYuv(y_term0 + b_chroma);

      const int y_term1 = kYuvY[static_cast<std::size_t>(y1)];
      d[3] = ScaleAndClampYuv(y_term1 + r_chroma);
      d[4] = ScaleAndClampYuv(y_term1 + g_chroma);
      d[5] = ScaleAndClampYuv(y_term1 + b_chroma);
      d += 6;
    }

    if (x < width) {
      // An odd width leaves the last pixel without its pair. A row padded to
      // the whole pair still carries that pixel's V byte, but a row the
      // driver packed to width * 2 bytes stops after the U byte, so reading
      // the V byte would walk past the row. Neutral chroma keeps the read
      // inside the row the caller gave.
      const std::size_t tail = static_cast<std::size_t>(x) * 2u;
      const int y0 = s[0];
      const int u = s[1];
      const int v = (tail + 4u <= src_stride) ? s[3] : 128;

      const int r_chroma = kYuvRFromV[static_cast<std::size_t>(v)];
      const int g_chroma = kYuvGFromU[static_cast<std::size_t>(u)] +
                           kYuvGFromV[static_cast<std::size_t>(v)];
      const int b_chroma = kYuvBFromU[static_cast<std::size_t>(u)];
      const int y_term0 = kYuvY[static_cast<std::size_t>(y0)];

      d[0] = ScaleAndClampYuv(y_term0 + r_chroma);
      d[1] = ScaleAndClampYuv(y_term0 + g_chroma);
      d[2] = ScaleAndClampYuv(y_term0 + b_chroma);
    }
  }
}

} // namespace studiocast::video::internal
