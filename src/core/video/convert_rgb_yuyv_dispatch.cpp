#include "convert_rgb_yuyv_internal.h"

#include <cstddef>
#include <cstdint>
#include <limits>

#ifndef STUDIOCAST_HAVE_LIBYUV
#define STUDIOCAST_HAVE_LIBYUV 0
#endif

#ifndef STUDIOCAST_HAVE_X86_SIMD
#define STUDIOCAST_HAVE_X86_SIMD 0
#endif

#if STUDIOCAST_HAVE_LIBYUV
#include <libyuv/convert_argb.h>
#include <libyuv/convert_from_argb.h>
#endif

namespace studiocast::video::internal {
namespace {

#if STUDIOCAST_HAVE_LIBYUV
bool FitsInt(std::size_t value) {
  return value <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}
#endif

struct Rgb24ToYuyvDispatchPlan {
  Rgb24ToYuyvBackend backend;
  Rgb24ToYuyvWithScratchFn convert;
  bool needs_scratch;
};

void ConvertScalarWithScratch(const std::uint8_t *src, int width, int height,
                              std::size_t src_stride, std::uint8_t *dst,
                              std::size_t dst_stride, std::uint8_t *scratch,
                              std::size_t scratch_size) {
  (void)scratch;
  (void)scratch_size;
  Rgb24ToYuyvScalar(src, width, height, src_stride, dst, dst_stride);
}

#if STUDIOCAST_HAVE_X86_SIMD
void ConvertSsse3WithScratch(const std::uint8_t *src, int width, int height,
                             std::size_t src_stride, std::uint8_t *dst,
                             std::size_t dst_stride, std::uint8_t *scratch,
                             std::size_t scratch_size) {
  (void)scratch;
  (void)scratch_size;
  Rgb24ToYuyvSsse3(src, width, height, src_stride, dst, dst_stride);
}

void ConvertAvx2WithScratch(const std::uint8_t *src, int width, int height,
                            std::size_t src_stride, std::uint8_t *dst,
                            std::size_t dst_stride, std::uint8_t *scratch,
                            std::size_t scratch_size) {
  (void)scratch;
  (void)scratch_size;
  Rgb24ToYuyvAvx2(src, width, height, src_stride, dst, dst_stride);
}
#endif

#if STUDIOCAST_HAVE_LIBYUV
void ConvertLibyuvWithScratch(const std::uint8_t *src, int width, int height,
                              std::size_t src_stride, std::uint8_t *dst,
                              std::size_t dst_stride, std::uint8_t *scratch,
                              std::size_t scratch_size) {
  if (Rgb24ToYuyvLibyuv(src, width, height, src_stride, dst, dst_stride,
                        scratch, scratch_size)) {
    return;
  }

  Rgb24ToYuyvScalar(src, width, height, src_stride, dst, dst_stride);
}
#endif

Rgb24ToYuyvDispatchPlan DetectDispatchPlan() {
#if STUDIOCAST_HAVE_X86_SIMD
  if (Rgb24ToYuyvAvx2Available())
    return {Rgb24ToYuyvBackend::avx2, &ConvertAvx2WithScratch, false};
  if (Rgb24ToYuyvSsse3Available())
    return {Rgb24ToYuyvBackend::ssse3, &ConvertSsse3WithScratch, false};
#endif

#if STUDIOCAST_HAVE_LIBYUV
  return {Rgb24ToYuyvBackend::libyuv, &ConvertLibyuvWithScratch, true};
#else
  return {Rgb24ToYuyvBackend::scalar, &ConvertScalarWithScratch, false};
#endif
}

const Rgb24ToYuyvDispatchPlan kDispatchPlan = DetectDispatchPlan();

} // namespace

const char *Rgb24ToYuyvBackendName(Rgb24ToYuyvBackend backend) {
  switch (backend) {
  case Rgb24ToYuyvBackend::scalar:
    return "scalar";
  case Rgb24ToYuyvBackend::libyuv:
    return "libyuv";
  case Rgb24ToYuyvBackend::ssse3:
    return "ssse3";
  case Rgb24ToYuyvBackend::avx2:
    return "avx2";
  }
  return "unknown";
}

Rgb24ToYuyvBackend Rgb24ToYuyvSelectedBackend() {
  return kDispatchPlan.backend;
}

bool Rgb24ToYuyvBackendAvailable(Rgb24ToYuyvBackend backend) {
  switch (backend) {
  case Rgb24ToYuyvBackend::scalar:
    return true;
  case Rgb24ToYuyvBackend::libyuv:
    return Rgb24ToYuyvLibyuvAvailable();
  case Rgb24ToYuyvBackend::ssse3:
    return Rgb24ToYuyvSsse3Available();
  case Rgb24ToYuyvBackend::avx2:
    return Rgb24ToYuyvAvx2Available();
  }
  return false;
}

std::size_t Rgb24ToYuyvDispatchScratchBytes(int width, int height) {
  if (kDispatchPlan.needs_scratch)
    return Rgb24ToYuyvLibyuvScratchBytes(width, height);
  return 0;
}

void Rgb24ToYuyvDispatchWithScratch(const std::uint8_t *src, int width,
                                    int height, std::size_t src_stride,
                                    std::uint8_t *dst, std::size_t dst_stride,
                                    std::uint8_t *scratch,
                                    std::size_t scratch_size) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  kDispatchPlan.convert(src, width, height, src_stride, dst, dst_stride,
                        scratch, scratch_size);
}

