#pragma once

#include <cstddef>
#include <cstdint>

namespace studiocast::video::internal {

enum class YuyvToRgbBackend {
  scalar,
  sse41,
};

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

} // namespace studiocast::video::internal
