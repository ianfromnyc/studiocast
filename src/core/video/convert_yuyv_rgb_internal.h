#pragma once

#include <cstddef>
#include <cstdint>

namespace studiocast::video::internal {

enum class YuyvToRgbBackend {
  scalar,
  sse41,
  avx2,
};

// Every converter takes `src_stride` as the bytes each source row holds, not
// only as the step between rows. A captured YUYV row can be packed to
// width * 2 bytes, which for an odd width stops after the U byte of the last
// pixel. The converters read the V byte of that pixel only when the row is
// long enough to hold it, and use neutral chroma when it is not, so a packed
// row is never read past its end. A sub-row call must therefore pass the
// bytes that are left in the row, not the stride of the whole row.
using YuyvToRgbFn = void (*)(const std::uint8_t *src, int width, int height,
                             std::size_t src_stride, std::uint8_t *dst,
                             std::size_t dst_stride);

const char *YuyvToRgbBackendName(YuyvToRgbBackend backend);
YuyvToRgbBackend YuyvToRgbSelectedBackend();
bool YuyvToRgbBackendAvailable(YuyvToRgbBackend backend);

void YuyvToRgbDispatch(const std::uint8_t *src, int width, int height,
                       std::size_t src_stride, std::uint8_t *dst,
                       std::size_t dst_stride);

void YuyvToRgbScalar(const std::uint8_t *src, int width, int height,
                     std::size_t src_stride, std::uint8_t *dst,
                     std::size_t dst_stride);

bool YuyvToRgbSse41Available();
void YuyvToRgbSse41(const std::uint8_t *src, int width, int height,
                    std::size_t src_stride, std::uint8_t *dst,
                    std::size_t dst_stride);

bool YuyvToRgbAvx2Available();
void YuyvToRgbAvx2(const std::uint8_t *src, int width, int height,
                   std::size_t src_stride, std::uint8_t *dst,
                   std::size_t dst_stride);

} // namespace studiocast::video::internal
