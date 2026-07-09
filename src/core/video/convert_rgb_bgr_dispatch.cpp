#include "convert_rgb_bgr_internal.h"

#include <cstddef>
#include <cstdint>

#ifndef STUDIOCAST_HAVE_X86_SIMD
#define STUDIOCAST_HAVE_X86_SIMD 0
#endif

namespace studiocast::video::internal {
namespace {

struct Rgb24Bgr24DispatchPlan {
  Rgb24Bgr24Backend backend;
  Rgb24Bgr24Fn convert;
};

Rgb24Bgr24DispatchPlan DetectDispatchPlan() {
#if STUDIOCAST_HAVE_X86_SIMD
  if (Rgb24Bgr24Avx2Available())
    return {Rgb24Bgr24Backend::avx2, &Rgb24Bgr24Avx2};
  if (Rgb24Bgr24Ssse3Available())
    return {Rgb24Bgr24Backend::ssse3, &Rgb24Bgr24Ssse3};
#endif
  return {Rgb24Bgr24Backend::scalar, &Rgb24Bgr24Scalar};
}

const Rgb24Bgr24DispatchPlan kDispatchPlan = DetectDispatchPlan();

} // namespace

const char *Rgb24Bgr24BackendName(Rgb24Bgr24Backend backend) {
  switch (backend) {
  case Rgb24Bgr24Backend::scalar:
    return "scalar";
  case Rgb24Bgr24Backend::ssse3:
    return "ssse3";
  case Rgb24Bgr24Backend::avx2:
    return "avx2";
  }
  return "unknown";
}

Rgb24Bgr24Backend Rgb24Bgr24SelectedBackend() {
  return kDispatchPlan.backend;
}

bool Rgb24Bgr24BackendAvailable(Rgb24Bgr24Backend backend) {
  switch (backend) {
  case Rgb24Bgr24Backend::scalar:
    return true;
  case Rgb24Bgr24Backend::ssse3:
    return Rgb24Bgr24Ssse3Available();
  case Rgb24Bgr24Backend::avx2:
    return Rgb24Bgr24Avx2Available();
  }
  return false;
}

void Rgb24Bgr24Dispatch(const std::uint8_t *src, std::uint8_t *dst, int width,
                        int height, std::size_t src_stride,
                        std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;
  kDispatchPlan.convert(src, dst, width, height, src_stride, dst_stride);
}

#if STUDIOCAST_HAVE_X86_SIMD
bool Rgb24Bgr24Ssse3Available() {
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

bool Rgb24Bgr24Avx2Available() {
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
bool Rgb24Bgr24Ssse3Available() { return false; }

void Rgb24Bgr24Ssse3(const std::uint8_t *src, std::uint8_t *dst, int width,
                     int height, std::size_t src_stride,
                     std::size_t dst_stride) {
  Rgb24Bgr24Scalar(src, dst, width, height, src_stride, dst_stride);
}

bool Rgb24Bgr24Avx2Available() { return false; }

void Rgb24Bgr24Avx2(const std::uint8_t *src, std::uint8_t *dst, int width,
                    int height, std::size_t src_stride,
                    std::size_t dst_stride) {
  Rgb24Bgr24Scalar(src, dst, width, height, src_stride, dst_stride);
}
#endif

} // namespace studiocast::video::internal
