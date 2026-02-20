#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/video/v4l2_writer.h"

namespace studiocast::video {

struct FrameLayout {
  int width = 0;
  int height = 0;
  PixelFormat format = PixelFormat::yuyv;
  std::size_t bytes_per_line = 0;
  std::size_t size_image = 0;
};

// Writes a moving color-bar test pattern into dst (row-stride aware).
bool FillMovingColorBars(std::uint8_t *dst, std::size_t dst_size,
                         const FrameLayout &layout, int frame_index,
                         std::string *error);

} // namespace studiocast::video
