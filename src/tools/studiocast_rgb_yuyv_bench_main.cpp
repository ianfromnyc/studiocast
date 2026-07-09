#include "core/video/convert.h"
#include "core/video/convert_rgb_bgr_internal.h"
#include "core/video/convert_rgb_yuyv_internal.h"
#include "core/video/convert_yuyv_rgb_internal.h"
#include "core/video/image_ppm.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class BenchBackend {
  original_scalar,
  current_scalar,
  libyuv,
  ssse3,
  avx2,
  selected,
};

enum class YuyvToRgbBenchBackend {
  original_scalar,
  current_scalar,
  sse41,
  avx2,
  selected,
};

enum class RgbBgrBenchBackend {
  current_scalar,
  ssse3,
  avx2,
  selected,
};

struct SizeCase {
  int width;
  int height;
};

struct ResizeCase {
  int src_width;
  int src_height;
  int dst_width;
  int dst_height;
};

struct Stats {
  double avg_ms{};
  double p95_ms{};
  double p99_ms{};
  double min_ms{};
  double max_ms{};
  std::uint64_t checksum{};
};

constexpr std::size_t ActiveYuyvBytes(int width) {
  return static_cast<std::size_t>((width + 1) / 2) * 4u;
}

inline std::uint8_t RgbToYOriginal(int r, int g, int b) {
  return static_cast<std::uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) +
                                   16);
}

inline std::uint8_t RgbToUOriginal(int r, int g, int b) {
  return static_cast<std::uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) +
                                   128);
}

inline std::uint8_t RgbToVOriginal(int r, int g, int b) {
  return static_cast<std::uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) +
                                   128);
}

inline std::uint8_t ClampByteOriginal(int v) {
  if (v < 0)
    return 0;
  if (v > 255)
    return 255;
  return static_cast<std::uint8_t>(v);
}

inline void YuvToRgbOriginal(int y, int u, int v, int *out_r, int *out_g,
                             int *out_b) {
  int c = y - 16;
  int d = u - 128;
  int e = v - 128;

  if (c < 0)
    c = 0;

  *out_r = (298 * c + 409 * e + 128) >> 8;
  *out_g = (298 * c - 100 * d - 208 * e + 128) >> 8;
  *out_b = (298 * c + 516 * d + 128) >> 8;
}

void Rgb24ToYuyvOriginalScalar(const std::uint8_t *src, int width, int height,
                               std::size_t src_stride, std::uint8_t *dst,
                               std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  for (int y = 0; y < height; ++y) {
    const std::uint8_t *s = src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *d = dst + static_cast<std::size_t>(y) * dst_stride;

    int x = 0;
    for (; x + 1 < width; x += 2) {
      const int r0 = s[0];
      const int g0 = s[1];
      const int b0 = s[2];
      const int r1 = s[3];
      const int g1 = s[4];
      const int b1 = s[5];
      s += 6;

      d[0] = RgbToYOriginal(r0, g0, b0);
      d[1] = static_cast<std::uint8_t>(
          (static_cast<int>(RgbToUOriginal(r0, g0, b0)) +
           static_cast<int>(RgbToUOriginal(r1, g1, b1))) /
          2);
      d[2] = RgbToYOriginal(r1, g1, b1);
      d[3] = static_cast<std::uint8_t>(
          (static_cast<int>(RgbToVOriginal(r0, g0, b0)) +
           static_cast<int>(RgbToVOriginal(r1, g1, b1))) /
          2);
      d += 4;
    }

    if (x < width) {
      const int r0 = s[0];
      const int g0 = s[1];
      const int b0 = s[2];
      const std::uint8_t y0 = RgbToYOriginal(r0, g0, b0);
      d[0] = y0;
      d[1] = RgbToUOriginal(r0, g0, b0);
      d[2] = y0;
      d[3] = RgbToVOriginal(r0, g0, b0);
    }
  }
}

void YuyvToRgb24OriginalScalar(const std::uint8_t *src, int width, int height,
                               std::size_t src_stride, std::uint8_t *dst,
                               std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  for (int y = 0; y < height; ++y) {
    const std::uint8_t *s = src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *d = dst + static_cast<std::size_t>(y) * dst_stride;

    for (int x = 0; x < width; x += 2) {
      const int y0 = s[0];
      const int u = s[1];
      const int y1 = s[2];
      const int v = s[3];
      s += 4;

      int r0, g0, b0;
      int r1, g1, b1;
      YuvToRgbOriginal(y0, u, v, &r0, &g0, &b0);
      YuvToRgbOriginal(y1, u, v, &r1, &g1, &b1);

      d[0] = ClampByteOriginal(r0);
      d[1] = ClampByteOriginal(g0);
      d[2] = ClampByteOriginal(b0);
      d += 3;

      if (x + 1 < width) {
        d[0] = ClampByteOriginal(r1);
        d[1] = ClampByteOriginal(g1);
        d[2] = ClampByteOriginal(b1);
        d += 3;
      }
    }
  }
}

int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

