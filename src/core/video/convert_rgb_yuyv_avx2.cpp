#include "convert_rgb_yuyv_internal.h"

#include <cstddef>
#include <cstdint>
#include <immintrin.h>

namespace studiocast::video::internal {
namespace {

struct Rgb8 {
  __m128i r;
  __m128i g;
  __m128i b;
};

inline Rgb8 LoadRgb8(const std::uint8_t *src) {
  constexpr char x = static_cast<char>(-128);
  const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 0));
  const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 8));

  const __m128i r_a =
      _mm_setr_epi8(0, 3, 6, 9, 12, 15, x, x, x, x, x, x, x, x, x, x);
  const __m128i r_b =
      _mm_setr_epi8(x, x, x, x, x, x, 10, 13, x, x, x, x, x, x, x, x);
  const __m128i g_a =
      _mm_setr_epi8(1, 4, 7, 10, 13, x, x, x, x, x, x, x, x, x, x, x);
  const __m128i g_b =
      _mm_setr_epi8(x, x, x, x, x, 8, 11, 14, x, x, x, x, x, x, x, x);
  const __m128i b_a =
      _mm_setr_epi8(2, 5, 8, 11, 14, x, x, x, x, x, x, x, x, x, x, x);
  const __m128i b_b =
      _mm_setr_epi8(x, x, x, x, x, 9, 12, 15, x, x, x, x, x, x, x, x);

  Rgb8 out{};
  out.r = _mm_or_si128(_mm_shuffle_epi8(a, r_a), _mm_shuffle_epi8(b, r_b));
  out.g = _mm_or_si128(_mm_shuffle_epi8(a, g_a), _mm_shuffle_epi8(b, g_b));
  out.b = _mm_or_si128(_mm_shuffle_epi8(a, b_a), _mm_shuffle_epi8(b, b_b));
  return out;
}

inline __m128i CombineRgb8(__m128i lo, __m128i hi) {
  return _mm_unpacklo_epi64(lo, hi);
}

inline __m128i YBytes16(__m256i r, __m256i g, __m256i b) {
  __m256i y = _mm256_add_epi16(_mm256_mullo_epi16(r, _mm256_set1_epi16(66)),
                               _mm256_mullo_epi16(g, _mm256_set1_epi16(129)));
  y = _mm256_add_epi16(y, _mm256_mullo_epi16(b, _mm256_set1_epi16(25)));
  y = _mm256_add_epi16(y, _mm256_set1_epi16(128));
  y = _mm256_srli_epi16(y, 8);
  y = _mm256_add_epi16(y, _mm256_set1_epi16(16));

  const __m256i packed = _mm256_packus_epi16(y, y);
  const __m128i lo = _mm256_castsi256_si128(packed);
  const __m128i hi = _mm256_extracti128_si256(packed, 1);
  return _mm_unpacklo_epi64(lo, hi);
}

inline __m128i ChromaBytes16(__m256i r, __m256i g, __m256i b, bool want_u) {
  const __m256i ones = _mm256_set1_epi16(1);
  const __m256i r_pair = _mm256_madd_epi16(r, ones);
  const __m256i g_pair = _mm256_madd_epi16(g, ones);
  const __m256i b_pair = _mm256_madd_epi16(b, ones);

  const __m256i r_coeff =
      want_u ? _mm256_set1_epi32(-38) : _mm256_set1_epi32(112);
  const __m256i g_coeff =
      want_u ? _mm256_set1_epi32(-74) : _mm256_set1_epi32(-94);
  const __m256i b_coeff =
      want_u ? _mm256_set1_epi32(112) : _mm256_set1_epi32(-18);

  __m256i chroma = _mm256_add_epi32(_mm256_mullo_epi32(r_pair, r_coeff),
                                    _mm256_mullo_epi32(g_pair, g_coeff));
  chroma = _mm256_add_epi32(chroma, _mm256_mullo_epi32(b_pair, b_coeff));
  chroma = _mm256_add_epi32(chroma, _mm256_set1_epi32(256));
  chroma = _mm256_srai_epi32(chroma, 9);
  chroma = _mm256_add_epi32(chroma, _mm256_set1_epi32(128));

  const __m256i chroma16 = _mm256_packs_epi32(chroma, chroma);
  const __m128i lo = _mm256_castsi256_si128(chroma16);
  const __m128i hi = _mm256_extracti128_si256(chroma16, 1);
  const __m128i chroma16_ordered = _mm_unpacklo_epi64(lo, hi);
  return _mm_packus_epi16(chroma16_ordered, chroma16_ordered);
}

inline void StoreYuyv16(const std::uint8_t *src, std::uint8_t *dst) {
  const Rgb8 lo = LoadRgb8(src);
  const Rgb8 hi = LoadRgb8(src + 24);

  const __m256i r = _mm256_cvtepu8_epi16(CombineRgb8(lo.r, hi.r));
  const __m256i g = _mm256_cvtepu8_epi16(CombineRgb8(lo.g, hi.g));
  const __m256i b = _mm256_cvtepu8_epi16(CombineRgb8(lo.b, hi.b));

  const __m128i y = YBytes16(r, g, b);
  const __m128i u = ChromaBytes16(r, g, b, true);
  const __m128i v = ChromaBytes16(r, g, b, false);
  const __m128i chroma = _mm_unpacklo_epi8(u, v);

  const __m128i yuyv_lo = _mm_unpacklo_epi8(y, chroma);
  const __m128i yuyv_hi =
      _mm_unpacklo_epi8(_mm_srli_si128(y, 8), _mm_srli_si128(chroma, 8));
  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), yuyv_lo);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 16), yuyv_hi);
}

} // namespace

void Rgb24ToYuyvAvx2(const std::uint8_t *src, int width, int height,
                     std::size_t src_stride, std::uint8_t *dst,
                     std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  for (int y = 0; y < height; ++y) {
    const std::uint8_t *src_row =
        src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *dst_row = dst + static_cast<std::size_t>(y) * dst_stride;

    int x = 0;
    for (; x + 15 < width; x += 16) {
      StoreYuyv16(src_row + static_cast<std::size_t>(x) * 3u,
                  dst_row + static_cast<std::size_t>(x) * 2u);
    }

    if (x < width) {
      Rgb24ToYuyvSsse3(src_row + static_cast<std::size_t>(x) * 3u, width - x, 1,
                       src_stride, dst_row + static_cast<std::size_t>(x) * 2u,
                       dst_stride);
    }
  }
}

} // namespace studiocast::video::internal
