#include "convert_yuyv_rgb_internal.h"

#include <cstddef>
#include <cstdint>

#include <smmintrin.h>

namespace studiocast::video::internal {
namespace {

inline __m128i ShiftRounded8(__m128i value) {
  return _mm_srai_epi32(_mm_add_epi32(value, _mm_set1_epi32(128)), 8);
}

inline __m128i ConvertR(__m128i y, __m128i v) {
  return ShiftRounded8(_mm_add_epi32(_mm_mullo_epi32(y, _mm_set1_epi32(298)),
                                     _mm_mullo_epi32(v, _mm_set1_epi32(409))));
}

inline __m128i ConvertG(__m128i y, __m128i u, __m128i v) {
  return ShiftRounded8(_mm_add_epi32(
      _mm_add_epi32(_mm_mullo_epi32(y, _mm_set1_epi32(298)),
                    _mm_mullo_epi32(u, _mm_set1_epi32(-100))),
      _mm_mullo_epi32(v, _mm_set1_epi32(-208))));
}

inline __m128i ConvertB(__m128i y, __m128i u) {
  return ShiftRounded8(_mm_add_epi32(_mm_mullo_epi32(y, _mm_set1_epi32(298)),
                                     _mm_mullo_epi32(u, _mm_set1_epi32(516))));
}

inline void StoreRgb8(__m128i r16, __m128i g16, __m128i b16,
                      std::uint8_t *dst) {
  const __m128i zero = _mm_setzero_si128();
  const __m128i r8 = _mm_packus_epi16(r16, zero);
  const __m128i g8 = _mm_packus_epi16(g16, zero);
  const __m128i b8 = _mm_packus_epi16(b16, zero);

  alignas(16) std::uint8_t r[16];
  alignas(16) std::uint8_t g[16];
  alignas(16) std::uint8_t b[16];
  _mm_store_si128(reinterpret_cast<__m128i *>(r), r8);
  _mm_store_si128(reinterpret_cast<__m128i *>(g), g8);
  _mm_store_si128(reinterpret_cast<__m128i *>(b), b8);

  for (int i = 0; i < 8; ++i) {
    dst[static_cast<std::size_t>(i) * 3u + 0u] = r[i];
    dst[static_cast<std::size_t>(i) * 3u + 1u] = g[i];
    dst[static_cast<std::size_t>(i) * 3u + 2u] = b[i];
  }
}

} // namespace

void YuyvToRgbSse41(const std::uint8_t *src, int width, int height,
                    std::size_t src_stride, std::uint8_t *dst,
                    std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  const __m128i zero = _mm_setzero_si128();
  const __m128i y_shuffle = _mm_setr_epi8(
      0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1);
  const __m128i u_shuffle = _mm_setr_epi8(
      1, 1, 5, 5, 9, 9, 13, 13, -1, -1, -1, -1, -1, -1, -1, -1);
  const __m128i v_shuffle = _mm_setr_epi8(
      3, 3, 7, 7, 11, 11, 15, 15, -1, -1, -1, -1, -1, -1, -1, -1);
  const __m128i k16 = _mm_set1_epi16(16);
  const __m128i k128 = _mm_set1_epi16(128);

  for (int row = 0; row < height; ++row) {
    const std::uint8_t *s =
        src + static_cast<std::size_t>(row) * src_stride;
    std::uint8_t *d = dst + static_cast<std::size_t>(row) * dst_stride;

    int x = 0;
    for (; x + 7 < width; x += 8) {
      const __m128i yuyv =
          _mm_loadu_si128(reinterpret_cast<const __m128i *>(s));
      s += 16;

      __m128i y16 =
          _mm_sub_epi16(_mm_unpacklo_epi8(_mm_shuffle_epi8(yuyv, y_shuffle),
                                          zero),
                        k16);
      y16 = _mm_max_epi16(y16, zero);
      const __m128i u16 =
          _mm_sub_epi16(_mm_unpacklo_epi8(_mm_shuffle_epi8(yuyv, u_shuffle),
                                          zero),
                        k128);
      const __m128i v16 =
          _mm_sub_epi16(_mm_unpacklo_epi8(_mm_shuffle_epi8(yuyv, v_shuffle),
                                          zero),
                        k128);

      const __m128i y_lo = _mm_cvtepi16_epi32(y16);
      const __m128i y_hi = _mm_cvtepi16_epi32(_mm_srli_si128(y16, 8));
      const __m128i u_lo = _mm_cvtepi16_epi32(u16);
      const __m128i u_hi = _mm_cvtepi16_epi32(_mm_srli_si128(u16, 8));
      const __m128i v_lo = _mm_cvtepi16_epi32(v16);
      const __m128i v_hi = _mm_cvtepi16_epi32(_mm_srli_si128(v16, 8));

      const __m128i r16 =
          _mm_packs_epi32(ConvertR(y_lo, v_lo), ConvertR(y_hi, v_hi));
      const __m128i g16 = _mm_packs_epi32(ConvertG(y_lo, u_lo, v_lo),
                                          ConvertG(y_hi, u_hi, v_hi));
      const __m128i b16 =
          _mm_packs_epi32(ConvertB(y_lo, u_lo), ConvertB(y_hi, u_hi));

      StoreRgb8(r16, g16, b16, d);
      d += 24;
    }

    if (x < width) {
      YuyvToRgbScalar(s, width - x, 1, src_stride, d, dst_stride);
    }
  }
}

} // namespace studiocast::video::internal
