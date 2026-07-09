#include "convert_yuyv_rgb_internal.h"

#include <cstddef>
#include <cstdint>

namespace studiocast::video::internal {
namespace {

struct YuyvToRgbDispatchPlan {
  YuyvToRgbBackend backend;
  YuyvToRgbFn convert;
};

YuyvToRgbDispatchPlan DetectDispatchPlan() {
#if STUDIOCAST_HAVE_X86_SIMD
  if (YuyvToRgbAvx2Available())
    return {YuyvToRgbBackend::avx2, &YuyvToRgbAvx2};
  if (YuyvToRgbSse41Available())
    return {YuyvToRgbBackend::sse41, &YuyvToRgbSse41};
#endif
  return {YuyvToRgbBackend::scalar, &YuyvToRgbScalar};
}

const YuyvToRgbDispatchPlan kDispatchPlan = DetectDispatchPlan();

} // namespace

const char *YuyvToRgbBackendName(YuyvToRgbBackend backend) {
  switch (backend) {
  case YuyvToRgbBackend::scalar:
    return "scalar";
  case YuyvToRgbBackend::sse41:
    return "sse4.1";
  case YuyvToRgbBackend::avx2:
    return "avx2";
  }
  return "unknown";
}

YuyvToRgbBackend YuyvToRgbSelectedBackend() { return kDispatchPlan.backend; }

bool YuyvToRgbBackendAvailable(YuyvToRgbBackend backend) {
  switch (backend) {
  case YuyvToRgbBackend::scalar:
    return true;
  case YuyvToRgbBackend::sse41:
    return YuyvToRgbSse41Available();
  case YuyvToRgbBackend::avx2:
    return YuyvToRgbAvx2Available();
  }
  return false;
}

void YuyvToRgbDispatch(const std::uint8_t *src, int width, int height,
                       std::size_t src_stride, std::uint8_t *dst,
                       std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;
  kDispatchPlan.convert(src, width, height, src_stride, dst, dst_stride);
}

#if STUDIOCAST_HAVE_X86_SIMD
bool YuyvToRgbSse41Available() {
#if defined(__GNUC__) || defined(__clang__)
  static const bool available = [] {
    __builtin_cpu_init();
    return static_cast<bool>(__builtin_cpu_supports("sse4.1"));
  }();
  return available;
#else
  return false;
#endif
}

bool YuyvToRgbAvx2Available() {
#if defined(__GNUC__) || defined(__clang__)
  static const bool available = [] {
    __builtin_cpu_init();
    return static_cast<bool>(__builtin_cpu_supports("avx")) &&
           static_cast<bool>(__builtin_cpu_supports("avx2"));
  }();
  return available;
#else
  return false;
#endif
}
#else
bool YuyvToRgbSse41Available() { return false; }

void YuyvToRgbSse41(const std::uint8_t *src, int width, int height,
                    std::size_t src_stride, std::uint8_t *dst,
                    std::size_t dst_stride) {
  YuyvToRgbScalar(src, width, height, src_stride, dst, dst_stride);
}

bool YuyvToRgbAvx2Available() { return false; }

void YuyvToRgbAvx2(const std::uint8_t *src, int width, int height,
                   std::size_t src_stride, std::uint8_t *dst,
                   std::size_t dst_stride) {
  YuyvToRgbScalar(src, width, height, src_stride, dst, dst_stride);
}
#endif

} // namespace studiocast::video::internal
