#include "convert_rgb_yuyv_internal.h"

#include <cstddef>
#include <cstdint>
#include <tmmintrin.h>

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

inline __m128i YBytes8(__m128i r8, __m128i g8, __m128i b8) {
  const __m128i zero = _mm_setzero_si128();
  const __m128i r = _mm_unpacklo_epi8(r8, zero);
  const __m128i g = _mm_unpacklo_epi8(g8, zero);
  const __m128i b = _mm_unpacklo_epi8(b8, zero);

  __m128i y = _mm_add_epi16(_mm_mullo_epi16(r, _mm_set1_epi16(66)),
                            _mm_mullo_epi16(g, _mm_set1_epi16(129)));
  y = _mm_add_epi16(y, _mm_mullo_epi16(b, _mm_set1_epi16(25)));
  y = _mm_add_epi16(y, _mm_set1_epi16(128));
  y = _mm_srli_epi16(y, 8);
  y = _mm_add_epi16(y, _mm_set1_epi16(16));
  return _mm_packus_epi16(y, y);
}

inline __m128i ChromaBytes8(__m128i r8, __m128i g8, __m128i b8, bool want_u) {
  const __m128i zero = _mm_setzero_si128();
  const __m128i r = _mm_unpacklo_epi8(r8, zero);
  const __m128i g = _mm_unpacklo_epi8(g8, zero);
  const __m128i b = _mm_unpacklo_epi8(b8, zero);
  const __m128i r_pair = _mm_hadd_epi16(r, r);
  const __m128i g_pair = _mm_hadd_epi16(g, g);
  const __m128i b_pair = _mm_hadd_epi16(b, b);

  const __m128i rg = _mm_unpacklo_epi16(r_pair, g_pair);
  const __m128i bz = _mm_unpacklo_epi16(b_pair, zero);

  const __m128i rg_coeff =
      want_u ? _mm_setr_epi16(-38, -74, -38, -74, -38, -74, -38, -74)
             : _mm_setr_epi16(112, -94, 112, -94, 112, -94, 112, -94);
  const __m128i b_coeff = want_u
                              ? _mm_setr_epi16(112, 0, 112, 0, 112, 0, 112, 0)
                              : _mm_setr_epi16(-18, 0, -18, 0, -18, 0, -18, 0);

  __m128i chroma =
      _mm_add_epi32(_mm_madd_epi16(rg, rg_coeff), _mm_madd_epi16(bz, b_coeff));
  chroma = _mm_add_epi32(chroma, _mm_set1_epi32(256));
  chroma = _mm_srai_epi32(chroma, 9);
  chroma = _mm_add_epi32(chroma, _mm_set1_epi32(128));

  const __m128i chroma16 = _mm_packs_epi32(chroma, chroma);
  return _mm_packus_epi16(chroma16, chroma16);
}

inline void StoreYuyv8(const std::uint8_t *src, std::uint8_t *dst) {
  const Rgb8 rgb = LoadRgb8(src);
  const __m128i y = YBytes8(rgb.r, rgb.g, rgb.b);
  const __m128i u = ChromaBytes8(rgb.r, rgb.g, rgb.b, true);
  const __m128i v = ChromaBytes8(rgb.r, rgb.g, rgb.b, false);
  const __m128i chroma = _mm_unpacklo_epi8(u, v);
  const __m128i yuyv = _mm_unpacklo_epi8(y, chroma);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), yuyv);
}

} // namespace

void Rgb24ToYuyvSsse3(const std::uint8_t *src, int width, int height,
                      std::size_t src_stride, std::uint8_t *dst,
                      std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  for (int y = 0; y < height; ++y) {
    const std::uint8_t *src_row =
        src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *dst_row = dst + static_cast<std::size_t>(y) * dst_stride;

    int x = 0;
    for (; x + 7 < width; x += 8) {
      StoreYuyv8(src_row + static_cast<std::size_t>(x) * 3u,
                 dst_row + static_cast<std::size_t>(x) * 2u);
    }

    if (x < width) {
      Rgb24ToYuyvScalar(src_row + static_cast<std::size_t>(x) * 3u, width - x,
                        1, src_stride,
                        dst_row + static_cast<std::size_t>(x) * 2u, dst_stride);
    }
  }
}

} // namespace studiocast::video::internal
