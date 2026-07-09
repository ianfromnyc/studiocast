#include "convert_rgb_bgr_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include <immintrin.h>

namespace studiocast::video::internal {
namespace {

using MaskBytes = std::array<std::uint8_t, 32>;

constexpr std::uint8_t z = 0x80u;

alignas(32) constexpr MaskBytes kOut0FromA{
    2, 1, 0, 5, 4, 3, 8, 7, 6, 11, 10, 9, 14, 13, 12, z,
    2, 1, 0, 5, 4, 3, 8, 7, 6, 11, 10, 9, 14, 13, 12, z};
alignas(32) constexpr MaskBytes kOut0FromB{
    z, z, z, z, z, z, z, z, z, z, z, z, z, z, z, 1,
    z, z, z, z, z, z, z, z, z, z, z, z, z, z, z, 1};

alignas(32) constexpr MaskBytes kOut1FromA{
    z, 15, z, z, z, z, z, z, z, z, z, z, z, z, z, z,
    z, 15, z, z, z, z, z, z, z, z, z, z, z, z, z, z};
alignas(32) constexpr MaskBytes kOut1FromB{
    0, z, 4, 3, 2, 7, 6, 5, 10, 9, 8, 13, 12, 11, z, 15,
    0, z, 4, 3, 2, 7, 6, 5, 10, 9, 8, 13, 12, 11, z, 15};
alignas(32) constexpr MaskBytes kOut1FromC{
    z, z, z, z, z, z, z, z, z, z, z, z, z, z, 0, z,
    z, z, z, z, z, z, z, z, z, z, z, z, z, z, 0, z};

alignas(32) constexpr MaskBytes kOut2FromB{
    14, z, z, z, z, z, z, z, z, z, z, z, z, z, z, z,
    14, z, z, z, z, z, z, z, z, z, z, z, z, z, z, z};
alignas(32) constexpr MaskBytes kOut2FromC{
    z, 3, 2, 1, 6, 5, 4, 9, 8, 7, 12, 11, 10, 15, 14, 13,
    z, 3, 2, 1, 6, 5, 4, 9, 8, 7, 12, 11, 10, 15, 14, 13};

struct ShuffleMasks {
  __m256i out0_from_a;
  __m256i out0_from_b;
  __m256i out1_from_a;
  __m256i out1_from_b;
  __m256i out1_from_c;
  __m256i out2_from_b;
  __m256i out2_from_c;
};

inline __m256i LoadMask(const MaskBytes &mask) {
  return _mm256_load_si256(reinterpret_cast<const __m256i *>(mask.data()));
}

inline ShuffleMasks LoadMasks() {
  return {
      LoadMask(kOut0FromA), LoadMask(kOut0FromB), LoadMask(kOut1FromA),
      LoadMask(kOut1FromB), LoadMask(kOut1FromC), LoadMask(kOut2FromB),
      LoadMask(kOut2FromC),
  };
}

inline const ShuffleMasks &Masks() {
  static const ShuffleMasks masks = LoadMasks();
  return masks;
}

inline void StoreBgr32(const std::uint8_t *src, std::uint8_t *dst,
                       const ShuffleMasks &m) {
  const __m256i in0 =
      _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src));
  const __m256i in1 =
      _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + 32));
  const __m256i in2 =
      _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + 64));

  const __m256i a = _mm256_permute2x128_si256(in0, in1, 0x30);
  const __m256i b = _mm256_permute2x128_si256(in0, in2, 0x21);
  const __m256i c = _mm256_permute2x128_si256(in1, in2, 0x30);

  const __m256i out0 =
      _mm256_or_si256(_mm256_shuffle_epi8(a, m.out0_from_a),
                      _mm256_shuffle_epi8(b, m.out0_from_b));
  const __m256i out1 = _mm256_or_si256(
      _mm256_or_si256(_mm256_shuffle_epi8(a, m.out1_from_a),
                      _mm256_shuffle_epi8(b, m.out1_from_b)),
      _mm256_shuffle_epi8(c, m.out1_from_c));
  const __m256i out2 =
      _mm256_or_si256(_mm256_shuffle_epi8(b, m.out2_from_b),
                      _mm256_shuffle_epi8(c, m.out2_from_c));

  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst),
                   _mm256_castsi256_si128(out0));
  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 16),
                   _mm256_castsi256_si128(out1));
  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 32),
                   _mm256_castsi256_si128(out2));
  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 48),
                   _mm256_extracti128_si256(out0, 1));
  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 64),
                   _mm256_extracti128_si256(out1, 1));
  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 80),
                   _mm256_extracti128_si256(out2, 1));
}

} // namespace

void Rgb24Bgr24Avx2(const std::uint8_t *src, std::uint8_t *dst, int width,
                    int height, std::size_t src_stride,
                    std::size_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0)
    return;

  const ShuffleMasks &masks = Masks();
  for (int y = 0; y < height; ++y) {
    const std::uint8_t *src_row =
        src + static_cast<std::size_t>(y) * src_stride;
    std::uint8_t *dst_row = dst + static_cast<std::size_t>(y) * dst_stride;

    int x = 0;
    for (; x + 31 < width; x += 32) {
      const std::size_t offset = static_cast<std::size_t>(x) * 3u;
      StoreBgr32(src_row + offset, dst_row + offset, masks);
    }

    if (x < width) {
      const std::size_t offset = static_cast<std::size_t>(x) * 3u;
      Rgb24Bgr24Ssse3(src_row + offset, dst_row + offset, width - x, 1,
                      src_stride, dst_stride);
    }
  }
}

} // namespace studiocast::video::internal
