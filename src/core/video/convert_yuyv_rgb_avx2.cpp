#include "convert_yuyv_rgb_internal.h"

#include <cstddef>
#include <cstdint>

#include <immintrin.h>

namespace studiocast::video::internal {
namespace {

inline __m256i ShiftRounded8(__m256i value) {
  return _mm256_srai_epi32(_mm256_add_epi32(value, _mm256_set1_epi32(128)), 8);
}

inline __m256i ConvertR(__m256i y, __m256i v) {
  return ShiftRounded8(
      _mm256_add_epi32(_mm256_mullo_epi32(y, _mm256_set1_epi32(298)),
                       _mm256_mullo_epi32(v, _mm256_set1_epi32(409))));
}

inline __m256i ConvertG(__m256i y, __m256i u, __m256i v) {
  return ShiftRounded8(_mm256_add_epi32(
      _mm256_add_epi32(_mm256_mullo_epi32(y, _mm256_set1_epi32(298)),
                       _mm256_mullo_epi32(u, _mm256_set1_epi32(-100))),
      _mm256_mullo_epi32(v, _mm256_set1_epi32(-208))));
}

inline __m256i ConvertB(__m256i y, __m256i u) {
  return ShiftRounded8(
      _mm256_add_epi32(_mm256_mullo_epi32(y, _mm256_set1_epi32(298)),
                       _mm256_mullo_epi32(u, _mm256_set1_epi32(516))));
}

inline __m128i PackToU8(__m256i lo, __m256i hi) {
  const __m128i lo16 = _mm_packs_epi32(_mm256_castsi256_si128(lo),
                                       _mm256_extracti128_si256(lo, 1));
  const __m128i hi16 = _mm_packs_epi32(_mm256_castsi256_si128(hi),
                                       _mm256_extracti128_si256(hi, 1));
  return _mm_packus_epi16(lo16, hi16);
}

inline void StoreRgb16(__m128i r8, __m128i g8, __m128i b8,
                       std::uint8_t *dst) {
  alignas(16) std::uint8_t r[16];
  alignas(16) std::uint8_t g[16];
  alignas(16) std::uint8_t b[16];
  _mm_store_si128(reinterpret_cast<__m128i *>(r), r8);
  _mm_store_si128(reinterpret_cast<__m128i *>(g), g8);
  _mm_store_si128(reinterpret_cast<__m128i *>(b), b8);

  for (int i = 0; i < 16; ++i) {
    dst[static_cast<std::size_t>(i) * 3u + 0u] = r[i];
    dst[static_cast<std::size_t>(i) * 3u + 1u] = g[i];
    dst[static_cast<std::size_t>(i) * 3u + 2u] = b[i];
  }
}

} // namespace

void YuyvToRgbAvx2(const std::uint8_t *src, int width, int height,
                   std::size_t src_stride, std::uint8_t *dst,
                   std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  const __m256i zero = _mm256_setzero_si256();
  const __m256i y_shuffle = _mm256_setr_epi8(
      0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1, 0, 2, 4, 6,
      8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1);
  const __m256i u_shuffle = _mm256_setr_epi8(
      1, 1, 5, 5, 9, 9, 13, 13, -1, -1, -1, -1, -1, -1, -1, -1, 1, 1, 5, 5,
      9, 9, 13, 13, -1, -1, -1, -1, -1, -1, -1, -1);
  const __m256i v_shuffle = _mm256_setr_epi8(
      3, 3, 7, 7, 11, 11, 15, 15, -1, -1, -1, -1, -1, -1, -1, -1, 3, 3, 7,
      7, 11, 11, 15, 15, -1, -1, -1, -1, -1, -1, -1, -1);
  const __m256i k16 = _mm256_set1_epi16(16);
  const __m256i k128 = _mm256_set1_epi16(128);

  for (int row = 0; row < height; ++row) {
    const std::uint8_t *s =
        src + static_cast<std::size_t>(row) * src_stride;
    std::uint8_t *d = dst + static_cast<std::size_t>(row) * dst_stride;

    int x = 0;
    for (; x + 15 < width; x += 16) {
      const __m256i yuyv =
          _mm256_loadu_si256(reinterpret_cast<const __m256i *>(s));
      s += 32;

      __m256i y16 = _mm256_sub_epi16(
          _mm256_unpacklo_epi8(_mm256_shuffle_epi8(yuyv, y_shuffle), zero),
          k16);
      y16 = _mm256_max_epi16(y16, zero);
      const __m256i u16 = _mm256_sub_epi16(
          _mm256_unpacklo_epi8(_mm256_shuffle_epi8(yuyv, u_shuffle), zero),
          k128);
      const __m256i v16 = _mm256_sub_epi16(
          _mm256_unpacklo_epi8(_mm256_shuffle_epi8(yuyv, v_shuffle), zero),
          k128);

      const __m128i y16_lo = _mm256_castsi256_si128(y16);
      const __m128i y16_hi = _mm256_extracti128_si256(y16, 1);
      const __m128i u16_lo = _mm256_castsi256_si128(u16);
      const __m128i u16_hi = _mm256_extracti128_si256(u16, 1);
      const __m128i v16_lo = _mm256_castsi256_si128(v16);
      const __m128i v16_hi = _mm256_extracti128_si256(v16, 1);

      const __m256i y_lo = _mm256_cvtepi16_epi32(y16_lo);
      const __m256i y_hi = _mm256_cvtepi16_epi32(y16_hi);
      const __m256i u_lo = _mm256_cvtepi16_epi32(u16_lo);
      const __m256i u_hi = _mm256_cvtepi16_epi32(u16_hi);
      const __m256i v_lo = _mm256_cvtepi16_epi32(v16_lo);
      const __m256i v_hi = _mm256_cvtepi16_epi32(v16_hi);

      const __m128i r8 = PackToU8(ConvertR(y_lo, v_lo), ConvertR(y_hi, v_hi));
      const __m128i g8 =
          PackToU8(ConvertG(y_lo, u_lo, v_lo), ConvertG(y_hi, u_hi, v_hi));
      const __m128i b8 = PackToU8(ConvertB(y_lo, u_lo), ConvertB(y_hi, u_hi));

      StoreRgb16(r8, g8, b8, d);
      d += 48;
    }

    if (x < width) {
      YuyvToRgbSse41(s, width - x, 1, src_stride, d, dst_stride);
    }
  }
}

} // namespace studiocast::video::internal
