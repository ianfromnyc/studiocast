#pragma once

#include <cstddef>
#include <cstdint>

namespace studiocast::video {

  // Convert a YUYV frame to RGB24.
  // - src_stride: bytes per line in the YUYV buffer (often width*2, but can be larger)
  // - dst_stride: bytes per line in the RGB buffer (often width*3, but can be larger)
  void YuyvToRgb24(const std::uint8_t* src,
                   int width,
                   int height,
                   std::size_t src_stride,
                   std::uint8_t* dst,
                   std::size_t dst_stride);

  // Convert RGB24 to YUYV.
  // - src_stride: bytes per line in the RGB buffer
  // - dst_stride: bytes per line in the YUYV buffer
  void Rgb24ToYuyv(const std::uint8_t* src,
                   int width,
                   int height,
                   std::size_t src_stride,
                   std::uint8_t* dst,
                   std::size_t dst_stride);

  // In-place horizontal mirror on RGB24 buffer.
  void MirrorRgb24InPlace(std::uint8_t* rgb,
                          int width,
                          int height,
                          std::size_t stride);

  // Swap RGB<->BGR channel order.
  // The conversion is symmetric, so the same implementation works both ways.
  void Rgb24ToBgr24(const std::uint8_t* src,
                    std::uint8_t* dst,
                    int width,
                    int height,
                    std::size_t src_stride,
                    std::size_t dst_stride);

  inline void Bgr24ToRgb24(const std::uint8_t* src,
                           std::uint8_t* dst,
                           int width,
                           int height,
                           std::size_t src_stride,
                           std::size_t dst_stride) {
    Rgb24ToBgr24(src, dst, width, height, src_stride, dst_stride);
  }

}  // namespace studiocast::video
