#include "core/video/convert.h"
#include "core/video/convert_rgb_yuyv_internal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace studiocast::tests {
namespace {

inline int RgbToY(int r, int g, int b) {
  return ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
}

inline int RgbToU(int r, int g, int b) {
  return ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
}

inline int RgbToV(int r, int g, int b) {
  return ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

struct ConvertCase {
  int width;
  int height;
  std::size_t src_padding;
  std::size_t dst_padding;
};

struct BackendCase {
  video::internal::Rgb24ToYuyvBackend backend;
  int chroma_tolerance;
};

constexpr std::size_t ActiveYuyvBytes(int width) {
  return static_cast<std::size_t>((width + 1) / 2) * 4u;
}

void FillDeterministicRgb(std::vector<std::uint8_t> *src, int width, int height,
                          std::size_t stride, std::uint32_t seed) {
  std::uint32_t state = seed;
  for (int y = 0; y < height; ++y) {
    std::uint8_t *row = src->data() + static_cast<std::size_t>(y) * stride;
    for (std::size_t x = 0; x < static_cast<std::size_t>(width) * 3u; ++x) {
      state = state * 1664525u + 1013904223u;
      row[x] = static_cast<std::uint8_t>((state >> 24) & 0xffu);
    }
    for (std::size_t x = static_cast<std::size_t>(width) * 3u; x < stride;
         ++x) {
      row[x] = static_cast<std::uint8_t>(0xa5u);
    }
  }
}

bool ConvertWithBackend(video::internal::Rgb24ToYuyvBackend backend,
                        const std::uint8_t *src, int width, int height,
                        std::size_t src_stride, std::uint8_t *dst,
                        std::size_t dst_stride) {
  using video::internal::Rgb24ToYuyvBackend;

  if (!video::internal::Rgb24ToYuyvBackendAvailable(backend))
    return false;

  switch (backend) {
  case Rgb24ToYuyvBackend::scalar:
    video::internal::Rgb24ToYuyvScalar(src, width, height, src_stride, dst,
                                       dst_stride);
    return true;
  case Rgb24ToYuyvBackend::libyuv: {
    const std::size_t scratch_size =
        video::internal::Rgb24ToYuyvLibyuvScratchBytes(width, height);
    std::vector<std::uint8_t> scratch(scratch_size);
    return video::internal::Rgb24ToYuyvLibyuv(src, width, height, src_stride,
                                              dst, dst_stride, scratch.data(),
                                              scratch.size());
  }
  case Rgb24ToYuyvBackend::ssse3:
    video::internal::Rgb24ToYuyvSsse3(src, width, height, src_stride, dst,
                                      dst_stride);
    return true;
  case Rgb24ToYuyvBackend::avx2:
    video::internal::Rgb24ToYuyvAvx2(src, width, height, src_stride, dst,
                                     dst_stride);
    return true;
  }

  return false;
}

bool CompareYuyvToReference(const std::vector<std::uint8_t> &actual,
                            const std::vector<std::uint8_t> &expected,
                            int width, int height, std::size_t dst_stride,
                            int chroma_tolerance, const std::string &label) {
  const std::size_t active = ActiveYuyvBytes(width);
  for (int y = 0; y < height; ++y) {
    const std::uint8_t *a =
        actual.data() + static_cast<std::size_t>(y) * dst_stride;
    const std::uint8_t *e =
        expected.data() + static_cast<std::size_t>(y) * dst_stride;

    for (int x = 0; x < width; x += 2) {
      const std::size_t out = static_cast<std::size_t>(x) * 2u;
      if (a[out + 0] != e[out + 0] || a[out + 2] != e[out + 2]) {
        std::cerr << label << " luma mismatch at " << x << "," << y << ": "
                  << static_cast<int>(a[out + 0]) << "/"
                  << static_cast<int>(a[out + 2]) << " expected "
                  << static_cast<int>(e[out + 0]) << "/"
                  << static_cast<int>(e[out + 2]) << "\n";
        return false;
      }

      const int u_delta =
          std::abs(static_cast<int>(a[out + 1]) - static_cast<int>(e[out + 1]));
      const int v_delta =
          std::abs(static_cast<int>(a[out + 3]) - static_cast<int>(e[out + 3]));
      if (u_delta > chroma_tolerance || v_delta > chroma_tolerance) {
        std::cerr << label << " chroma mismatch at " << x << "," << y
                  << ": delta U=" << u_delta << " V=" << v_delta
                  << " tolerance=" << chroma_tolerance << "\n";
        return false;
      }
    }

    for (std::size_t i = active; i < dst_stride; ++i) {
      if (a[i] != 0xcdu) {
        std::cerr << label << " overwrote destination padding at row " << y
                  << " byte " << i << "\n";
        return false;
      }
    }
  }

  return true;
}

} // namespace

bool TestRgb24ToYuyvMatchesBt601WithinChromaRounding() {
  constexpr int width = 17;
  constexpr int height = 11;
  constexpr std::size_t src_stride = width * 3 + 5;
  constexpr std::size_t dst_stride = width * 2 + 8;

  std::vector<std::uint8_t> src(src_stride * height);
  std::vector<std::uint8_t> dst(dst_stride * height, 0xcd);

  for (std::size_t i = 0; i < src.size(); ++i)
    src[i] = static_cast<std::uint8_t>((i * 37 + (i / 5) * 17 + 91) & 0xff);

  video::Rgb24ToYuyv(src.data(), width, height, src_stride, dst.data(),
                     dst_stride);

  int max_chroma_delta = 0;
  for (int y = 0; y < height; ++y) {
    const std::uint8_t *s =
        src.data() + static_cast<std::size_t>(y) * src_stride;
    const std::uint8_t *d =
        dst.data() + static_cast<std::size_t>(y) * dst_stride;

    for (int x = 0; x < width; x += 2) {
      const int r0 = s[static_cast<std::size_t>(x) * 3u + 0];
      const int g0 = s[static_cast<std::size_t>(x) * 3u + 1];
      const int b0 = s[static_cast<std::size_t>(x) * 3u + 2];

      int r1 = r0;
      int g1 = g0;
      int b1 = b0;
      if (x + 1 < width) {
        r1 = s[static_cast<std::size_t>(x + 1) * 3u + 0];
        g1 = s[static_cast<std::size_t>(x + 1) * 3u + 1];
        b1 = s[static_cast<std::size_t>(x + 1) * 3u + 2];
      }

      const int expected_y0 = RgbToY(r0, g0, b0);
      const int expected_y1 = RgbToY(r1, g1, b1);
      const int expected_u = (RgbToU(r0, g0, b0) + RgbToU(r1, g1, b1)) / 2;
      const int expected_v = (RgbToV(r0, g0, b0) + RgbToV(r1, g1, b1)) / 2;

      const std::size_t out = static_cast<std::size_t>(x) * 2u;
      if (d[out + 0] != expected_y0 || d[out + 2] != expected_y1) {
        std::cerr << "YUYV luma mismatch at " << x << "," << y << "\n";
        return false;
      }

      max_chroma_delta =
          std::max(max_chroma_delta,
                   std::abs(static_cast<int>(d[out + 1]) - expected_u));
      max_chroma_delta =
          std::max(max_chroma_delta,
                   std::abs(static_cast<int>(d[out + 3]) - expected_v));
    }
  }

  if (max_chroma_delta > 1) {
    std::cerr << "YUYV chroma delta exceeded rounding tolerance: "
              << max_chroma_delta << "\n";
    return false;
  }

  return true;
}

bool TestRgb24ToYuyvBackendsMatchScalarReference() {
  using video::internal::Rgb24ToYuyvBackend;

  const std::array<ConvertCase, 11> cases{{
      {1, 3, 5, 8},
      {2, 3, 4, 5},
      {7, 5, 1, 7},
      {8, 5, 8, 3},
      {9, 5, 2, 11},
      {15, 4, 5, 6},
      {16, 4, 3, 9},
      {17, 11, 5, 8},
      {640, 480, 13, 16},
      {1280, 720, 7, 32},
      {1920, 1080, 5, 64},
  }};

  const std::array<BackendCase, 4> backends{{
      {Rgb24ToYuyvBackend::scalar, 0},
      {Rgb24ToYuyvBackend::ssse3, 0},
      {Rgb24ToYuyvBackend::avx2, 0},
      {Rgb24ToYuyvBackend::libyuv, 1},
  }};

  for (const ConvertCase &c : cases) {
    const std::size_t src_stride =
        static_cast<std::size_t>(c.width) * 3u + c.src_padding;
    const std::size_t dst_stride = ActiveYuyvBytes(c.width) + c.dst_padding;
    std::vector<std::uint8_t> src(src_stride *
                                  static_cast<std::size_t>(c.height));
    FillDeterministicRgb(
        &src, c.width, c.height, src_stride,
        static_cast<std::uint32_t>(c.width * 131 + c.height * 17));

    std::vector<std::uint8_t> expected(
        dst_stride * static_cast<std::size_t>(c.height), 0xcd);
    video::internal::Rgb24ToYuyvScalar(src.data(), c.width, c.height,
                                       src_stride, expected.data(), dst_stride);

    for (const BackendCase &backend : backends) {
      if (!video::internal::Rgb24ToYuyvBackendAvailable(backend.backend))
        continue;

      std::vector<std::uint8_t> actual(
          dst_stride * static_cast<std::size_t>(c.height), 0xcd);
      if (!ConvertWithBackend(backend.backend, src.data(), c.width, c.height,
                              src_stride, actual.data(), dst_stride)) {
        std::cerr << "Backend "
                  << video::internal::Rgb24ToYuyvBackendName(backend.backend)
                  << " reported available but conversion failed\n";
        return false;
      }

      const std::string label =
          std::string(
              video::internal::Rgb24ToYuyvBackendName(backend.backend)) +
          " " + std::to_string(c.width) + "x" + std::to_string(c.height);
      if (!CompareYuyvToReference(actual, expected, c.width, c.height,
                                  dst_stride, backend.chroma_tolerance,
                                  label)) {
        return false;
      }
    }
  }

  return true;
}

bool TestRgb24ToYuyvPublicPathMatchesScalarWithScratchVariants() {
  constexpr int width = 31;
  constexpr int height = 9;
  constexpr std::size_t src_stride = width * 3 + 7;
  constexpr std::size_t dst_stride = ActiveYuyvBytes(width) + 13;

  std::vector<std::uint8_t> src(src_stride * height);
  FillDeterministicRgb(&src, width, height, src_stride, 0x5eed1234u);

  std::vector<std::uint8_t> expected(dst_stride * height, 0xcd);
  video::internal::Rgb24ToYuyvScalar(src.data(), width, height, src_stride,
                                     expected.data(), dst_stride);

  {
    std::vector<std::uint8_t> actual(dst_stride * height, 0xcd);
    video::Rgb24ToYuyv(src.data(), width, height, src_stride, actual.data(),
                       dst_stride);
    const int chroma_tolerance =
        video::internal::Rgb24ToYuyvSelectedBackend() ==
                video::internal::Rgb24ToYuyvBackend::libyuv
            ? 1
            : 0;
    if (!CompareYuyvToReference(actual, expected, width, height, dst_stride,
                                chroma_tolerance, "public no-scratch")) {
      return false;
    }
  }

  {
    std::vector<std::uint8_t> actual(dst_stride * height, 0xcd);
    std::array<std::uint8_t, 1> undersized_scratch{0};
    video::Rgb24ToYuyvWithScratch(
        src.data(), width, height, src_stride, actual.data(), dst_stride,
        undersized_scratch.data(), undersized_scratch.size());
    if (!CompareYuyvToReference(actual, expected, width, height, dst_stride, 0,
                                "public undersized scratch")) {
      return false;
    }
  }

  {
    std::vector<std::uint8_t> actual(dst_stride * height, 0xcd);
    const std::size_t scratch_size =
        video::Rgb24ToYuyvScratchBytes(width, height);
    std::vector<std::uint8_t> scratch(scratch_size);
    video::Rgb24ToYuyvWithScratch(
        src.data(), width, height, src_stride, actual.data(), dst_stride,
        scratch.empty() ? nullptr : scratch.data(), scratch.size());
    const int chroma_tolerance =
        video::internal::Rgb24ToYuyvSelectedBackend() ==
                video::internal::Rgb24ToYuyvBackend::libyuv
            ? 1
            : 0;
    if (!CompareYuyvToReference(actual, expected, width, height, dst_stride,
                                chroma_tolerance, "public selected scratch")) {
      return false;
    }
  }

  return true;
}

} // namespace studiocast::tests
