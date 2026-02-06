#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace studiocast::video {

// Owning RGB24 frame buffer.
//
// Data is tightly packed with `stride_bytes == width * 3`.
struct Rgb24Frame {
  int width = 0;
  int height = 0;
  std::size_t stride_bytes = 0;
  std::vector<std::uint8_t> buf;

  void ResizeTight(int w, int h);

  std::uint8_t* data() { return buf.data(); }
  const std::uint8_t* data() const { return buf.data(); }
  std::size_t size() const { return buf.size(); }
};

// Decode an MJPEG/JPEG frame into tightly packed RGB24.
//
// Returns false on decode errors.
// Thread-safe: no shared global decoder state.
bool DecodeMjpegToRgb24(const std::uint8_t* mjpg, std::size_t len, Rgb24Frame& out, int& w, int& h);

}  // namespace studiocast::video
