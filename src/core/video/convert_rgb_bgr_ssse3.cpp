#include "convert_rgb_bgr_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include <tmmintrin.h>

namespace studiocast::video::internal {
namespace {

using MaskBytes = std::array<std::uint8_t, 16>;

constexpr std::uint8_t z = 0x80u;

alignas(16) constexpr MaskBytes kOut0From0{
    2, 1, 0, 5, 4, 3, 8, 7, 6, 11, 10, 9, 14, 13, 12, z};
alignas(16) constexpr MaskBytes kOut0From1{
    z, z, z, z, z, z, z, z, z, z, z, z, z, z, z, 1};

alignas(16) constexpr MaskBytes kOut1From0{
    z, 15, z, z, z, z, z, z, z, z, z, z, z, z, z, z};
alignas(16) constexpr MaskBytes kOut1From1{
    0, z, 4, 3, 2, 7, 6, 5, 10, 9, 8, 13, 12, 11, z, 15};
alignas(16) constexpr MaskBytes kOut1From2{
    z, z, z, z, z, z, z, z, z, z, z, z, z, z, 0, z};

alignas(16) constexpr MaskBytes kOut2From1{
    14, z, z, z, z, z, z, z, z, z, z, z, z, z, z, z};
alignas(16) constexpr MaskBytes kOut2From2{
    z, 3, 2, 1, 6, 5, 4, 9, 8, 7, 12, 11, 10, 15, 14, 13};

struct ShuffleMasks {
  __m128i out0_from0;
  __m128i out0_from1;
  __m128i out1_from0;
  __m128i out1_from1;
  __m128i out1_from2;
  __m128i out2_from1;
  __m128i out2_from2;
};

inline __m128i LoadMask(const MaskBytes &mask) {
  return _mm_load_si128(reinterpret_cast<const __m128i *>(mask.data()));
}

inline ShuffleMasks LoadMasks() {
  return {
      LoadMask(kOut0From0), LoadMask(kOut0From1), LoadMask(kOut1From0),
      LoadMask(kOut1From1), LoadMask(kOut1From2), LoadMask(kOut2From1),
      LoadMask(kOut2From2),
  };
}

inline const ShuffleMasks &Masks() {
  static const ShuffleMasks masks = LoadMasks();
  return masks;
}

inline void StoreBgr16(const std::uint8_t *src, std::uint8_t *dst,
                       const ShuffleMasks &m) {
  const __m128i in0 =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(src));
  const __m128i in1 =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 16));
  const __m128i in2 =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 32));

  const __m128i out0 =
      _mm_or_si128(_mm_shuffle_epi8(in0, m.out0_from0),
                   _mm_shuffle_epi8(in1, m.out0_from1));
  const __m128i out1 = _mm_or_si128(
      _mm_or_si128(_mm_shuffle_epi8(in0, m.out1_from0),
                   _mm_shuffle_epi8(in1, m.out1_from1)),
      _mm_shuffle_epi8(in2, m.out1_from2));
  const __m128i out2 =
      _mm_or_si128(_mm_shuffle_epi8(in1, m.out2_from1),
                   _mm_shuffle_epi8(in2, m.out2_from2));

  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), out0);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 16), out1);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 32), out2);
}

} // namespace

void Rgb24Bgr24Ssse3(const std::uint8_t *src, std::uint8_t *dst, int width,
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
    for (; x + 15 < width; x += 16) {
      const std::size_t offset = static_cast<std::size_t>(x) * 3u;
      StoreBgr16(src_row + offset, dst_row + offset, masks);
    }

    if (x < width) {
      const std::size_t offset = static_cast<std::size_t>(x) * 3u;
      Rgb24Bgr24Scalar(src_row + offset, dst_row + offset, width - x, 1,
                       src_stride, dst_stride);
    }
  }
}

} // namespace studiocast::video::internal
