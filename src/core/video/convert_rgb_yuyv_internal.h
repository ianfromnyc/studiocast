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

// Every RGB24 -> YUYV converter below writes ceil(width / 2) * 4 bytes into
// each destination row: an odd width still fills the whole final YUYV pair,
// where the second luma slot repeats the last pixel. So dst_stride must be at
// least ceil(width / 2) * 4, which is width * 2 rounded up to the next pair.
// The source rows need width * 3 bytes.

std::size_t Rgb24ToYuyvDispatchScratchBytes(int width, int height);
// Checks the shared preconditions once, for whichever backend runs, and
// returns false without writing when they do not hold: a null buffer, a width
// or height that is not positive, or a dst_stride under the row size above.
// The scratch buffer is the one recoverable input: a backend that cannot use
// it converts with the scalar path instead.
bool Rgb24ToYuyvDispatchWithScratch(const std::uint8_t *src, int width,
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
// Returns false, and writes nothing, when the build has no libyuv or when the
// scratch buffer is too small. The caller then falls back to another
// converter. It also returns false for a dst_stride under the row size above,
// which is a caller error rather than a recoverable one: the dispatch entry
// point rejects that input before any backend sees it, so this is only a
// guard for a direct call.
bool Rgb24ToYuyvLibyuv(const std::uint8_t *src, int width, int height,
                       std::size_t src_stride, std::uint8_t *dst,
                       std::size_t dst_stride, std::uint8_t *scratch,
                       std::size_t scratch_size);

} // namespace studiocast::video::internal
