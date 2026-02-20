#include "box_blur.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace studiocast::video::effects {
namespace {

inline int Clamp(int v, int lo, int hi) {
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}

} // namespace

void BoxBlurRgb24InPlace(const Rgb24FrameView &frame, int radius,
                         std::vector<std::uint8_t> *scratch) {
  if (!frame.Valid())
    return;
  if (radius <= 0)
    return;

  const int w = frame.width;
  const int h = frame.height;
  const std::size_t srcStride = frame.stride_bytes;
  const std::size_t tightStride = static_cast<std::size_t>(w) * 3u;
  const int win = radius * 2 + 1;

  // Intermediate buffer (horizontal pass) stored with tight stride.
  const std::size_t need = tightStride * static_cast<std::size_t>(h);
  if (!scratch)
    return;
  if (scratch->size() < need)
    scratch->assign(need, 0);

  std::uint8_t *tmp = scratch->data();

  // Horizontal pass: src -> tmp
  for (int y = 0; y < h; ++y) {
    const std::uint8_t *srcRow =
        frame.data + static_cast<std::size_t>(y) * srcStride;
    std::uint8_t *tmpRow = tmp + static_cast<std::size_t>(y) * tightStride;

    int sumR = 0, sumG = 0, sumB = 0;

    // Initial window centered at x=0 with edge clamping.
    for (int dx = -radius; dx <= radius; ++dx) {
      const int sx = Clamp(dx, 0, w - 1);
      const std::uint8_t *p = srcRow + static_cast<std::size_t>(sx) * 3u;
      sumR += p[0];
      sumG += p[1];
      sumB += p[2];
    }

    // x=0
    tmpRow[0] = static_cast<std::uint8_t>(sumR / win);
    tmpRow[1] = static_cast<std::uint8_t>(sumG / win);
    tmpRow[2] = static_cast<std::uint8_t>(sumB / win);

    for (int x = 1; x < w; ++x) {
      const int addX = Clamp(x + radius, 0, w - 1);
      const int remX = Clamp(x - radius - 1, 0, w - 1);

      const std::uint8_t *pAdd = srcRow + static_cast<std::size_t>(addX) * 3u;
      const std::uint8_t *pRem = srcRow + static_cast<std::size_t>(remX) * 3u;

      sumR += pAdd[0] - pRem[0];
      sumG += pAdd[1] - pRem[1];
      sumB += pAdd[2] - pRem[2];

      std::uint8_t *d = tmpRow + static_cast<std::size_t>(x) * 3u;
      d[0] = static_cast<std::uint8_t>(sumR / win);
      d[1] = static_cast<std::uint8_t>(sumG / win);
      d[2] = static_cast<std::uint8_t>(sumB / win);
    }
  }

  // Vertical pass: tmp -> dst (in-place into frame)
  for (int x = 0; x < w; ++x) {
    int sumR = 0, sumG = 0, sumB = 0;

    // Initial window at y=0.
    for (int dy = -radius; dy <= radius; ++dy) {
      const int sy = Clamp(dy, 0, h - 1);
      const std::uint8_t *p = tmp + static_cast<std::size_t>(sy) * tightStride +
                              static_cast<std::size_t>(x) * 3u;
      sumR += p[0];
      sumG += p[1];
      sumB += p[2];
    }

    // y=0
    {
      std::uint8_t *dstRow = frame.data;
      std::uint8_t *d = dstRow + static_cast<std::size_t>(x) * 3u;
      d[0] = static_cast<std::uint8_t>(sumR / win);
      d[1] = static_cast<std::uint8_t>(sumG / win);
      d[2] = static_cast<std::uint8_t>(sumB / win);
    }

    for (int y = 1; y < h; ++y) {
      const int addY = Clamp(y + radius, 0, h - 1);
      const int remY = Clamp(y - radius - 1, 0, h - 1);

      const std::uint8_t *pAdd = tmp +
                                 static_cast<std::size_t>(addY) * tightStride +
                                 static_cast<std::size_t>(x) * 3u;
      const std::uint8_t *pRem = tmp +
                                 static_cast<std::size_t>(remY) * tightStride +
                                 static_cast<std::size_t>(x) * 3u;

      sumR += pAdd[0] - pRem[0];
      sumG += pAdd[1] - pRem[1];
      sumB += pAdd[2] - pRem[2];

      std::uint8_t *dstRow =
          frame.data + static_cast<std::size_t>(y) * srcStride;
      std::uint8_t *d = dstRow + static_cast<std::size_t>(x) * 3u;
      d[0] = static_cast<std::uint8_t>(sumR / win);
      d[1] = static_cast<std::uint8_t>(sumG / win);
      d[2] = static_cast<std::uint8_t>(sumB / win);
    }
  }
}

} // namespace studiocast::video::effects
