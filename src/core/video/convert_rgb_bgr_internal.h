#pragma once

#include <cstddef>
#include <cstdint>

namespace studiocast::video::internal {

enum class Rgb24Bgr24Backend {
  scalar,
  ssse3,
};

using Rgb24Bgr24Fn = void (*)(const std::uint8_t *src, std::uint8_t *dst,
                              int width, int height,
                              std::size_t src_stride,
                              std::size_t dst_stride);

const char *Rgb24Bgr24BackendName(Rgb24Bgr24Backend backend);
Rgb24Bgr24Backend Rgb24Bgr24SelectedBackend();
bool Rgb24Bgr24BackendAvailable(Rgb24Bgr24Backend backend);

void Rgb24Bgr24Dispatch(const std::uint8_t *src, std::uint8_t *dst, int width,
                        int height, std::size_t src_stride,
                        std::size_t dst_stride);

void Rgb24Bgr24Scalar(const std::uint8_t *src, std::uint8_t *dst, int width,
                      int height, std::size_t src_stride,
                      std::size_t dst_stride);

bool Rgb24Bgr24Ssse3Available();
void Rgb24Bgr24Ssse3(const std::uint8_t *src, std::uint8_t *dst, int width,
                     int height, std::size_t src_stride,
                     std::size_t dst_stride);

} // namespace studiocast::video::internal
