#include "pattern.h"

#include <algorithm>
#include <array>

namespace studiocast::video {
namespace {

struct Rgb {
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
};

std::uint8_t ClampU8(int v) {
  if (v < 0)
    return 0;
  if (v > 255)
    return 255;
  return static_cast<std::uint8_t>(v);
}

void RgbToYuv(const Rgb &c, std::uint8_t *y, std::uint8_t *u, std::uint8_t *v) {
  // Integer approx of BT.601 full range.
  // Y  =  0.299R + 0.587G + 0.114B
  // U' = -0.169R - 0.331G + 0.5B + 128
  // V' =  0.5R   - 0.419G - 0.081B + 128
  const int R = static_cast<int>(c.r);
  const int G = static_cast<int>(c.g);
  const int B = static_cast<int>(c.b);

  const int Y = (77 * R + 150 * G + 29 * B) >> 8;
  const int U = ((-43 * R - 85 * G + 128 * B) >> 8) + 128;
  const int V = ((128 * R - 107 * G - 21 * B) >> 8) + 128;

  *y = ClampU8(Y);
  *u = ClampU8(U);
  *v = ClampU8(V);
}

std::size_t MinBytesPerLine(int width, PixelFormat fmt) {
  const auto w = static_cast<std::size_t>(width);
  switch (fmt) {
  case PixelFormat::yuyv:
    return w * 2u;
  case PixelFormat::rgb24:
    return w * 3u;
  }
  return w * 2u;
}

} // namespace

bool FillMovingColorBars(std::uint8_t *dst, std::size_t dst_size,
                         const FrameLayout &layout, int frame_index,
                         std::string *error) {
  if (!dst) {
    if (error)
      *error = "dst is null";
    return false;
  }
  if (layout.width <= 0 || layout.height <= 0) {
    if (error)
      *error = "invalid layout width/height";
    return false;
  }

  const std::size_t w = static_cast<std::size_t>(layout.width);
  const std::size_t h = static_cast<std::size_t>(layout.height);

  const std::size_t minBpl = MinBytesPerLine(layout.width, layout.format);
  if (layout.bytes_per_line < minBpl) {
    if (error)
      *error = "layout.bytes_per_line is smaller than minimum for format";
    return false;
  }

  const std::size_t required = layout.bytes_per_line * h;
  if (dst_size < required) {
    if (error)
      *error = "dst buffer too small for bytes_per_line*height";
    return false;
  }

  // Clear whole frame (including padding) so padding bytes are deterministic.
  std::fill(dst, dst + required, static_cast<std::uint8_t>(0));

  constexpr std::array<Rgb, 8> kBars = {{
      {255, 255, 255}, // white
      {255, 255, 0},   // yellow
      {0, 255, 255},   // cyan
      {0, 255, 0},     // green
      {255, 0, 255},   // magenta
      {255, 0, 0},     // red
      {0, 0, 255},     // blue
      {0, 0, 0},       // black
  }};

  const std::size_t shift =
      (w == 0) ? 0u : ((static_cast<std::size_t>(frame_index) * 4u) % w);

  if (layout.format == PixelFormat::rgb24) {
    for (std::size_t y = 0; y < h; ++y) {
      std::uint8_t *row = dst + y * layout.bytes_per_line;
      for (std::size_t x = 0; x < w; ++x) {
        const std::size_t xs = (x + shift) % w;
        std::size_t bar = (xs * 8u) / w;
        if (bar > 7u)
          bar = 7u;

        const Rgb c = kBars[bar];
        const std::size_t o = x * 3u;
        row[o + 0u] = c.r;
        row[o + 1u] = c.g;
        row[o + 2u] = c.b;
      }
    }
    return true;
  }

  // YUYV requires even width because pixels are packed in pairs.
  if ((layout.width % 2) != 0) {
    if (error)
      *error = "YUYV requires an even width";
    return false;
  }

  for (std::size_t y = 0; y < h; ++y) {
    std::uint8_t *row = dst + y * layout.bytes_per_line;

    for (std::size_t x = 0; x < w; x += 2u) {
      const std::size_t xs0 = (x + shift) % w;
      const std::size_t xs1 = ((x + 1u) + shift) % w;

      std::size_t b0 = (xs0 * 8u) / w;
      if (b0 > 7u)
        b0 = 7u;
      std::size_t b1 = (xs1 * 8u) / w;
      if (b1 > 7u)
        b1 = 7u;

      std::uint8_t y0 = 0, u0 = 128, v0 = 128;
      std::uint8_t y1 = 0, u1 = 128, v1 = 128;

      RgbToYuv(kBars[b0], &y0, &u0, &v0);
      RgbToYuv(kBars[b1], &y1, &u1, &v1);

      const int uAvg = (static_cast<int>(u0) + static_cast<int>(u1)) / 2;
      const int vAvg = (static_cast<int>(v0) + static_cast<int>(v1)) / 2;

      const std::size_t o = x * 2u; // 2 bytes per pixel, packed (Y0 U Y1 V)
      row[o + 0u] = y0;
      row[o + 1u] = ClampU8(uAvg);
      row[o + 2u] = y1;
      row[o + 3u] = ClampU8(vAvg);
    }
  }

  return true;
}

} // namespace studiocast::video
