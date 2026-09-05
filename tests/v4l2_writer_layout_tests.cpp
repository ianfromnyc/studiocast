#include "core/video/v4l2_writer.h"

#include <cstddef>
#include <iostream>

namespace studiocast::tests {
namespace {

bool Expect(bool condition, const char *message) {
  if (!condition)
    std::cerr << message << "\n";
  return condition;
}

struct RowSizeCase {
  int width;
  std::size_t yuyv;
  std::size_t rgb24;
};

} // namespace

// The row size the writer asks the driver for is the size the RGB24 to YUYV
// converters write, and the pipeline sizes its output buffer from it. A YUYV
// row packs pixels in pairs, so an odd width still fills the whole final pair:
// the row is ceil(width / 2) * 4 bytes. Two bytes less per row, and the last
// row of the frame runs past the buffer.
bool TestV4l2WriterRowSizeHoldsTheOddWidthYuyvPair() {
  using video::MinBytesPerLine;
  using video::PixelFormat;

  const RowSizeCase cases[] = {
      {1, 4u, 3u},    {2, 4u, 6u},    {3, 8u, 9u},         {7, 16u, 21u},
      {16, 32u, 48u}, {17, 36u, 51u}, {640, 1280u, 1920u},
  };

  for (const RowSizeCase &c : cases) {
    const std::size_t yuyv = MinBytesPerLine(c.width, PixelFormat::yuyv);
    if (!Expect(yuyv == c.yuyv,
                "YUYV row size must hold the whole final pixel pair")) {
      std::cerr << "  width " << c.width << ": expected " << c.yuyv << ", got "
                << yuyv << "\n";
      return false;
    }

    const std::size_t rgb24 = MinBytesPerLine(c.width, PixelFormat::rgb24);
    if (!Expect(rgb24 == c.rgb24,
                "RGB24 row size must be three bytes per pixel")) {
      std::cerr << "  width " << c.width << ": expected " << c.rgb24 << ", got "
                << rgb24 << "\n";
      return false;
    }
  }

  return true;
}

} // namespace studiocast::tests