bool ResizeRgb24BilinearOriginal(const std::uint8_t *src_rgb, int src_w,
                                 int src_h, std::size_t src_stride, int dst_w,
                                 int dst_h, std::vector<std::uint8_t> *dst_rgb,
                                 std::size_t dst_stride, std::string *error) {
  if (!dst_rgb)
    return false;
  dst_rgb->clear();

  if (!src_rgb || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
    if (error)
      *error = "Invalid resize dimensions";
    return false;
  }
  if (src_stride < static_cast<std::size_t>(src_w) * 3u ||
      dst_stride < static_cast<std::size_t>(dst_w) * 3u) {
    if (error)
      *error = "Invalid resize stride";
    return false;
  }

  dst_rgb->resize(dst_stride * static_cast<std::size_t>(dst_h));

  const float scale_x = static_cast<float>(src_w) / static_cast<float>(dst_w);
  const float scale_y = static_cast<float>(src_h) / static_cast<float>(dst_h);

  for (int y = 0; y < dst_h; ++y) {
    const float src_y = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
    const int y0 = ClampInt(static_cast<int>(std::floor(src_y)), 0, src_h - 1);
    const int y1 = ClampInt(y0 + 1, 0, src_h - 1);
    const float fy = src_y - static_cast<float>(y0);

    auto *dst_row = dst_rgb->data() + static_cast<std::size_t>(y) * dst_stride;
    const auto *src_row0 = src_rgb + static_cast<std::size_t>(y0) * src_stride;
    const auto *src_row1 = src_rgb + static_cast<std::size_t>(y1) * src_stride;

    for (int x = 0; x < dst_w; ++x) {
      const float src_x = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
      const int x0 =
          ClampInt(static_cast<int>(std::floor(src_x)), 0, src_w - 1);
      const int x1 = ClampInt(x0 + 1, 0, src_w - 1);
      const float fx = src_x - static_cast<float>(x0);

      const auto *p00 = src_row0 + static_cast<std::size_t>(x0) * 3u;
      const auto *p10 = src_row0 + static_cast<std::size_t>(x1) * 3u;
      const auto *p01 = src_row1 + static_cast<std::size_t>(x0) * 3u;
      const auto *p11 = src_row1 + static_cast<std::size_t>(x1) * 3u;

      for (int c = 0; c < 3; ++c) {
        const float v0 =
            static_cast<float>(p00[c]) +
            fx * (static_cast<float>(p10[c]) - static_cast<float>(p00[c]));
        const float v1 =
            static_cast<float>(p01[c]) +
            fx * (static_cast<float>(p11[c]) - static_cast<float>(p01[c]));
        const float v = v0 + fy * (v1 - v0);
        const int iv = ClampInt(static_cast<int>(std::lround(v)), 0, 255);
        dst_row[static_cast<std::size_t>(x) * 3u +
                static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(iv);
      }
    }
  }

  return true;
}

void FillDeterministicRgb(std::vector<std::uint8_t> *src, int width, int height,
                          std::size_t stride) {
  std::uint32_t state =
      static_cast<std::uint32_t>(width * 1009 + height * 9176 + 0x12345);
  for (int y = 0; y < height; ++y) {
    std::uint8_t *row = src->data() + static_cast<std::size_t>(y) * stride;
    for (std::size_t x = 0; x < static_cast<std::size_t>(width) * 3u; ++x) {
      state = state * 1664525u + 1013904223u;
      row[x] = static_cast<std::uint8_t>((state >> 24) & 0xffu);
    }
  }
}

void CopyRgbToPaddedOriginal(const std::uint8_t *src, int width, int height,
                             std::size_t src_stride, std::uint8_t *dst,
                             std::size_t dst_stride) {
  const std::size_t active = static_cast<std::size_t>(width) * 3u;
  for (int y = 0; y < height; ++y) {
    const std::uint8_t *src_row =
        src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *dst_row = dst + static_cast<std::size_t>(y) * dst_stride;
    for (std::size_t i = 0; i < active; ++i)
      dst_row[i] = src_row[i];
    for (std::size_t i = active; i < dst_stride; ++i)
      dst_row[i] = 0;
  }
}

void CopyRgbToPaddedCurrent(const std::uint8_t *src, int width, int height,
                            std::size_t src_stride, std::uint8_t *dst,
                            std::size_t dst_stride) {
  const std::size_t active = static_cast<std::size_t>(width) * 3u;
  for (int y = 0; y < height; ++y) {
    const std::uint8_t *src_row =
        src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *dst_row = dst + static_cast<std::size_t>(y) * dst_stride;
    for (std::size_t i = 0; i < active; ++i)
      dst_row[i] = src_row[i];
  }
}

void FillDeterministicYuyv(std::vector<std::uint8_t> *src, int width,
                           int height, std::size_t stride) {
  std::uint32_t state =
      static_cast<std::uint32_t>(width * 7351 + height * 2797 + 0x54321);
  const std::size_t active = ActiveYuyvBytes(width);
  for (int y = 0; y < height; ++y) {
    std::uint8_t *row = src->data() + static_cast<std::size_t>(y) * stride;
    for (std::size_t x = 0; x < active; ++x) {
      state = state * 1664525u + 1013904223u;
      row[x] = static_cast<std::uint8_t>((state >> 24) & 0xffu);
    }
  }
}

const char *BackendName(BenchBackend backend) {
  switch (backend) {
  case BenchBackend::original_scalar:
    return "original-scalar";
  case BenchBackend::current_scalar:
    return "current-scalar";
  case BenchBackend::libyuv:
    return "libyuv";
  case BenchBackend::ssse3:
    return "ssse3";
  case BenchBackend::avx2:
    return "avx2";
  case BenchBackend::selected:
    return "selected";
  }
  return "unknown";
}

const char *BackendName(YuyvToRgbBenchBackend backend) {
  switch (backend) {
  case YuyvToRgbBenchBackend::original_scalar:
    return "original-scalar";
  case YuyvToRgbBenchBackend::current_scalar:
    return "current-scalar";
  case YuyvToRgbBenchBackend::sse41:
    return "sse4.1";
  case YuyvToRgbBenchBackend::avx2:
    return "avx2";
  case YuyvToRgbBenchBackend::selected:
    return "selected";
  }
  return "unknown";
}

const char *BackendName(RgbBgrBenchBackend backend) {
  switch (backend) {
  case RgbBgrBenchBackend::current_scalar:
    return "current-scalar";
  case RgbBgrBenchBackend::ssse3:
    return "ssse3";
  case RgbBgrBenchBackend::avx2:
    return "avx2";
  case RgbBgrBenchBackend::selected:
    return "selected";
  }
  return "unknown";
}

bool BackendAvailable(YuyvToRgbBenchBackend backend) {
  using studiocast::video::internal::YuyvToRgbBackend;

  switch (backend) {
  case YuyvToRgbBenchBackend::original_scalar:
  case YuyvToRgbBenchBackend::current_scalar:
  case YuyvToRgbBenchBackend::selected:
    return true;
  case YuyvToRgbBenchBackend::sse41:
    return studiocast::video::internal::YuyvToRgbBackendAvailable(
        YuyvToRgbBackend::sse41);
  case YuyvToRgbBenchBackend::avx2:
    return studiocast::video::internal::YuyvToRgbBackendAvailable(
        YuyvToRgbBackend::avx2);
  }
  return false;
}

bool BackendAvailable(RgbBgrBenchBackend backend) {
  using studiocast::video::internal::Rgb24Bgr24Backend;

  switch (backend) {
  case RgbBgrBenchBackend::current_scalar:
  case RgbBgrBenchBackend::selected:
    return true;
  case RgbBgrBenchBackend::ssse3:
    return studiocast::video::internal::Rgb24Bgr24BackendAvailable(
        Rgb24Bgr24Backend::ssse3);
  case RgbBgrBenchBackend::avx2:
    return studiocast::video::internal::Rgb24Bgr24BackendAvailable(
        Rgb24Bgr24Backend::avx2);
  }
  return false;
}

bool BackendAvailable(BenchBackend backend) {
  using studiocast::video::internal::Rgb24ToYuyvBackend;

  switch (backend) {
  case BenchBackend::original_scalar:
  case BenchBackend::current_scalar:
  case BenchBackend::selected:
    return true;
  case BenchBackend::libyuv:
    return studiocast::video::internal::Rgb24ToYuyvBackendAvailable(
        Rgb24ToYuyvBackend::libyuv);
  case BenchBackend::ssse3:
    return studiocast::video::internal::Rgb24ToYuyvBackendAvailable(
        Rgb24ToYuyvBackend::ssse3);
  case BenchBackend::avx2:
    return studiocast::video::internal::Rgb24ToYuyvBackendAvailable(
        Rgb24ToYuyvBackend::avx2);
  }
  return false;
}

std::size_t ScratchBytes(BenchBackend backend, int width, int height) {
  switch (backend) {
  case BenchBackend::libyuv:
    return studiocast::video::internal::Rgb24ToYuyvLibyuvScratchBytes(width,
                                                                      height);
  case BenchBackend::selected:
    return studiocast::video::Rgb24ToYuyvScratchBytes(width, height);
  case BenchBackend::original_scalar:
  case BenchBackend::current_scalar:
  case BenchBackend::ssse3:
  case BenchBackend::avx2:
    return 0;
  }
  return 0;
}

bool Convert(BenchBackend backend, const std::uint8_t *src, int width,
             int height, std::size_t src_stride, std::uint8_t *dst,
             std::size_t dst_stride, std::uint8_t *scratch,
             std::size_t scratch_size) {
  switch (backend) {
  case BenchBackend::original_scalar:
    Rgb24ToYuyvOriginalScalar(src, width, height, src_stride, dst, dst_stride);
    return true;
  case BenchBackend::current_scalar:
    studiocast::video::internal::Rgb24ToYuyvScalar(src, width, height,
                                                   src_stride, dst, dst_stride);
    return true;
  case BenchBackend::libyuv:
    return studiocast::video::internal::Rgb24ToYuyvLibyuv(
        src, width, height, src_stride, dst, dst_stride, scratch, scratch_size);
  case BenchBackend::ssse3:
    studiocast::video::internal::Rgb24ToYuyvSsse3(src, width, height,
                                                  src_stride, dst, dst_stride);
    return true;
  case BenchBackend::avx2:
    studiocast::video::internal::Rgb24ToYuyvAvx2(src, width, height, src_stride,
                                                 dst, dst_stride);
    return true;
  case BenchBackend::selected:
    studiocast::video::Rgb24ToYuyvWithScratch(
        src, width, height, src_stride, dst, dst_stride, scratch, scratch_size);
    return true;
  }
  return false;
}

bool Convert(YuyvToRgbBenchBackend backend, const std::uint8_t *src, int width,
             int height, std::size_t src_stride, std::uint8_t *dst,
             std::size_t dst_stride) {
  switch (backend) {
  case YuyvToRgbBenchBackend::original_scalar:
    YuyvToRgb24OriginalScalar(src, width, height, src_stride, dst, dst_stride);
    return true;
  case YuyvToRgbBenchBackend::current_scalar:
    studiocast::video::internal::YuyvToRgbScalar(src, width, height, src_stride,
                                                 dst, dst_stride);
    return true;
  case YuyvToRgbBenchBackend::sse41:
    studiocast::video::internal::YuyvToRgbSse41(src, width, height, src_stride,
                                                dst, dst_stride);
    return true;
  case YuyvToRgbBenchBackend::avx2:
    studiocast::video::internal::YuyvToRgbAvx2(src, width, height, src_stride,
                                               dst, dst_stride);
    return true;
  case YuyvToRgbBenchBackend::selected:
    studiocast::video::YuyvToRgb24(src, width, height, src_stride, dst,
                                   dst_stride);
    return true;
  }
  return false;
}

bool Convert(RgbBgrBenchBackend backend, const std::uint8_t *src, int width,
             int height, std::size_t src_stride, std::uint8_t *dst,
             std::size_t dst_stride) {
  switch (backend) {
  case RgbBgrBenchBackend::current_scalar:
    studiocast::video::internal::Rgb24Bgr24Scalar(src, dst, width, height,
                                                  src_stride, dst_stride);
    return true;
  case RgbBgrBenchBackend::ssse3:
    studiocast::video::internal::Rgb24Bgr24Ssse3(src, dst, width, height,
                                                 src_stride, dst_stride);
    return true;
  case RgbBgrBenchBackend::avx2:
    studiocast::video::internal::Rgb24Bgr24Avx2(src, dst, width, height,
                                                src_stride, dst_stride);
    return true;
  case RgbBgrBenchBackend::selected:
    studiocast::video::Rgb24ToBgr24(src, dst, width, height, src_stride,
                                    dst_stride);
    return true;
  }
  return false;
}

Stats RunBench(BenchBackend backend, const SizeCase &size, int iterations,
               int warmup) {
  const std::size_t src_stride = static_cast<std::size_t>(size.width) * 3u;
  const std::size_t dst_stride = ActiveYuyvBytes(size.width);
  std::vector<std::uint8_t> src(src_stride *
                                static_cast<std::size_t>(size.height));
  std::vector<std::uint8_t> dst(dst_stride *
                                static_cast<std::size_t>(size.height));
  std::vector<std::uint8_t> scratch(
      ScratchBytes(backend, size.width, size.height));
  FillDeterministicRgb(&src, size.width, size.height, src_stride);

  for (int i = 0; i < warmup; ++i) {
    Convert(backend, src.data(), size.width, size.height, src_stride,
            dst.data(), dst_stride, scratch.empty() ? nullptr : scratch.data(),
            scratch.size());
  }

  std::vector<double> samples_ms;
  samples_ms.reserve(static_cast<std::size_t>(iterations));
  std::uint64_t checksum = 0;

  for (int i = 0; i < iterations; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    Convert(backend, src.data(), size.width, size.height, src_stride,
            dst.data(), dst_stride, scratch.empty() ? nullptr : scratch.data(),
            scratch.size());
    const auto t1 = std::chrono::steady_clock::now();

    samples_ms.push_back(
        std::chrono::duration<double, std::milli>(t1 - t0).count());
    checksum +=
        dst[(static_cast<std::size_t>(i) * 131u) %
            std::max<std::size_t>(static_cast<std::size_t>(1), dst.size())];
  }

  std::sort(samples_ms.begin(), samples_ms.end());
  const auto percentile = [&](double p) {
    const double pos =
        (p / 100.0) * static_cast<double>(samples_ms.size() - 1u);
    const std::size_t idx = static_cast<std::size_t>(pos + 0.5);
    return samples_ms[std::min(idx, samples_ms.size() - 1u)];
  };

  double sum = 0.0;
  for (double sample : samples_ms)
    sum += sample;

  Stats stats{};
  stats.avg_ms = sum / static_cast<double>(samples_ms.size());
  stats.p95_ms = percentile(95.0);
  stats.p99_ms = percentile(99.0);
  stats.min_ms = samples_ms.front();
  stats.max_ms = samples_ms.back();
  stats.checksum = checksum;
  return stats;
}

Stats RunYuyvToRgbBench(YuyvToRgbBenchBackend backend, const SizeCase &size,
                        int iterations, int warmup) {
  const std::size_t src_stride = ActiveYuyvBytes(size.width);
  const std::size_t dst_stride = static_cast<std::size_t>(size.width) * 3u;
  std::vector<std::uint8_t> src(src_stride *
                                static_cast<std::size_t>(size.height));
  std::vector<std::uint8_t> dst(dst_stride *
                                static_cast<std::size_t>(size.height));
  FillDeterministicYuyv(&src, size.width, size.height, src_stride);

  for (int i = 0; i < warmup; ++i) {
    Convert(backend, src.data(), size.width, size.height, src_stride,
            dst.data(), dst_stride);
  }

  std::vector<double> samples_ms;
  samples_ms.reserve(static_cast<std::size_t>(iterations));
  std::uint64_t checksum = 0;

  for (int i = 0; i < iterations; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    Convert(backend, src.data(), size.width, size.height, src_stride,
            dst.data(), dst_stride);
    const auto t1 = std::chrono::steady_clock::now();

    samples_ms.push_back(
        std::chrono::duration<double, std::milli>(t1 - t0).count());
    checksum +=
        dst[(static_cast<std::size_t>(i) * 131u) %
            std::max<std::size_t>(static_cast<std::size_t>(1), dst.size())];
  }

  std::sort(samples_ms.begin(), samples_ms.end());
  const auto percentile = [&](double p) {
    const double pos =
        (p / 100.0) * static_cast<double>(samples_ms.size() - 1u);
    const std::size_t idx = static_cast<std::size_t>(pos + 0.5);
    return samples_ms[std::min(idx, samples_ms.size() - 1u)];
  };

  double sum = 0.0;
  for (double sample : samples_ms)
    sum += sample;

  Stats stats{};
  stats.avg_ms = sum / static_cast<double>(samples_ms.size());
  stats.p95_ms = percentile(95.0);
  stats.p99_ms = percentile(99.0);
  stats.min_ms = samples_ms.front();
  stats.max_ms = samples_ms.back();
  stats.checksum = checksum;
  return stats;
}

Stats RunRgbBgrBench(RgbBgrBenchBackend backend, const SizeCase &size,
                     int iterations, int warmup) {
  const std::size_t src_stride = static_cast<std::size_t>(size.width) * 3u;
  const std::size_t dst_stride = static_cast<std::size_t>(size.width) * 3u;
  std::vector<std::uint8_t> src(src_stride *
                                static_cast<std::size_t>(size.height));
  std::vector<std::uint8_t> dst(dst_stride *
                                static_cast<std::size_t>(size.height));
  FillDeterministicRgb(&src, size.width, size.height, src_stride);

  for (int i = 0; i < warmup; ++i) {
    Convert(backend, src.data(), size.width, size.height, src_stride,
            dst.data(), dst_stride);
  }

  std::vector<double> samples_ms;
  samples_ms.reserve(static_cast<std::size_t>(iterations));
  std::uint64_t checksum = 0;

  for (int i = 0; i < iterations; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    Convert(backend, src.data(), size.width, size.height, src_stride,
            dst.data(), dst_stride);
    const auto t1 = std::chrono::steady_clock::now();

    samples_ms.push_back(
        std::chrono::duration<double, std::milli>(t1 - t0).count());
    checksum +=
        dst[(static_cast<std::size_t>(i) * 131u) %
            std::max<std::size_t>(static_cast<std::size_t>(1), dst.size())];
  }

  std::sort(samples_ms.begin(), samples_ms.end());
  const auto percentile = [&](double p) {
    const double pos =
        (p / 100.0) * static_cast<double>(samples_ms.size() - 1u);
    const std::size_t idx = static_cast<std::size_t>(pos + 0.5);
    return samples_ms[std::min(idx, samples_ms.size() - 1u)];
  };

  double sum = 0.0;
  for (double sample : samples_ms)
    sum += sample;

  Stats stats{};
  stats.avg_ms = sum / static_cast<double>(samples_ms.size());
  stats.p95_ms = percentile(95.0);
  stats.p99_ms = percentile(99.0);
  stats.min_ms = samples_ms.front();
  stats.max_ms = samples_ms.back();
  stats.checksum = checksum;
  return stats;
}

Stats RunResizeBench(bool original, const ResizeCase &size, int iterations,
                     int warmup) {
  const std::size_t src_stride =
      static_cast<std::size_t>(size.src_width) * 3u + 32u;
  const std::size_t dst_stride =
      static_cast<std::size_t>(size.dst_width) * 3u + 32u;
  std::vector<std::uint8_t> src(src_stride *
                                static_cast<std::size_t>(size.src_height));
  std::vector<std::uint8_t> dst;
  dst.reserve(dst_stride * static_cast<std::size_t>(size.dst_height));
  FillDeterministicRgb(&src, size.src_width, size.src_height, src_stride);

  std::string err;
  studiocast::video::Rgb24BilinearResizePlan plan;
  if (!original) {
    (void)plan.Configure(size.src_width, size.src_height, size.dst_width,
                         size.dst_height, &err);
  }
  for (int i = 0; i < warmup; ++i) {
    if (original) {
      (void)ResizeRgb24BilinearOriginal(
          src.data(), size.src_width, size.src_height, src_stride,
          size.dst_width, size.dst_height, &dst, dst_stride, &err);
    } else {
      (void)plan.Apply(src.data(), src_stride, &dst, dst_stride, &err);
    }
  }

  std::vector<double> samples_ms;
  samples_ms.reserve(static_cast<std::size_t>(iterations));
  std::uint64_t checksum = 0;

  for (int i = 0; i < iterations; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    if (original) {
      (void)ResizeRgb24BilinearOriginal(
          src.data(), size.src_width, size.src_height, src_stride,
          size.dst_width, size.dst_height, &dst, dst_stride, &err);
    } else {
      (void)plan.Apply(src.data(), src_stride, &dst, dst_stride, &err);
    }
    const auto t1 = std::chrono::steady_clock::now();

    samples_ms.push_back(
        std::chrono::duration<double, std::milli>(t1 - t0).count());
    checksum +=
        dst[(static_cast<std::size_t>(i) * 131u) %
            std::max<std::size_t>(static_cast<std::size_t>(1), dst.size())];
  }

  std::sort(samples_ms.begin(), samples_ms.end());
  const auto percentile = [&](double p) {
    const double pos =
        (p / 100.0) * static_cast<double>(samples_ms.size() - 1u);
    const std::size_t idx = static_cast<std::size_t>(pos + 0.5);
    return samples_ms[std::min(idx, samples_ms.size() - 1u)];
  };

  double sum = 0.0;
  for (double sample : samples_ms)
    sum += sample;

  Stats stats{};
  stats.avg_ms = sum / static_cast<double>(samples_ms.size());
  stats.p95_ms = percentile(95.0);
  stats.p99_ms = percentile(99.0);
  stats.min_ms = samples_ms.front();
  stats.max_ms = samples_ms.back();
  stats.checksum = checksum;
  return stats;
}

Stats RunPaddedRgbCopyBench(bool original, const SizeCase &size, int iterations,
                            int warmup) {
  const std::size_t src_stride = static_cast<std::size_t>(size.width) * 3u;
  const std::size_t dst_stride =
      static_cast<std::size_t>(size.width) * 3u + 128u;
  std::vector<std::uint8_t> src(src_stride *
                                static_cast<std::size_t>(size.height));
  std::vector<std::uint8_t> dst(dst_stride *
                                static_cast<std::size_t>(size.height));
  FillDeterministicRgb(&src, size.width, size.height, src_stride);

  for (int i = 0; i < warmup; ++i) {
    if (original) {
      CopyRgbToPaddedOriginal(src.data(), size.width, size.height, src_stride,
                              dst.data(), dst_stride);
    } else {
      CopyRgbToPaddedCurrent(src.data(), size.width, size.height, src_stride,
                             dst.data(), dst_stride);
    }
  }

  std::vector<double> samples_ms;
  samples_ms.reserve(static_cast<std::size_t>(iterations));
  std::uint64_t checksum = 0;

  for (int i = 0; i < iterations; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    if (original) {
      CopyRgbToPaddedOriginal(src.data(), size.width, size.height, src_stride,
                              dst.data(), dst_stride);
    } else {
      CopyRgbToPaddedCurrent(src.data(), size.width, size.height, src_stride,
                             dst.data(), dst_stride);
    }
    const auto t1 = std::chrono::steady_clock::now();

    samples_ms.push_back(
        std::chrono::duration<double, std::milli>(t1 - t0).count());
    checksum +=
        dst[(static_cast<std::size_t>(i) * 131u) %
            std::max<std::size_t>(static_cast<std::size_t>(1), dst.size())];
  }

  std::sort(samples_ms.begin(), samples_ms.end());
  const auto percentile = [&](double p) {
    const double pos =
        (p / 100.0) * static_cast<double>(samples_ms.size() - 1u);
    const std::size_t idx = static_cast<std::size_t>(pos + 0.5);
    return samples_ms[std::min(idx, samples_ms.size() - 1u)];
  };

  double sum = 0.0;
  for (double sample : samples_ms)
    sum += sample;

  Stats stats{};
  stats.avg_ms = sum / static_cast<double>(samples_ms.size());
  stats.p95_ms = percentile(95.0);
  stats.p99_ms = percentile(99.0);
  stats.min_ms = samples_ms.front();
  stats.max_ms = samples_ms.back();
  stats.checksum = checksum;
  return stats;
}

std::string GetArgValue(int argc, char **argv, std::string_view key) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == key)
      return argv[i + 1] ? std::string(argv[i + 1]) : std::string();
  }
  return {};
}

