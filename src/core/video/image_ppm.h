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

class Rgb24BilinearResizePlan {
public:
  bool Configure(int src_w, int src_h, int dst_w, int dst_h,
                 std::string *error);
  void Clear();

  bool Apply(const std::uint8_t *src_rgb, std::size_t src_stride,
             std::vector<std::uint8_t> *dst_rgb, std::size_t dst_stride,
             std::string *error) const;

private:
  struct AxisSample {
    int i0 = 0;
    int i1 = 0;
    float f = 0.0f;
  };

  int src_w_ = 0;
  int src_h_ = 0;
  int dst_w_ = 0;
  int dst_h_ = 0;
  std::vector<AxisSample> x_samples_;
  std::vector<AxisSample> y_samples_;
};

// Resize a tightly packed RGB24 buffer to a new size using bilinear filtering.
//
// - `src_stride` and `dst_stride` are in bytes.
// - `dst_rgb` is resized and overwritten.
bool ResizeRgb24Bilinear(const std::uint8_t *src_rgb, int src_w, int src_h,
                         std::size_t src_stride, int dst_w, int dst_h,
                         std::vector<std::uint8_t> *dst_rgb,
                         std::size_t dst_stride, std::string *error);

} // namespace studiocast::video
