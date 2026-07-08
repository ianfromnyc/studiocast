#pragma once

#include <cstddef>
#include <cstdint>

namespace studiocast::video::internal {

enum class Rgb24ToYuyvBackend {
  scalar,
  libyuv,
  ssse3,
  avx2,
};

using Rgb24ToYuyvWithScratchFn = void (*)(const std::uint8_t *src, int width,
                                          int height, std::size_t src_stride,
                                          std::uint8_t *dst,
                                          std::size_t dst_stride,
                                          std::uint8_t *scratch,
                                          std::size_t scratch_size);

const char *Rgb24ToYuyvBackendName(Rgb24ToYuyvBackend backend);
Rgb24ToYuyvBackend Rgb24ToYuyvSelectedBackend();
bool Rgb24ToYuyvBackendAvailable(Rgb24ToYuyvBackend backend);

std::size_t Rgb24ToYuyvDispatchScratchBytes(int width, int height);
void Rgb24ToYuyvDispatchWithScratch(const std::uint8_t *src, int width,
                                    int height, std::size_t src_stride,
                                    std::uint8_t *dst, std::size_t dst_stride,
                                    std::uint8_t *scratch,
                                    std::size_t scratch_size);

void Rgb24ToYuyvScalar(const std::uint8_t *src, int width, int height,
                       std::size_t src_stride, std::uint8_t *dst,
                       std::size_t dst_stride);

bool Rgb24ToYuyvSsse3Available();
void Rgb24ToYuyvSsse3(const std::uint8_t *src, int width, int height,
                      std::size_t src_stride, std::uint8_t *dst,
                      std::size_t dst_stride);

bool Rgb24ToYuyvAvx2Available();
void Rgb24ToYuyvAvx2(const std::uint8_t *src, int width, int height,
                     std::size_t src_stride, std::uint8_t *dst,
                     std::size_t dst_stride);

bool Rgb24ToYuyvLibyuvAvailable();
std::size_t Rgb24ToYuyvLibyuvScratchBytes(int width, int height);
bool Rgb24ToYuyvLibyuv(const std::uint8_t *src, int width, int height,
                       std::size_t src_stride, std::uint8_t *dst,
                       std::size_t dst_stride, std::uint8_t *scratch,
                       std::size_t scratch_size);

} // namespace studiocast::video::internal