int GetArgInt(int argc, char **argv, std::string_view key, int fallback) {
  const std::string value = GetArgValue(argc, argv, key);
  if (value.empty())
    return fallback;
  return std::max(1, std::atoi(value.c_str()));
}

bool HasArg(int argc, char **argv, std::string_view key) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == key)
      return true;
  }
  return false;
}

bool ApplyBackendFilter(std::vector<BenchBackend> *backends,
                        std::string_view filter) {
  if (filter.empty())
    return true;
  for (BenchBackend backend : *backends) {
    if (filter == BackendName(backend)) {
      *backends = {backend};
      return true;
    }
  }
  return false;
}

bool ApplyBackendFilter(std::vector<YuyvToRgbBenchBackend> *backends,
                        std::string_view filter) {
  if (filter.empty())
    return true;
  for (YuyvToRgbBenchBackend backend : *backends) {
    if (filter == BackendName(backend)) {
      *backends = {backend};
      return true;
    }
  }
  return false;
}

bool ApplyBackendFilter(std::vector<RgbBgrBenchBackend> *backends,
                        std::string_view filter) {
  if (filter.empty())
    return true;
  for (RgbBgrBenchBackend backend : *backends) {
    if (filter == BackendName(backend)) {
      *backends = {backend};
      return true;
    }
  }
  return false;
}