bool Rgb24ToYuyvLibyuvAvailable() {
#if STUDIOCAST_HAVE_LIBYUV
  return true;
#else
  return false;
#endif
}

std::size_t Rgb24ToYuyvLibyuvScratchBytes(int width, int height) {
#if STUDIOCAST_HAVE_LIBYUV
  if (width <= 0 || height <= 0)
    return 0;
  return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
         4u;
#else
  (void)width;
  (void)height;
  return 0;
#endif
}

bool Rgb24ToYuyvLibyuv(const std::uint8_t *src, int width, int height,
                       std::size_t src_stride, std::uint8_t *dst,
                       std::size_t dst_stride, std::uint8_t *scratch,
                       std::size_t scratch_size) {
#if STUDIOCAST_HAVE_LIBYUV
  if (!src || !dst || width <= 0 || height <= 0)
    return false;

  const std::size_t argb_stride = static_cast<std::size_t>(width) * 4u;
  const std::size_t argb_bytes = argb_stride * static_cast<std::size_t>(height);
  if (!scratch || scratch_size < argb_bytes || !FitsInt(src_stride) ||
      !FitsInt(dst_stride) || !FitsInt(argb_stride)) {
    return false;
  }

  const int src_stride_i = static_cast<int>(src_stride);
  const int dst_stride_i = static_cast<int>(dst_stride);
  const int argb_stride_i = static_cast<int>(argb_stride);
  if (libyuv::RAWToARGB(src, src_stride_i, scratch, argb_stride_i, width,
                        height) != 0 ||
      libyuv::ARGBToYUY2(scratch, argb_stride_i, dst, dst_stride_i, width,
                         height) != 0) {
    return false;
  }

  // libyuv writes 0 into the second luma slot of the last YUYV pair when the
  // width is odd. The other backends duplicate the final pixel there. Copy the
  // luma so every backend gives the same bytes.
  if ((width & 1) != 0) {
    const std::size_t tail = static_cast<std::size_t>(width - 1) * 2u;
    for (int y = 0; y < height; ++y) {
      std::uint8_t *row = dst + static_cast<std::size_t>(y) * dst_stride;
      row[tail + 2u] = row[tail];
    }
  }

  return true;
#else
  (void)src;
  (void)width;
  (void)height;
  (void)src_stride;
  (void)dst;
  (void)dst_stride;
  (void)scratch;
  (void)scratch_size;
  return false;
#endif
}

#if STUDIOCAST_HAVE_X86_SIMD
bool Rgb24ToYuyvSsse3Available() {
#if defined(__GNUC__) || defined(__clang__)
  static const bool available = [] {
    __builtin_cpu_init();
    return static_cast<bool>(__builtin_cpu_supports("ssse3"));
  }();
  return available;
#else
  return false;
#endif
}

bool Rgb24ToYuyvAvx2Available() {
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
bool Rgb24ToYuyvSsse3Available() { return false; }

void Rgb24ToYuyvSsse3(const std::uint8_t *src, int width, int height,
                      std::size_t src_stride, std::uint8_t *dst,
                      std::size_t dst_stride) {
  Rgb24ToYuyvScalar(src, width, height, src_stride, dst, dst_stride);
}

bool Rgb24ToYuyvAvx2Available() { return false; }

void Rgb24ToYuyvAvx2(const std::uint8_t *src, int width, int height,
                     std::size_t src_stride, std::uint8_t *dst,
                     std::size_t dst_stride) {
  Rgb24ToYuyvScalar(src, width, height, src_stride, dst, dst_stride);
}
#endif

} // namespace studiocast::video::internal
