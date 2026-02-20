#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace studiocast::video {

// Load an image file into tightly packed RGB24.
//
// Supported formats:
//  - .ppm (binary P6, maxval <= 255)
//  - .png (8-bit; alpha is composited onto black)
bool LoadImageRgb24(const std::filesystem::path &path, int *out_w, int *out_h,
                    std::vector<std::uint8_t> *out_rgb, std::string *error);

// Minimal image loader for Virtual Background Replace.
//
// Initial format support is intentionally small and dependency-free:
//   - PPM binary (P6) with maxval <= 255
//
// Output is tightly packed RGB24.
bool LoadPpmP6Rgb24(const std::filesystem::path &path, int *out_w, int *out_h,
                    std::vector<std::uint8_t> *out_rgb, std::string *error);

// Resize a tightly packed RGB24 buffer to a new size using bilinear filtering.
//
// - `src_stride` and `dst_stride` are in bytes.
// - `dst_rgb` is resized and overwritten.
bool ResizeRgb24Bilinear(const std::uint8_t *src_rgb, int src_w, int src_h,
                         std::size_t src_stride, int dst_w, int dst_h,
                         std::vector<std::uint8_t> *dst_rgb,
                         std::size_t dst_stride, std::string *error);

} // namespace studiocast::video