void Usage(const char *argv0) {
  std::cout << "StudioCast RGB/YUYV Conversion Benchmark\n\n"
            << "Usage:\n"
            << "  " << argv0
            << " [--iterations N] [--warmup N] [--csv] "
               "[--only all|convert|resize|copy|prep]\n"
            << "      [--rgb-yuyv-backend NAME] [--yuyv-rgb-backend NAME]\n"
            << "      [--rgb-bgr-backend NAME]\n\n"
            << "RGB24 -> YUYV backends: original-scalar, current-scalar, "
               "libyuv, ssse3, avx2, selected\n"
            << "YUYV -> RGB24 backends: original-scalar, current-scalar, "
               "sse4.1, avx2, selected\n"
            << "RGB24 <-> BGR24 backends: current-scalar, ssse3, avx2, "
               "selected\n"
            << "Resize/write prep: previous-loop, current\n"
            << "Sizes:    640x480, 1280x720, 1920x1080\n";
}

} // namespace

int main(int argc, char **argv) {
  if (HasArg(argc, argv, "--help") || HasArg(argc, argv, "-h")) {
    Usage(argv[0]);
    return 0;
  }

  const int iterations = GetArgInt(argc, argv, "--iterations", 120);
  const int warmup = GetArgInt(argc, argv, "--warmup", 10);
  const bool csv = HasArg(argc, argv, "--csv");
  const std::string only = GetArgValue(argc, argv, "--only");
  const bool run_conversions =
      only.empty() || only == "all" || only == "convert";
  const bool run_resize =
      only.empty() || only == "all" || only == "resize" || only == "prep";
  const bool run_copy =
      only.empty() || only == "all" || only == "copy" || only == "prep";

  const std::vector<SizeCase> sizes{{640, 480}, {1280, 720}, {1920, 1080}};
  const std::vector<ResizeCase> resize_sizes{
      {640, 480, 1280, 720},
      {1280, 720, 1920, 1080},
      {1920, 1080, 1280, 720},
  };
  std::vector<BenchBackend> backends{
      BenchBackend::original_scalar,
      BenchBackend::current_scalar,
      BenchBackend::libyuv,
      BenchBackend::ssse3,
      BenchBackend::avx2,
      BenchBackend::selected,
  };
  std::vector<YuyvToRgbBenchBackend> yuyv_to_rgb_backends{
      YuyvToRgbBenchBackend::original_scalar,
      YuyvToRgbBenchBackend::current_scalar,
      YuyvToRgbBenchBackend::sse41,
      YuyvToRgbBenchBackend::avx2,
      YuyvToRgbBenchBackend::selected,
  };
  std::vector<RgbBgrBenchBackend> rgb_bgr_backends{
      RgbBgrBenchBackend::current_scalar,
      RgbBgrBenchBackend::ssse3,
      RgbBgrBenchBackend::avx2,
      RgbBgrBenchBackend::selected,
  };

  const std::string rgb_yuyv_backend =
      GetArgValue(argc, argv, "--rgb-yuyv-backend");
  const std::string yuyv_rgb_backend =
      GetArgValue(argc, argv, "--yuyv-rgb-backend");
  const std::string rgb_bgr_backend =
      GetArgValue(argc, argv, "--rgb-bgr-backend");
  if (!ApplyBackendFilter(&backends, rgb_yuyv_backend)) {
    std::cerr << "Unknown RGB24 -> YUYV backend: " << rgb_yuyv_backend
              << "\n\n";
    Usage(argv[0]);
    return 2;
  }
  if (!ApplyBackendFilter(&yuyv_to_rgb_backends, yuyv_rgb_backend)) {
    std::cerr << "Unknown YUYV -> RGB24 backend: " << yuyv_rgb_backend
              << "\n\n";
    Usage(argv[0]);
    return 2;
  }
  if (!ApplyBackendFilter(&rgb_bgr_backends, rgb_bgr_backend)) {
    std::cerr << "Unknown RGB24 <-> BGR24 backend: " << rgb_bgr_backend
              << "\n\n";
    Usage(argv[0]);
    return 2;
  }

  std::cout << "StudioCast RGB/YUYV Conversion Benchmark\n";
  std::cout << "Selected RGB24 -> YUYV backend: "
            << studiocast::video::internal::Rgb24ToYuyvBackendName(
                   studiocast::video::internal::Rgb24ToYuyvSelectedBackend())
            << "\n";
  std::cout << "Selected YUYV -> RGB24 backend: "
            << studiocast::video::internal::YuyvToRgbBackendName(
                   studiocast::video::internal::YuyvToRgbSelectedBackend())
            << "\n";
  std::cout << "Selected RGB24 <-> BGR24 backend: "
            << studiocast::video::internal::Rgb24Bgr24BackendName(
                   studiocast::video::internal::Rgb24Bgr24SelectedBackend())
            << "\n";
  std::cout << "Iterations: " << iterations << " measured, " << warmup
            << " warmup\n\n";

  if (csv) {
    std::cout << "direction,backend,width,height,avg_ms,p95_ms,p99_ms,min_ms,"
                 "max_ms,checksum\n";
  } else if (run_conversions) {
    std::cout << "RGB24 -> YUYV\n";
    std::cout << std::left << std::setw(18) << "backend" << std::right
              << std::setw(12) << "size" << std::setw(12) << "avg ms"
              << std::setw(12) << "p95 ms" << std::setw(12) << "p99 ms"
              << std::setw(12) << "min ms" << std::setw(12) << "max ms"
              << "\n";
  }

  if (run_conversions) {
    for (BenchBackend backend : backends) {
      if (!BackendAvailable(backend)) {
        if (!csv) {
          std::cout << std::left << std::setw(18) << BackendName(backend)
                    << "unavailable\n";
        }
        continue;
      }

      for (const SizeCase &size : sizes) {
        const Stats stats = RunBench(backend, size, iterations, warmup);
        if (csv) {
          std::cout << "rgb24-to-yuyv," << BackendName(backend) << ","
                    << size.width << "," << size.height << "," << stats.avg_ms
                    << "," << stats.p95_ms << "," << stats.p99_ms << ","
                    << stats.min_ms << "," << stats.max_ms << ","
                    << stats.checksum << "\n";
        } else {
          std::cout << std::fixed << std::setprecision(3);
          std::cout << std::left << std::setw(18) << BackendName(backend)
                    << std::right << std::setw(7) << size.width << "x"
                    << std::left << std::setw(4) << size.height << std::right
                    << std::setw(12) << stats.avg_ms << std::setw(12)
                    << stats.p95_ms << std::setw(12) << stats.p99_ms
                    << std::setw(12) << stats.min_ms << std::setw(12)
                    << stats.max_ms << "\n";
        }
      }
    }

    if (!csv) {
      std::cout << "\nYUYV -> RGB24\n";
      std::cout << std::left << std::setw(18) << "backend" << std::right
                << std::setw(12) << "size" << std::setw(12) << "avg ms"
                << std::setw(12) << "p95 ms" << std::setw(12) << "p99 ms"
                << std::setw(12) << "min ms" << std::setw(12) << "max ms"
                << "\n";
    }

    for (YuyvToRgbBenchBackend backend : yuyv_to_rgb_backends) {
      if (!BackendAvailable(backend)) {
        if (!csv) {
          std::cout << std::left << std::setw(18) << BackendName(backend)
                    << "unavailable\n";
        }
        continue;
      }

      for (const SizeCase &size : sizes) {
        const Stats stats =
            RunYuyvToRgbBench(backend, size, iterations, warmup);
        if (csv) {
          std::cout << "yuyv-to-rgb24," << BackendName(backend) << ","
                    << size.width << "," << size.height << "," << stats.avg_ms
                    << "," << stats.p95_ms << "," << stats.p99_ms << ","
                    << stats.min_ms << "," << stats.max_ms << ","
                    << stats.checksum << "\n";
        } else {
          std::cout << std::fixed << std::setprecision(3);
          std::cout << std::left << std::setw(18) << BackendName(backend)
                    << std::right << std::setw(7) << size.width << "x"
                    << std::left << std::setw(4) << size.height << std::right
                    << std::setw(12) << stats.avg_ms << std::setw(12)
                    << stats.p95_ms << std::setw(12) << stats.p99_ms
                    << std::setw(12) << stats.min_ms << std::setw(12)
                    << stats.max_ms << "\n";
        }
      }
    }

    if (!csv) {
      std::cout << "\nRGB24 <-> BGR24\n";
      std::cout << std::left << std::setw(18) << "backend" << std::right
                << std::setw(12) << "size" << std::setw(12) << "avg ms"
                << std::setw(12) << "p95 ms" << std::setw(12) << "p99 ms"
                << std::setw(12) << "min ms" << std::setw(12) << "max ms"
                << "\n";
    }

    for (RgbBgrBenchBackend backend : rgb_bgr_backends) {
      if (!BackendAvailable(backend)) {
        if (!csv) {
          std::cout << std::left << std::setw(18) << BackendName(backend)
                    << "unavailable\n";
        }
        continue;
      }

      for (const SizeCase &size : sizes) {
        const Stats stats = RunRgbBgrBench(backend, size, iterations, warmup);
        if (csv) {
          std::cout << "rgb24-to-bgr24," << BackendName(backend) << ","
                    << size.width << "," << size.height << "," << stats.avg_ms
                    << "," << stats.p95_ms << "," << stats.p99_ms << ","
                    << stats.min_ms << "," << stats.max_ms << ","
                    << stats.checksum << "\n";
        } else {
          std::cout << std::fixed << std::setprecision(3);
          std::cout << std::left << std::setw(18) << BackendName(backend)
                    << std::right << std::setw(7) << size.width << "x"
                    << std::left << std::setw(4) << size.height << std::right
                    << std::setw(12) << stats.avg_ms << std::setw(12)
                    << stats.p95_ms << std::setw(12) << stats.p99_ms
                    << std::setw(12) << stats.min_ms << std::setw(12)
                    << stats.max_ms << "\n";
        }
      }
    }
  }

  if (run_resize && !csv) {
    std::cout << "\nRGB24 CPU resize\n";
    std::cout << std::left << std::setw(18) << "backend" << std::right
              << std::setw(21) << "size" << std::setw(12) << "avg ms"
              << std::setw(12) << "p95 ms" << std::setw(12) << "p99 ms"
              << std::setw(12) << "min ms" << std::setw(12) << "max ms"
              << "\n";
  }

  if (run_resize) {
    for (bool original : {true, false}) {
      const char *name = original ? "previous-loop" : "current";
      for (const ResizeCase &size : resize_sizes) {
        const Stats stats = RunResizeBench(original, size, iterations, warmup);
        if (csv) {
          std::cout << "rgb24-resize-" << size.src_width << "x"
                    << size.src_height << "-to-" << size.dst_width << "x"
                    << size.dst_height << "," << name << "," << size.dst_width
                    << "," << size.dst_height << "," << stats.avg_ms << ","
                    << stats.p95_ms << "," << stats.p99_ms << ","
                    << stats.min_ms << "," << stats.max_ms << ","
                    << stats.checksum << "\n";
        } else {
          std::cout << std::fixed << std::setprecision(3);
          std::cout << std::left << std::setw(18) << name << std::right
                    << std::setw(7) << size.src_width << "x" << std::left
                    << std::setw(4) << size.src_height << "->" << std::right
                    << std::setw(5) << size.dst_width << "x" << std::left
                    << std::setw(4) << size.dst_height << std::right
                    << std::setw(12) << stats.avg_ms << std::setw(12)
                    << stats.p95_ms << std::setw(12) << stats.p99_ms
                    << std::setw(12) << stats.min_ms << std::setw(12)
                    << stats.max_ms << "\n";
        }
      }
    }
  }

  if (run_copy && !csv) {
    std::cout << "\nRGB24 padded output copy\n";
    std::cout << std::left << std::setw(18) << "backend" << std::right
              << std::setw(12) << "size" << std::setw(12) << "avg ms"
              << std::setw(12) << "p95 ms" << std::setw(12) << "p99 ms"
              << std::setw(12) << "min ms" << std::setw(12) << "max ms"
              << "\n";
  }

  if (run_copy) {
    for (bool original : {true, false}) {
      const char *name = original ? "previous-loop" : "current";
      for (const SizeCase &size : sizes) {
        const Stats stats =
            RunPaddedRgbCopyBench(original, size, iterations, warmup);
        if (csv) {
          std::cout << "rgb24-padded-copy," << name << "," << size.width << ","
                    << size.height << "," << stats.avg_ms << "," << stats.p95_ms
                    << "," << stats.p99_ms << "," << stats.min_ms << ","
                    << stats.max_ms << "," << stats.checksum << "\n";
        } else {
          std::cout << std::fixed << std::setprecision(3);
          std::cout << std::left << std::setw(18) << name << std::right
                    << std::setw(7) << size.width << "x" << std::left
                    << std::setw(4) << size.height << std::right
                    << std::setw(12) << stats.avg_ms << std::setw(12)
                    << stats.p95_ms << std::setw(12) << stats.p99_ms
                    << std::setw(12) << stats.min_ms << std::setw(12)
                    << stats.max_ms << "\n";
        }
      }
    }
  }

  return 0;
}
